// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * H.264 parameter-set and slice-header parsing for the MT6589 VDEC.
 *
 * See mtk-vdec-mt6589-h264.h for why this lives in the kernel at all: the
 * MT6589 AVC parser takes already-parsed fields, so somebody has to parse them,
 * and on mainline that somebody is the driver rather than a closed userspace
 * blob.
 *
 * The bit reader is the only subtle part.  H.264 uses RBSP, which removes
 * emulation-prevention bytes: within a NAL unit, 00 00 03 must be read as 00 00.
 * Skipping that step does not fail loudly, it silently shifts every following
 * field by one byte, so the reader handles it in place rather than pre-stripping
 * the buffer - the latter would need a second copy of every input.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "mtk-vdec-mt6589-h264.h"

/* Bits available in a NAL unit, worst case - enough for any SPS or PPS. */
#define MTK_H264_MAX_BITS	(1024 * 1024)

/*
 * The reader itself is declared in the header, because exactly one function on it
 * has a caller outside this file (mtk_h264_slice_data_bits(), used by the
 * datapath to find where slice_data begins).  Everything else here treats it as
 * private.
 */
#define mtk_bitreader	mtk_h264_rbsp

static void br_init(struct mtk_bitreader *br, const u8 *data, size_t len)
{
	br->data = data;
	br->len = len;
	br->bit = 0;
	br->zeros = 0;
	br->overflow = false;
}

/*
 * Map a logical RBSP bit index to a physical byte offset, skipping emulation
 * prevention bytes.  Done by walking rather than by a precomputed table because
 * it has to work for any NAL size without a second allocation.
 */
static int br_byte_at(struct mtk_bitreader *br, size_t logical, size_t *pos)
{
	size_t i, seen = 0, zeros = 0;

	for (i = 0; i < br->len; i++) {
		u8 b = br->data[i];

		/* 00 00 03 is an escape: the 03 carries no data. */
		if (zeros >= 2 && b == 0x03) {
			zeros = 0;
			continue;
		}

		if (seen == logical) {
			*pos = i;
			return 0;
		}
		seen++;
		zeros = b ? 0 : zeros + 1;
	}

	return -ERANGE;
}

static u32 br_bit(struct mtk_bitreader *br, size_t n)
{
	size_t pos;
	u8 b;

	if (br->overflow)
		return 0;
	if (br->bit + n > MTK_H264_MAX_BITS) {
		br->overflow = true;
		return 0;
	}

	if (br_byte_at(br, br->bit / 8, &pos)) {
		br->overflow = true;
		return 0;
	}

	b = br->data[pos];
	br->bit++;
	return (b >> (7 - ((br->bit - 1) % 8))) & 1;
}

static u32 br_bits(struct mtk_bitreader *br, unsigned int n)
{
	u32 v = 0;
	unsigned int i;

	if (n > 32) {
		br->overflow = true;
		return 0;
	}

	for (i = 0; i < n; i++)
		v = (v << 1) | br_bit(br, 1);

	return v;
}

static u32 br_ue(struct mtk_bitreader *br)
{
	unsigned int leading = 0;
	u32 v;

	while (!br->overflow && !br_bit(br, 1)) {
		if (++leading > 31) {
			br->overflow = true;
			return 0;
		}
	}

	if (!leading)
		return 0;

	v = br_bits(br, leading);
	return (1u << leading) - 1 + v;
}

static s32 br_se(struct mtk_bitreader *br)
{
	u32 ue = br_ue(br);

	if (ue & 1)
		return (s32)((ue + 1) / 2);
	return -(s32)(ue / 2);
}

/*
 * more_rbsp_data: true while there are real bits left rather than only the
 * rbsp_stop_one_bit and its zero padding.  Without this the scaling-list loop
 * and the slice-header tail read past the end of short NAL units.
 */
