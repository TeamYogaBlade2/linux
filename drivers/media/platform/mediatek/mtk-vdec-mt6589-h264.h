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
 * @slice_qp_delta: signed; kept for completeness, not programmed (the QP comes
 *	from the PPS and the MB layer).
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
};

/**
 * struct mtk_h264_ps - parameter set storage for one context.
 * @sps: indexed by seq_parameter_set_id.
 * @pps: indexed by pic_parameter_set_id.
 */
struct mtk_h264_ps {
	struct mtk_h264_sps sps[MTK_H264_MAX_SPS];
	struct mtk_h264_pps pps[MTK_H264_MAX_PPS];
};

int mtk_h264_parse_sps(const u8 *data, size_t len, struct mtk_h264_sps *out);
int mtk_h264_parse_pps(const u8 *data, size_t len, struct mtk_h264_pps *out);
int mtk_h264_parse_slice(const u8 *data, size_t len, u8 nal_type,
			 const struct mtk_h264_ps *ps, struct mtk_h264_slice *out);

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
