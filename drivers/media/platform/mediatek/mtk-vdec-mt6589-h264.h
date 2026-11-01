/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * H.264 bitstream parsing for the MT6589 VDEC.
 *
 * The MT6589 AVC parser does not read the bitstream itself: the caller parses
 * the parameter sets and slice headers in software and hands the parsed fields
 * over through VDEC_AVC_VLD_* registers.  That is what the vendor does, and it is
 * what this file does - see the field packing documented in
 * mtk-vcodec-mt6589-reg.h, which was transcribed from the vendor LDVT harness
 * (hal/vdec_hal_if_h264.c).
 *
 * Scope: exactly the fields those registers take.  This is not a general H.264
 * parser - no macroblock layer, no CABAC, no reference picture list
 * construction beyond the counts the hardware needs, no SEI.
 *
 * Every bounds check here exists because the input is an untrusted bitstream
 * from userspace.  A parser that trusts it will run off the end of a buffer on
 * the first malformed frame it meets.
 */

#ifndef _MTK_VDEC_MT6589_H264_H
#define _MTK_VDEC_MT6589_H264_H

#include <linux/bits.h>
#include <linux/types.h>

/* NAL unit types we care about (ITU-T H.264 table 7-1). */
#define MTK_H264_NAL_SLICE		1
#define MTK_H264_NAL_IDR		5
#define MTK_H264_NAL_SEI		6
#define MTK_H264_NAL_SPS		7
#define MTK_H264_NAL_PPS		8
#define MTK_H264_NAL_AUD		9

#define MTK_H264_MAX_SPS		32
#define MTK_H264_MAX_PPS		256
#define MTK_H264_MAX_REF_FRAMES		16

/*
 * Largest picture this decoder can be pointed at.
 *
 * The datapath allocates one 1920x1088 buffer per reference picture and the
 * hardware writes a full macroblock grid into it, so these are hard safety
 * limits, not just a capability statement: a larger geometry makes the writeback
 * run off the end of the allocation.  Keep MTK_VDEC_PIC_W/MTK_VDEC_PIC_H in
 * mtk-vdec-mt6589-dp.h in step with these.
 */
#define MTK_H264_MAX_MB_WIDTH		120	/* 1920 / 16 */
#define MTK_H264_MAX_MB_HEIGHT		68	/* 1088 / 16 */

/**
 * struct mtk_h264_sps - the SPS fields the hardware is given.
 * @valid: set once parsed; the driver refuses a slice without it.
 * @id: seq_parameter_set_id.
 * @chroma_format_idc: 0=mono, 1=4:2:0, 2=4:2:2, 3=4:4:4. Hardware takes 2 bits.
 * @log2_max_frame_num_minus4: 0..12.
 * @pic_order_cnt_type: 0, 1 or 2.
 * @log2_max_pic_order_cnt_lsb_minus4: 0..12, type 0 only.
 * @delta_pic_order_always_zero_flag: type 1 only.
 * @num_ref_frames: 0..16; hardware takes 5 bits.
 * @frame_mbs_only_flag: 0 means the stream may be field-coded.
 * @mb_adaptive_frame_field_flag: MBAFF.
 * @direct_8x8_inference_flag: required by the hardware word.
 * @pic_width_in_mbs_minus1: from pic_width_in_mbs.
 * @pic_height_in_map_units_minus1: map units, not frame MBs.
 * @width: pic_width_in_mbs * 16.
 * @height: (map_units * 16) << (1 - frame_mbs_only_flag), i.e. frame height.
 */
struct mtk_h264_sps {
	bool valid;
	u8 id;
	u8 chroma_format_idc;
	u8 log2_max_frame_num_minus4;
	u8 pic_order_cnt_type;
	u8 log2_max_pic_order_cnt_lsb_minus4;
	bool delta_pic_order_always_zero_flag;
	u8 num_ref_frames;
	bool frame_mbs_only_flag;
	bool mb_adaptive_frame_field_flag;
	bool direct_8x8_inference_flag;
	u16 pic_width_in_mbs_minus1;
	u16 pic_height_in_map_units_minus1;
	unsigned int width;
	unsigned int height;
};