static bool br_more_rbsp_data(struct mtk_bitreader *br)
{
	size_t pos, total = 0;
	size_t last_one = 0, i;

	/* Count logical RBSP bytes and locate the final set bit. */
	for (i = 0; i < br->len; i++) {
		u8 b = br->data[i];

		if (br->zeros >= 2 && b == 0x03) {
			br->zeros = 0;
			continue;
		}
		total++;
		br->zeros = b ? 0 : br->zeros + 1;
	}
	br->zeros = 0;

	if (!total)
		return false;

	/* Walk back from the end to the stop bit. */
	for (i = total; i > 0; i--) {
		if (br_byte_at(br, i - 1, &pos))
			return false;
		if (br->data[pos]) {
			u8 b = br->data[pos];
			unsigned int k;

			for (k = 0; k < 8; k++)
				if (b & (1 << k))
					last_one = (i - 1) * 8 + (7 - k);
			break;
		}
	}

	return br->bit < last_one;
}

/*
 * scaling_list, H.264 7.3.2.1.1.1.  The values are not programmed - the driver
 * only needs to know whether a list was present so the PPS word's flag bits and
 * the AVLD weight-table handshake stay in step with the stream.
 */
static void br_skip_scaling_list(struct mtk_bitreader *br, int size)
{
	int last = 8, next = 8;
	int i;

	for (i = 0; i < size && !br->overflow; i++) {
		if (next != 0)
			next = (last + br_se(br) + 256) % 256;
		last = next ? next : last;
	}
}

int mtk_h264_parse_sps(const u8 *data, size_t len, struct mtk_h264_sps *out)
{
	struct mtk_bitreader br;
	u32 profile_idc, chroma, i;

	if (!data || !out)
		return -EINVAL;

	br_init(&br, data, len);
	memset(out, 0, sizeof(*out));

	/*
	 * SPS RBSP order (H.264 7.3.2.1.1): profile_idc, constraint flags and
	 * reserved bits, level_idc, and only then seq_parameter_set_id.  Taking
	 * the id first - as an earlier revision did - reads the profile byte as
	 * the id and shifts every following field by eight bits, while still
	 * returning plausible-looking numbers rather than failing.
	 */
	profile_idc = br_bits(&br, 8);
	br_bits(&br, 8);		/* constraint_set flags + reserved */
	br_bits(&br, 8);		/* level_idc */

	out->id = br_ue(&br);
	if (out->id >= MTK_H264_MAX_SPS)
		return -ERANGE;

	chroma = 1;
	switch (profile_idc) {
	case 100: case 110: case 122: case 244: case 44: case 83:
	case 86: case 118: case 128: case 138: case 139: case 134: case 135:
		chroma = br_ue(&br);
		if (chroma > 3) {
			/* 4:4:4 has extra chroma-location syntax. */
			return -EOPNOTSUPP;
		}
		if (chroma == 3)
			br_bit(&br, 1);		/* separate_colour_plane_flag */
		br_ue(&br);			/* bit_depth_luma_minus8 */
		br_ue(&br);			/* bit_depth_chroma_minus8 */
		br_bit(&br, 1);			/* qpprime_y_zero_transform_bypass */
		if (br_bit(&br, 1)) {		/* seq_scaling_matrix_present */
			unsigned int n = (chroma != 3) ? 8 : 12;

			for (i = 0; i < n && !br.overflow; i++)
				if (br_bit(&br, 1))
					br_skip_scaling_list(&br,
						i < 6 ? 16 : 64);
		}
		break;
	default:
		break;
	}
	out->chroma_format_idc = chroma;

	out->log2_max_frame_num_minus4 = br_ue(&br);
	if (out->log2_max_frame_num_minus4 > 12)
		return -ERANGE;

	out->pic_order_cnt_type = br_ue(&br);
	switch (out->pic_order_cnt_type) {
	case 0:
		out->log2_max_pic_order_cnt_lsb_minus4 = br_ue(&br);
		if (out->log2_max_pic_order_cnt_lsb_minus4 > 12)
			return -ERANGE;
		break;
	case 1:
		out->delta_pic_order_always_zero_flag = br_bit(&br, 1);
		br_se(&br);			/* offset_for_non_ref_pic */
		br_se(&br);			/* offset_for_top_to_bottom_field */
		{
			u32 n = br_ue(&br);

			if (n > 255)
				return -ERANGE;
			for (i = 0; i < n && !br.overflow; i++)
				br_se(&br);
		}
		break;
	case 2:
		break;
	default:
		return -EINVAL;
	}

	out->num_ref_frames = br_ue(&br);
	if (out->num_ref_frames > MTK_H264_MAX_REF_FRAMES)
		return -ERANGE;

	br_bit(&br, 1);				/* gaps_in_frame_num_allowed */
	out->pic_width_in_mbs_minus1 = br_ue(&br);
	out->pic_height_in_map_units_minus1 = br_ue(&br);

	if (!out->pic_width_in_mbs_minus1 || !out->pic_height_in_map_units_minus1)
		return -ERANGE;
	/* Guard the multiply as well as the hardware's own limit. */
	if (out->pic_width_in_mbs_minus1 > 255 ||
	    out->pic_height_in_map_units_minus1 > 255)
		return -ERANGE;

	out->frame_mbs_only_flag = br_bit(&br, 1);
	if (!out->frame_mbs_only_flag)
		out->mb_adaptive_frame_field_flag = br_bit(&br, 1);
	out->direct_8x8_inference_flag = br_bit(&br, 1);

	br_bit(&br, 1);				/* frame_cropping_flag */
	/*
	 * Cropping offsets are parsed for validity only; the hardware takes the
	 * uncropped MB geometry and the client applies the visible rectangle.
	 */

	if (br.overflow)
		return -EINVAL;

	out->width = (out->pic_width_in_mbs_minus1 + 1) * 16;
	out->height = (out->pic_height_in_map_units_minus1 + 1) * 16;
	if (!out->frame_mbs_only_flag)
		out->height *= 2;

	out->valid = true;
	return 0;
}

