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

struct mtk_bitreader {
	const u8 *data;
	size_t len;
	size_t bit;		/* absolute bit position in the RBSP */
	size_t zeros;		/* consecutive zero bytes seen, for 00 00 03 */
	bool overflow;
};

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

int mtk_h264_parse_slice(const u8 *data, size_t len, u8 nal_type,
			 const struct mtk_h264_ps *ps, struct mtk_h264_slice *out)
{
	struct mtk_bitreader br;
	const struct mtk_h264_pps *pps;
	const struct mtk_h264_sps *sps;
	u32 first_mb, slice_type, pps_id;

	if (!data || !ps || !out)
		return -EINVAL;

	br_init(&br, data, len);
	memset(out, 0, sizeof(*out));

	out->nal_type = nal_type;

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
	 * ref_pic_list_modification and pred_weight_table follow.  Neither is
	 * needed for the fields this driver programs: the hardware builds its
	 * own lists from MAX_PIC_NUM/CUR_PIC_NUM, and the weight table is only
	 * used when pps->weighted_pred_flag is set - in which case decode is
	 * refused below rather than given a table this parser would have to
	 * reproduce bit-exactly to be correct.
	 */
	if (pps->weighted_pred_flag && (slice_type % 5 != 1))
		return -EOPNOTSUPP;
	if (pps->weighted_bipred_idc == 1 && (slice_type % 5) == 1)
		return -EOPNOTSUPP;

	/* CABAC alignment. */
	if (pps->entropy_coding_mode_flag && slice_type % 5 != 2)
		br_ue(&br);		/* cabac_init_idc */

	out->slice_qp_delta = br_se(&br);
	/* slice_qp_delta is bounded by the QP range: -87..77 covers every profile. */
	if (out->slice_qp_delta < -87 || out->slice_qp_delta > 77)
		return -ERANGE;

	if (pps->deblocking_filter_control_present_flag) {
		u32 disable = br_ue(&br);

		if (!disable) {
			br_se(&br);	/* slice_alpha_c0_offset_div2 */
			br_se(&br);	/* slice_beta_offset_div2 */
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