/**
 * struct mtk_h264_pps - the PPS fields the hardware is given.
 * @valid: set once parsed.
 * @id: pic_parameter_set_id.
 * @sps_id: seq_parameter_set_id this PPS refers to.
 * @entropy_coding_mode_flag: 0 = CAVLC, 1 = CABAC.
 * @pic_order_present_flag: bottom_field_pic_order_in_frame_present_flag.
 * @num_slice_groups_minus1: kept only to reject FMO, which this driver does not
 *	handle and the hardware word has no field for.
 * @num_ref_idx_l0_active_minus1: 5 bits in the hardware.
 * @num_ref_idx_l1_active_minus1: 5 bits in the hardware.
 * @weighted_pred_flag: P/SP weighting.
 * @weighted_bipred_idc: 0..3.
 * @pic_init_qp_minus26: signed, 6 bits in the hardware.
 * @pic_init_qs_minus26: parsed for stream validity, not programmed.
 * @chroma_qp_index_offset: signed, 5 bits.
 * @deblocking_filter_control_present_flag: decides whether the slice header
 *	carries the filter fields.
 * @constrained_intra_pred_flag: programmed.
 * @redundant_pic_cnt_present_flag: decides whether the slice header carries
 *	redundant_pic_cnt.
 * @transform_8x8_mode_flag: programmed.
 * @second_chroma_qp_index_offset: signed, 5 bits.
 */
struct mtk_h264_pps {
	bool valid;
	u8 id;
	u8 sps_id;
	bool entropy_coding_mode_flag;
	bool pic_order_present_flag;
	u8 num_slice_groups_minus1;
	u8 num_ref_idx_l0_active_minus1;
	u8 num_ref_idx_l1_active_minus1;
	bool weighted_pred_flag;
	u8 weighted_bipred_idc;
	s8 pic_init_qp_minus26;
	s8 pic_init_qs_minus26;
	s8 chroma_qp_index_offset;
	bool deblocking_filter_control_present_flag;
	bool constrained_intra_pred_flag;
	bool redundant_pic_cnt_present_flag;
	bool transform_8x8_mode_flag;
	s8 second_chroma_qp_index_offset;
};

/**
 * struct mtk_h264_slice - the slice header fields the hardware is given.
 * @valid: set once parsed.
 * @nal_type: 1 (non-IDR) or 5 (IDR).
 * @slice_type: 0..9 as coded; 5 is added for "all slices in picture same type".
 * @first_mb_in_slice: 13 bits in the hardware.
 * @pps_id: the PPS this slice uses.
 * @frame_num: modulo max_frame_num.
 * @field_pic_flag: from the stream, only legal when !frame_mbs_only.
 * @bottom_field_flag: from the stream.
 * @direct_spatial_mv_pred_flag: B slices only.
 * @num_ref_idx_l0_active_minus1: slice override, or the PPS value.
 * @num_ref_idx_l1_active_minus1: slice override, or the PPS value.
 * @slice_qp_delta: signed; SHDRAVLD2[6:0].
 * @nal_ref_idc: from the NAL unit header byte, not the slice header.  Carried
 *	here because SHDRAVLD2[18:17] needs it and the NAL header is a long way
 *	from the register write.  0 means "not a reference picture".
 * @disable_deblocking_filter_idc: 0..2.  Only present when the PPS has
 *	deblocking_filter_control_present_flag; the default of 0 applies otherwise.
 * @slice_alpha_c0_offset_div2: signed, -6..6, SHDRAVLD2[12:9].
 * @slice_beta_offset_div2: signed, -6..6, SHDRAVLD2[16:13].
 * @cabac_init_idc: 0..2, SHDRAVLD2[21:20].  Meaningful only for CABAC.
 *
 * The last five fields are recorded because the AVC_VLD block needs them in
 * SHDRAVLD2 even though they play no part in the fields the parser already
 * extracted: the block reads the slice *data* itself, but the deblocking and
 * CABAC parameters come from the header, which it is handed.  They were being
 * parsed and thrown away before, so recording them changes no existing value.
 */
struct mtk_h264_slice {
	bool valid;
	u8 nal_type;
	u8 slice_type;
	u32 first_mb_in_slice;
	u8 pps_id;
	u32 frame_num;
	bool field_pic_flag;
	bool bottom_field_flag;
	bool direct_spatial_mv_pred_flag;
	u8 num_ref_idx_l0_active_minus1;
	u8 num_ref_idx_l1_active_minus1;
	s8 slice_qp_delta;
	u8 nal_ref_idc;
	u8 disable_deblocking_filter_idc;
	s8 slice_alpha_c0_offset_div2;
	s8 slice_beta_offset_div2;
	u8 cabac_init_idc;
};