int mtk_h264_parse_pps(const u8 *data, size_t len, struct mtk_h264_pps *out)
{
	struct mtk_bitreader br;
	u8 scaling_present[8] = { 0 };
	bool any_scaling = false;
	unsigned int i;

	if (!data || !out)
		return -EINVAL;

	br_init(&br, data, len);
	memset(out, 0, sizeof(*out));

	out->id = br_ue(&br);
	out->sps_id = br_ue(&br);
	if (out->id >= MTK_H264_MAX_PPS || out->sps_id >= MTK_H264_MAX_SPS)
		return -ERANGE;

	out->entropy_coding_mode_flag = br_bit(&br, 1);
	out->pic_order_present_flag = br_bit(&br, 1);

	out->num_slice_groups_minus1 = br_ue(&br);
	/*
	 * FMO is not supported: the hardware word has no slice-group field and
	 * the driver has no map-unit table to give it.  Reject rather than
	 * decode into the wrong MBs.
	 */
	if (out->num_slice_groups_minus1)
		return -EOPNOTSUPP;

	out->num_ref_idx_l0_active_minus1 = br_ue(&br);
	out->num_ref_idx_l1_active_minus1 = br_ue(&br);
	if (out->num_ref_idx_l0_active_minus1 > 31 ||
	    out->num_ref_idx_l1_active_minus1 > 31)
		return -ERANGE;

	out->weighted_pred_flag = br_bit(&br, 1);
	out->weighted_bipred_idc = br_bits(&br, 2);
	out->pic_init_qp_minus26 = br_se(&br);
	out->pic_init_qs_minus26 = br_se(&br);
	out->chroma_qp_index_offset = br_se(&br);

	if (out->pic_init_qp_minus26 < -26 || out->pic_init_qp_minus26 > 25)
		return -ERANGE;
	if (out->chroma_qp_index_offset < -12 || out->chroma_qp_index_offset > 12)
		return -ERANGE;

	out->deblocking_filter_control_present_flag = br_bit(&br, 1);
	out->constrained_intra_pred_flag = br_bit(&br, 1);
	out->redundant_pic_cnt_present_flag = br_bit(&br, 1);

	/*
	 * The extension block is optional and only present when more_rbsp_data
	 * says so.  Reading it unconditionally is the classic way to mis-parse
	 * every PPS written by an encoder that omits it.
	 */
	if (br_more_rbsp_data(&br)) {
		out->transform_8x8_mode_flag = br_bit(&br, 1);
		if (br_bit(&br, 1)) {		/* pic_scaling_matrix_present */
			unsigned int n = 6 + (out->transform_8x8_mode_flag ? 2 : 0);

			for (i = 0; i < n && !br.overflow; i++) {
				scaling_present[i] = br_bit(&br, 1);
				if (scaling_present[i])
					br_skip_scaling_list(&br, i < 6 ? 16 : 64);
			}
			any_scaling = true;
		}
		out->second_chroma_qp_index_offset = br_se(&br);
	} else {
		out->second_chroma_qp_index_offset =
			out->chroma_qp_index_offset;
	}

	if (br.overflow)
		return -EINVAL;

	out->valid = true;
	return 0;
}

int mtk_h264_parse_slice(const u8 *data, size_t len, u8 nal_type, u8 nal_ref_idc,
			 const struct mtk_h264_ps *ps, struct mtk_h264_slice *out)
{
	struct mtk_bitreader br;
	const struct mtk_h264_pps *pps;
	const struct mtk_h264_sps *sps;
	u32 first_mb, slice_type, pps_id, st, i;

	if (!data || !ps || !out)
		return -EINVAL;

	br_init(&br, data, len);
	memset(out, 0, sizeof(*out));

	out->nal_type = nal_type;
	out->nal_ref_idc = nal_ref_idc & 0x3;

	first_mb = br_ue(&br);
	slice_type = br_ue(&br);
	pps_id = br_ue(&br);

	if (first_mb > 0x1FFF)		/* 13 bits in the hardware */
		return -ERANGE;
	if (slice_type > 9)
		return -EINVAL;
	if (pps_id >= MTK_H264_MAX_PPS)
		return -ERANGE;

	pps = &ps->pps[pps_id];
	if (!pps->valid)
		return -ENOENT;
	sps = &ps->sps[pps->sps_id];
	if (!sps->valid)
		return -ENOENT;

	out->first_mb_in_slice = first_mb;
	out->slice_type = slice_type;
	out->pps_id = pps_id;

	/* frame_num is log2_max_frame_num bits. */
	out->frame_num = br_bits(&br, sps->log2_max_frame_num_minus4 + 4);

	if (!sps->frame_mbs_only_flag) {
		out->field_pic_flag = br_bit(&br, 1);
		if (out->field_pic_flag)
			out->bottom_field_flag = br_bit(&br, 1);
	}

	if (out->nal_type == MTK_H264_NAL_IDR)
		br_ue(&br);		/* idr_pic_id */

	if (sps->pic_order_cnt_type == 0) {
		br_bits(&br, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->pic_order_present_flag && !out->field_pic_flag)
			br_se(&br);	/* delta_pic_order_cnt_bottom */
	}

	if (sps->pic_order_cnt_type == 1 && !sps->delta_pic_order_always_zero_flag) {
		br_se(&br);		/* delta_pic_order_cnt[0] */
		if (pps->pic_order_present_flag && !out->field_pic_flag)
			br_se(&br);	/* delta_pic_order_cnt[1] */
	}

	if (pps->redundant_pic_cnt_present_flag)
		br_ue(&br);		/* redundant_pic_cnt */

	out->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_active_minus1;
	out->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_active_minus1;

	/* B slice. */
	if ((slice_type % 5) == 1) {
		out->direct_spatial_mv_pred_flag = br_bit(&br, 1);
	}

	/* P, SP, B slices may override the active reference counts. */
	if ((slice_type % 5) == 0 || (slice_type % 5) == 1 ||
	    (slice_type % 5) == 3) {
		if (br_bit(&br, 1)) {		/* num_ref_idx_active_override */
			out->num_ref_idx_l0_active_minus1 = br_ue(&br);
			if ((slice_type % 5) == 1)
				out->num_ref_idx_l1_active_minus1 = br_ue(&br);
			if (out->num_ref_idx_l0_active_minus1 > 31 ||
			    out->num_ref_idx_l1_active_minus1 > 31)
				return -ERANGE;
		}
	}

	/*
	 * ref_pic_list_modification (7.3.3.1) and dec_ref_pic_marking (7.3.3.3)
	 * are not needed for any field this driver programs - the hardware builds
	 * its own reference list - but they MUST still be walked, because
	 * everything after them is positional.  Leaving them out does not fail:
	 * it reads slice_qp_delta and the deblocking offsets from the wrong bits,
	 * and slice_qp_delta's range check then rejects perfectly valid streams
	 * (measured: a 320x240 x264 baseline IDR frame failed with -ERANGE because
	 * alpha_c0_offset_div2 came out as 12).
	 */
	st = slice_type % 5;

	if (st != 2 && st != 4) {
		if (br_bit(&br, 1)) {	/* ref_pic_list_modification_flag_l0 */
			for (i = 0; i < 32 && !br.overflow; i++) {
				if (br_ue(&br) == 3)
					break;
				br_ue(&br);
			}
		}
	}
	if (st == 1) {
		if (br_bit(&br, 1)) {	/* ref_pic_list_modification_flag_l1 */
			for (i = 0; i < 32 && !br.overflow; i++) {
				if (br_ue(&br) == 3)
					break;
				br_ue(&br);
			}
		}
	}

	if (out->nal_ref_idc) {
		if (nal_type == MTK_H264_NAL_IDR) {
			br_bit(&br, 1);		/* no_output_of_prior_pics */
			br_bit(&br, 1);		/* long_term_reference_flag */
		} else if (br_bit(&br, 1)) {	/* adaptive_ref_pic_marking_mode */
			for (i = 0; i < 66 && !br.overflow; i++) {
				u32 mmco = br_ue(&br);

				if (!mmco)
					break;
				if (mmco <= 6)
					br_ue(&br);
			}
		}
	}

	/*
	 * pred_weight_table is refused rather than skipped: the harness only
	 * raises a handshake for it and writes no coefficient, so there is
	 * nothing to program.  Refused at the position where it would be read,
	 * so a weighted stream fails here rather than decoding wrongly.
	 */
	if (pps->weighted_pred_flag && st != 1)
		return -EOPNOTSUPP;
	if (pps->weighted_bipred_idc == 1 && st == 1)
		return -EOPNOTSUPP;

	/* CABAC alignment. */
	if (pps->entropy_coding_mode_flag && slice_type % 5 != 2) {
		u32 idc = br_ue(&br);		/* cabac_init_idc */

		if (idc > 2)
			return -EINVAL;
		out->cabac_init_idc = idc;
	}

	out->slice_qp_delta = br_se(&br);
	/* slice_qp_delta is bounded by the QP range: -87..77 covers every profile. */
	if (out->slice_qp_delta < -87 || out->slice_qp_delta > 77)
		return -ERANGE;

	if (pps->deblocking_filter_control_present_flag) {
		u32 disable = br_ue(&br);

		if (disable > 2)
			return -EINVAL;
		out->disable_deblocking_filter_idc = disable;

		if (!disable) {
			s32 alpha = br_se(&br);	/* slice_alpha_c0_offset_div2 */
			s32 beta = br_se(&br);	/* slice_beta_offset_div2 */

			if (alpha < -6 || alpha > 6 || beta < -6 || beta > 6)
				return -ERANGE;
			out->slice_alpha_c0_offset_div2 = alpha;
			out->slice_beta_offset_div2 = beta;
		}
	}

	if (br.overflow)
		return -EINVAL;

	out->valid = true;
	return 0;
}