/**
 * struct mtk_h264_ps - parameter set storage for one context.
 * @sps: indexed by seq_parameter_set_id.
 * @pps: indexed by pic_parameter_set_id.
 */struct mtk_h264_ps {
	struct mtk_h264_sps sps[MTK_H264_MAX_SPS];
	struct mtk_h264_pps pps[MTK_H264_MAX_PPS];
};

int mtk_h264_parse_sps(const u8 *data, size_t len, struct mtk_h264_sps *out);
int mtk_h264_parse_pps(const u8 *data, size_t len, struct mtk_h264_pps *out);
/*
 * @nal_type is the NAL unit type (1 or 5) and @nal_ref_idc its top two header
 * bits, both from the NAL unit header byte, which is NOT part of @data: @data
 * points at the payload one byte later.  nal_ref_idc is a separate argument
 * because it decides whether dec_ref_pic_marking is present, so it has to be
 * known *before* the walk rather than recorded after it.
 */
int mtk_h264_parse_slice(const u8 *data, size_t len, u8 nal_type, u8 nal_ref_idc,
			 const struct mtk_h264_ps *ps, struct mtk_h264_slice *out);

/**
 * struct mtk_h264_rbsp - the driver's RBSP reader.
 *
 * Exposed only so that one function on it is reachable from the datapath:
 * mtk_h264_slice_data_bits().  Everything else in here is an implementation
 * detail and is private to mtk-vdec-mt6589-h264.c; nothing outside that file
 * should touch these fields.
 *
 * It is a hand-rolled reader rather than a kernel bitfield helper because the
 * emulation-prevention escape (00 00 03 -> 00 00) makes the byte-to-bit mapping
 * non-linear, and because the reader has to report overflow rather than fault at
 * the end of a NAL unit.
 */
struct mtk_h264_rbsp {
	const u8 *data;
	size_t len;
	size_t bit;		/* absolute bit position in the RBSP */
	size_t zeros;		/* consecutive zero bytes seen, for 00 00 03 */
	bool overflow;
};

/**
 * mtk_h264_slice_data_bits() - how many bits of a slice NAL unit the hardware
 * should decode.
 *
 * The parser programs the AVC_VLD block with the slice header fields and then
 * expects the block to read the slice *data* off the bitstream FIFO, starting at
 * the byte the header ended on.  So it needs two numbers: the byte offset of the
 * first slice_data bit, and how many bits from there belong to this NAL - not to
 * the next start code, and not including this NAL's trailing pad.
 *
 * Both come out of the same walk, because both are properties of where the
 * header ended:
 *
 *   @data_bits = (bit offset of rbsp_stop_one_bit) - (bit offset of slice_data)
 *   @data_len covers the bytes from slice_data's byte to the NAL unit's last
 *   non-zero byte inclusive.
 *
 * The subtraction is what makes this safe.  Once slices may be packed (the
 * stream has a deblocking-filter-control flag, which x264 sets, so the header
 * always ends with ue/se fields), the stop bit is the ONLY upper bound the
 * driver has on slice_data: without it the decoder would run past the end of
 * the picture and into the next slice, and the hardware would not report an
 * error, because a wrong slice length looks exactly like a corrupt bitstream.
 *
 * Returns 0 on success, -EOPNOTSUPP if this driver cannot compute the header
 * length for this stream, or another negative errno on a malformed NAL unit.
 */
int mtk_h264_slice_data_bits(const u8 *data, size_t len, u8 nal_type,
			     const struct mtk_h264_ps *ps,
			     const struct mtk_h264_slice *slice,
			     size_t *data_off, size_t *data_len,
			     unsigned int *data_bits);

/**
 * mtk_h264_find_nal - locate the next NAL unit in a buffer.
 * @data: buffer start.
 * @len: buffer length.
 * @pos: in/out cursor. On entry the search starts here; on return it points at
 *	the first byte after this NAL unit's start code.
 * @nal_start: out, first byte of the NAL unit (after the 3- or 4-byte start code).
 * @nal_len: out, payload length, trailing zero bytes not included.
 *
 * Handles both 00 00 01 and 00 00 00 01 start codes, which may be mixed in one
 * stream. Returns 0 and fills the outputs, 1 when the buffer is exhausted, or a
 * negative errno if the buffer ends inside a start code.
 */
int mtk_h264_find_nal(const u8 *data, size_t len, size_t *pos,
		      size_t *nal_start, size_t *nal_len);

#endif /* _MTK_VDEC_MT6589_H264_H */