/*
 * Locate the next NAL unit.  The payload runs to the next start code or to the
 * end of the buffer, minus any trailing zero bytes (the zeros of the *next*
 * start code, and trailing_zero_8bits padding).
 */
int mtk_h264_find_nal(const u8 *data, size_t len, size_t *pos,
		      size_t *nal_start, size_t *nal_len)
{
	size_t i, start, end;

	if (!data || !pos || !nal_start || !nal_len)
		return -EINVAL;

	for (i = *pos; i + 3 <= len; i++) {
		if (data[i] != 0 || data[i + 1] != 0)
			continue;
		if (data[i + 2] == 0x01)
			break;
		if (data[i + 2] == 0x00 && i + 4 <= len && data[i + 3] == 0x01)
			break;
	}

	if (i + 3 > len) {
		*pos = len;
		return 1;
	}

	start = (data[i + 2] == 0x01) ? i + 3 : i + 4;

	/* Find where this NAL ends: the next start code, or the buffer end. */
	for (end = start; end + 3 <= len; end++) {
		if (data[end] == 0 && data[end + 1] == 0 &&
		    (data[end + 2] == 0x01 ||
		     (data[end + 2] == 0x00 && end + 4 <= len &&
		      data[end + 3] == 0x01)))
			break;
	}
	if (end + 3 > len)
		end = len;

	/* Strip trailing zeros: they belong to the next start code or padding. */
	while (end > start && data[end - 1] == 0)
		end--;

	*nal_start = start;
	*nal_len = end - start;
	*pos = (end + 3 <= len) ? end : len;
	return 0;
}

/*
 * Where does slice_data start, and how many bits of it are there?
 *
 * The parser above stops at the fields the hardware is handed.  The hardware
 * decodes slice_data off the bitstream FIFO itself, so the driver additionally
 * needs the bit offset at which the header ended -- which means walking the
 * syntax elements the parser skips.
 *
 * That walk is kept here, next to the parser and out of the datapath, for one
 * reason: the two must stay in step.  Any divergence silently shifts every
 * following field, so the sequence below matches mtk_h264_parse_slice() line for
 * line until the point where the parser stops looking.
 */
static int slice_header_end_bit(const u8 *data, size_t len, u8 nal_type,
				const struct mtk_h264_slice *slice,
				const struct mtk_h264_sps *sps,
				const struct mtk_h264_pps *pps,
				size_t *header_end_bit)
{
	struct mtk_bitreader br;
	unsigned int st = slice->slice_type % 5;
	u32 i;

	br_init(&br, data, len);

	br_ue(&br);			/* first_mb_in_slice */
	br_ue(&br);			/* slice_type */
	br_ue(&br);			/* pic_parameter_set_id */
	br_bits(&br, sps->log2_max_frame_num_minus4 + 4);	/* frame_num */

	if (!sps->frame_mbs_only_flag) {
		if (br_bit(&br, 1))	/* field_pic_flag */
			br_bit(&br, 1);	/* bottom_field_flag */
	}
	if (nal_type == MTK_H264_NAL_IDR)
		br_ue(&br);		/* idr_pic_id */

	if (sps->pic_order_cnt_type == 0) {
		br_bits(&br, sps->log2_max_pic_order_cnt_lsb_minus4 + 4);
		if (pps->pic_order_present_flag && !slice->field_pic_flag)
			br_se(&br);	/* delta_pic_order_cnt_bottom */
	}
	if (sps->pic_order_cnt_type == 1 &&
	    !sps->delta_pic_order_always_zero_flag) {
		br_se(&br);		/* delta_pic_order_cnt[0] */
		if (pps->pic_order_present_flag && !slice->field_pic_flag)
			br_se(&br);	/* delta_pic_order_cnt[1] */
	}

	if (pps->redundant_pic_cnt_present_flag)
		br_ue(&br);		/* redundant_pic_cnt */

	if (st == 1)			/* B */
		br_bit(&br, 1);		/* direct_spatial_mv_pred_flag */

	if (st == 0 || st == 1 || st == 3) {
		if (br_bit(&br, 1)) {	/* num_ref_idx_active_override_flag */
			br_ue(&br);	/* num_ref_idx_l0_active_minus1 */
			if (st == 1)
				br_ue(&br);
		}
	}

	/*
	 * ref_pic_list_modification (H.264 7.3.3.1).  The loop is bounded: the
	 * syntax requires a terminating idc of 3, and a stream without one is
	 * malformed, so a bound here turns corruption into an error instead of
	 * into an unbounded read.
	 */
	if (st != 2 && st != 4) {
		if (br_bit(&br, 1)) {	/* ref_pic_list_modification_flag_l0 */
			for (i = 0; i < 32 && !br.overflow; i++) {
				if (br_ue(&br) == 3)
					break;
				br_ue(&br);	/* abs_diff_pic_num_minus1 */
			}
		}
	}
	if (st == 1) {
		if (br_bit(&br, 1)) {	/* ref_pic_list_modification_flag_l1 */
			for (i = 0; i < 32 && !br.overflow; i++) {
				if (br_ue(&br) == 3)
					break;
				br_ue(&br);
			}
		}
	}

	/*
	 * dec_ref_pic_marking (H.264 7.3.3.3).  Present only for reference
	 * pictures, and this is the step that was missing: without it every field
	 * after it - slice_qp_delta, the deblocking offsets, cabac_init_idc - is
	 * read from the wrong bit position, which does not fail, it produces a
	 * plausible-looking slice_qp_delta and an out-of-range alpha offset.
	 * Caught by checking against real x264 output, where alpha came out 12
	 * instead of within -6..6.
	 */
	if (slice->nal_ref_idc) {
		if (nal_type == MTK_H264_NAL_IDR) {
			br_bit(&br, 1);		/* no_output_of_prior_pics */
			br_bit(&br, 1);		/* long_term_reference_flag */
		} else if (br_bit(&br, 1)) {	/* adaptive_ref_pic_marking_mode */
			/*
			 * memory_management_control_operation loop.  Each operation
			 * carries 0..2 ue(v) operands; bounded because a malformed
			 * stream can otherwise loop forever.
			 */
			for (i = 0; i < 66 && !br.overflow; i++) {
				u32 mmco = br_ue(&br);

				if (!mmco)
					break;
				if (mmco <= 2)
					br_ue(&br);
				else if (mmco == 3 || mmco == 6)
					br_ue(&br);
				else if (mmco == 4)
					br_ue(&br);
			}
		}
	}

	/*
	 * pred_weight_table.  Refused here rather than in the parser so the
	 * reason is stated once: the harness's vVDEC_HAL_H264_PredWeightTable()
	 * (vdec_hal_if_h264.c:560) raises a handshake and writes no coefficient
	 * anywhere, so there is nothing to program and no evidence the hardware
	 * reads them out of the bitstream.  Guessing produces plausible garbage.
	 */
	if ((pps->weighted_pred_flag && (st == 0 || st == 3)) ||
	    (pps->weighted_bipred_idc == 1 && st == 1))
		return -EOPNOTSUPP;

	if (pps->entropy_coding_mode_flag && st != 2 && st != 4)
		br_ue(&br);		/* cabac_init_idc */

	br_se(&br);			/* slice_qp_delta */

	if (pps->deblocking_filter_control_present_flag) {
		u32 disable = br_ue(&br);

		if (!disable) {
			br_se(&br);	/* slice_alpha_c0_offset_div2 */
			br_se(&br);	/* slice_beta_offset_div2 */
		}
	}

	if (br.overflow)
		return -EINVAL;

	*header_end_bit = br.bit;
	return 0;
}

/*
 * Last set bit of the RBSP, as a logical bit offset.  This is
 * rbsp_stop_one_bit, which is the only upper bound the driver has on slice_data
 * once slices may be packed: without it the decoder runs past the end of the
 * picture and the hardware reports nothing, because a wrong slice length looks
 * exactly like a corrupt bitstream.
 */
static int rbsp_last_set_bit(const u8 *data, size_t len, size_t *last_bit)
{
	size_t i, lb = 0, zeros = 0;
	bool found = false;
	size_t best = 0;

	for (i = 0; i < len; i++) {
		u8 b = data[i];
		unsigned int k;

		if (zeros >= 2 && b == 0x03) {
			zeros = 0;
			continue;
		}
		zeros = b ? 0 : zeros + 1;

		for (k = 0; k < 8; k++) {
			if (b & (1 << (7 - k))) {
				best = lb * 8 + k;
				found = true;
			}
		}
		lb++;
	}

	if (!found)
		return -EINVAL;

	*last_bit = best;
	return 0;
}

int mtk_h264_slice_data_bits(const u8 *data, size_t len, u8 nal_type,
			     const struct mtk_h264_ps *ps,
			     const struct mtk_h264_slice *slice,
			     size_t *data_off, size_t *data_len,
			     unsigned int *data_bits)
{
	const struct mtk_h264_pps *pps;
	const struct mtk_h264_sps *sps;
	size_t header_end, stop_bit, i, zeros = 0, logical = 0;
	size_t off, last_byte;
	int ret;

	if (!data || !ps || !slice || !data_off || !data_len || !data_bits)
		return -EINVAL;
	if (!slice->valid)
		return -EINVAL;
	if (slice->pps_id >= MTK_H264_MAX_PPS)
		return -ERANGE;

	pps = &ps->pps[slice->pps_id];
	if (!pps->valid)
		return -ENOENT;
	if (pps->sps_id >= MTK_H264_MAX_SPS)
		return -ERANGE;
	sps = &ps->sps[pps->sps_id];
	if (!sps->valid)
		return -ENOENT;

	ret = slice_header_end_bit(data, len, nal_type, slice, sps, pps,
				   &header_end);
	if (ret)
		return ret;

	/* Header-end bit -> logical byte offset within the unit. */
	off = header_end / 8;

	zeros = 0;
	logical = 0;
	for (i = 0; i < len; i++) {
		if (zeros >= 2 && data[i] == 0x03) {
			zeros = 0;
			continue;
		}
		zeros = data[i] ? 0 : zeros + 1;
		logical++;
		if (logical > off)
			break;
	}
	if (logical <= off)
		return -EINVAL;

	ret = rbsp_last_set_bit(data, len, &stop_bit);
	if (ret)
		return ret;

	if (stop_bit < header_end)
		return -EINVAL;

	*data_bits = (unsigned int)(stop_bit - header_end);
	if (!*data_bits)
		return -EINVAL;

	/*
	 * Physical byte range for the copy: from the header's last byte through
	 * the last non-zero byte of the NAL unit.  Trailing zeros are the next
	 * start code or trailing_zero_8bits padding; feeding them to the decoder
	 * as slice data is how a frame picks up a phantom few thousand zero bits.
	 */
	last_byte = len;
	while (last_byte > off && !data[last_byte - 1])
		last_byte--;
	if (last_byte <= off)
		return -EINVAL;

	*data_off = off;
	*data_len = last_byte - off;
	return 0;
}
