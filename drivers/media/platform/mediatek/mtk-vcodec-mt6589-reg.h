/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2026 Akari Tsuyukusa
 *
 * MediaTek MT6589 hardware video codec registers.
 *
 * Every offset below was derived from the MT6589 data sheet register tables
 * (chapter 60 "H.264/VP8 Video Encoder" and chapter 61 "MPEG-4 Video Encoder",
 * both base +0x17002000) cross-checked against the vendor kernel driver's
 * named offsets.  Where the two agree it is noted, because the agreement is the
 * evidence that the data sheet text extraction is being read correctly: the
 * vendor driver uses VENC_IRQ_STATUS at +0x05C and VENC_IRQ_ACK at +0x060
 * (videocodec_kernel_driver.c:118-119), and so does the data sheet.
 *
 * Unit conventions, stated once here because several register families use
 * them and they are easy to get wrong:
 *
 *   DIV16   the register field holds the byte address >> 4, and the buffer
 *           must be 16-byte aligned.  Applies to every frame, bitstream, RC
 *           and colocated-info address register of the H.264/VP8 front end.
 *   DIV128  the register field holds the byte size >> 7.  Applies to
 *           VENC_BITSTREAM_BUF_SIZE.
 *
 * The MPEG-4 encoder is the exception to both: its address registers hold plain
 * byte addresses with an 8-byte alignment requirement, and its BASE_ADDR field
 * is 32 bits wide rather than 28.  Do not carry the DIV16 convention across.
 *
 * Resulting addressable span.  A 28-bit DIV16 field encodes
 * 2^28 * 16 == 2^32 bytes, i.e. the whole 4 GiB 32-bit address space -- which is
 * to say a 28-bit DIV16 field is exactly as wide as a 32-bit byte address and
 * nothing is actually lost.  (An earlier revision of the .c file's header claimed
 * 2^28 bytes / "256 MiB" for this field, which contradicts the arithmetic the
 * same file performs; the code's arithmetic was right and the comment was
 * wrong.)  The MPEG-4 datapath's 32-bit plain byte address also spans 4 GiB, so
 * both encoders cover the full 32-bit IOVA space.
 *
 * VENC (base 0x17002000, 4 KiB)
 */
#ifndef _MTK_VCODEC_MT6589_REG_H
#define _MTK_VCODEC_MT6589_REG_H

#include <linux/bitops.h>

/* ---- chapter 60: H.264 / VP8 encoder, shared front end ---- */

/* Hardware mode of the shared encoder core; documented by the ch.60 summary
 * table (draft/ds/venc.txt:1910) and its own bit-field section (:2579, reset
 * 0x10000020).  Bits 19:0 are a mix of RW configuration and RO status (:2606);
 * nothing documents it as a command-queue flush.
 */
#define VENC_HW_MODE_SEL		0x000

/* Chapter 60 documents VENC_IRQ_STATUS here; matches the vendor driver. */
#define VENC_IRQ_STATUS			0x05c
/* Chapter 60 documents VENC_IRQ_ACK here; matches the vendor driver. */
#define VENC_IRQ_ACK			0x060

/*
 * Start-of-encode triggers.  VENC_CODEC_CTRL is a one-shot command register: the
 * data sheet gives every bit the same "0: No operation / 1: Start to encode"
 * wording (ch.60, draft/ds/venc.txt:4330-4375), so a set bit starts the
 * corresponding unit and the hardware consumes it.  Writing 0 does nothing at
 * all -- it is NOT a way to clear the bit, which is why this driver never
 * pre-clears CODEC_CTRL before starting a frame.
 */
#define VENC_CODEC_CTRL			0x058	/* VIDEO_CODEC_CONTROL */
#define VENC_CODEC_CTRL_RELEASE_PAUSE_FRM	BIT(4)	/* resume unfinished frame */
#define VENC_CODEC_CTRL_RELEASE_BS_DRAM	BIT(3)	/* restart, new bitstream base */
#define VENC_CODEC_CTRL_ENC_FRM		BIT(2)	/* start one frame */
#define VENC_CODEC_CTRL_ENC_PPS		BIT(1)	/* start PPS */
#define VENC_CODEC_CTRL_ENC_SPS		BIT(0)	/* start SPS */

/*
 * VENC_CE, the video encoder "codec enable" at 0x0EC.
 *
 * SOURCING IS WEAK AND DELIBERATELY LABELLED AS SUCH: this register has no
 * bit-field section anywhere in the data sheet.  It appears exactly once, as a
 * bare name "VENC_CE / VIDEO_CE" in the ch.60 summary register table
 * (draft/ds/venc.txt:2176).  Its field width, its bit position and therefore
 * the exact value that means "enabled" are NOT documented; 1 below is inferred
 * from the vendor driver, not read out of a table.
 *
 * The behavioural evidence, however, is real and consistent.  The vendor clock
 * management code writes VENC_CE = 0x1 as a hard prerequisite before touching
 * any other VENC internal register -- in both the DCM-enable path
 * (kernel/core/mt_dcm.c:299) and the DCM-disable path (mt_dcm.c:396) it is the
 * very first VENC write, immediately before VENC_CLK_DCM_CTRL,
 * VENC_CLK_CG_CTRL and VENC_MP4_DCM_CTRL, and the DCM register-dump path
 * (mt_dcm.c:186) writes it for the same reason before reading those registers.
 * The offset agrees with the data sheet exactly: mt_dcm.h:102 defines it as
 * 0xF70020EC, which is 0x170020EC after the +0xE0000000 remap.  It is never
 * written 0 anywhere in the vendor tree.
 *
 * So: the offset is solid, the "write 1 to enable" semantics is solid, and only
 * the field's width and bit position are unknown.  This driver writes the
 * whole word as 1, which enables whatever bit(s) exist without depending on a
 * position that is not documented.
 */
#define VENC_CE				0x0ec

#define VENC_STUFFING_REPORT		0x0a0	/* bitstream stuffing report */
#define VENC_IRQ_MODE_SEL		0x0a4	/* irq vs dma completion mode */
#define VENC_SW_HRST_N			0x0a8	/* 1 = encoder out of soft reset */
#define VENC_SW_PAUSE			0x0ac	/* pause/resume the engine */
#define VENC_PAUSE_MODE_INFO		0x0b0	/* read-only pause reason */

/*
 * Frame and bitstream buffers.  This is an inter-frame encoder: the datapath
 * reads the current frame, transforms and quantises against the reference
 * frame, and DMAs the reconstructed pixels out to the REC buffers.  The
 * reconstruction buffer is therefore not optional -- without it the transform
 * stage has nowhere to write its result.
 *
 * Every address below is DIV16 (see the unit conventions at the top of this
 * file) and every field is 28 bits wide.
 */
#define VENC_BITSTREAM_BUF_ADDR		0x064
#define VENC_BITSTREAM_BUF_SIZE		0x068	/* DIV128 */
#define VENC_FRM_CUR_Y_ADDR		0x06c	/* source luma */
#define VENC_FRM_CUR_UV_ADDR		0x070	/* source chroma */
#define VENC_FRM_REF_Y_ADDR		0x074	/* reference luma */
#define VENC_FRM_REF_UV_ADDR		0x078	/* reference chroma */
#define VENC_FRM_REC_Y_ADDR		0x07c	/* reconstructed luma */
#define VENC_FRM_REC_UV_ADDR		0x080	/* reconstructed chroma */

/*
 * VP8 writes its frame header to a second, separate DRAM buffer; the residual
 * partition then goes to BITSTREAM_BUF.  That is why the VP8 byte accounting
 * needs two counters and why a VP8 encode needs two output buffers.  Recorded
 * for completeness; the driver does not program these (see the VP8 note in the
 * .c file).
 */
#define VENC_VP8_HDR_BUF_ADDR		0x0e0
#define VENC_VP8_HDR_BUF_SIZE		0x0e4

/*
 * Quantiser fields.  These are the registers the data sheet's bit tables actually
 * define, and they are NOT the ones their own summary table's names suggest:
 * QP_I_FRM lives in VENC_ENCODER_INFO_0 at +0x004 (bits 31:26) and QP_P_FRM /
 * QP_B_FRM in VENC_ENCODER_INFO_1 at +0x008 (bits 15:10 and 31:26)
 * (draft/ds/venc.txt:2802-2804, 2893-2901, 2945-2947).
 *
 * Confusingly, VENC_H264_ENC_INFO_0 at +0x030 has nothing to do with QP: its bit
 * table is CABAC / MBAFF / PROFILE / H264_LEVEL (draft/ds/venc.txt:3655ff), and
 * VENC_H264_ENC_INFO_1 at +0x034 is "rev", i.e. wholly reserved
 * (draft/ds/venc.txt:3774ff).  Writing a QP there would write into CABAC and
 * level fields, so this driver does not.
 */
#define VENC_ENCODER_INFO_0		0x004
#define VENC_ENCODER_INFO_1		0x008
#define VENC_QP_I_FRM_SHIFT		26
#define VENC_QP_I_FRM_MASK		GENMASK(31, 26)
#define VENC_QP_P_FRM_SHIFT		10
#define VENC_QP_P_FRM_MASK		GENMASK(15, 10)
#define VENC_QP_B_FRM_SHIFT		26
#define VENC_QP_B_FRM_MASK		GENMASK(31, 26)

/*
 * The data sheet states the H.264 quantiser range explicitly as "[0, 51] for
 * H.264" for all three of QP_I_FRM, QP_P_FRM and QP_B_FRM
 * (draft/ds/venc.txt:2803, 2894-2895, 2946).  The neighbouring rows give VP8
 * as [1, 63] and MPEG-4 as [1, 31]; this driver only drives the H.264 path, so
 * 0..51 is the range it validates against.  Note that 0 is a legal H.264 QP --
 * an earlier revision of this driver clamped 1..31 and rejected valid requests.
 */
#define VENC_H264_QP_MIN			0
#define VENC_H264_QP_MAX			51

/* Per-codec encoder info blocks; recorded, deliberately not programmed. */
#define VENC_H264_ENC_INFO_0		0x030	/* CABAC / MBAFF / profile / level */
#define VENC_H264_ENC_INFO_1		0x034	/* wholly reserved */
#define VENC_VP8_ENC_INFO_0		0x040
#define VENC_VP8_ENC_INFO_1		0x044

/*
 * Rate control.  RATECONTROL_INFO_0 and _1 are the only two of the four that
 * carry any defined field; _2 and _3 are "rev" top to bottom
 * (draft/ds/venc.txt:4206-4260) and an earlier revision of this driver wrote the
 * QP triple and a bitrate limit into them, which programmed nothing but garbage.
 *
 *   INFO_0 (draft/ds/venc.txt:4025-4087)
 *     18      RC_CBR               constant vs variable bit rate
 *     17      RC_INI_QP            use the QP_I/P/B_FRM values as the initial QP
 *     16:0    RC_TARGET_BIT_RATE   target bit rate
 *
 *   INFO_1 (draft/ds/venc.txt:4088-4205)
 *     31      ENABLE_EIS           rate control reads the EIS MMR
 *     30      ENABLE_ROI           rate control reads the ROI MMR
 *     27      AIFI                 insert adaptive I-frames
 *     26      SKYPE_MODE           skype mode rate control algorithm
 *     25      AFPS                 adaptively change fps
 *     24      ATBR                 adaptively change target bit rate
 *     23:16   RC_FPS               rate control fps, 0 = default 30
 *     15:8    BfrmQLimter          B frame QP adjust limiter, "Suggested: 5"
 *     7:0     PfrmQLimter          P frame QP adjust limiter, "Suggested: 3"
 */
#define VENC_RATECONTROL_INFO_0		0x048
#define VENC_RATECONTROL_INFO_1		0x04c
#define VENC_RATECONTROL_INFO_2		0x050	/* entirely reserved, do not write */
#define VENC_RATECONTROL_INFO_3		0x054	/* entirely reserved, do not write */

#define VENC_RC_TARGET_BIT_RATE_MASK	GENMASK(16, 0)
#define VENC_RC_CBR			BIT(18)
#define VENC_RC_INI_QP			BIT(17)
#define VENC_RC_FPS_SHIFT		16
#define VENC_RC_FPS_MASK			GENMASK(23, 16)
#define VENC_RC_PFRM_Q_LIM_MASK		GENMASK(7, 0)
#define VENC_RC_BFRM_Q_LIM_MASK		GENMASK(15, 8)

/*
 * The data sheet gives a suggested value AND a valid range for each limiter, not
 * just a suggested one: "Suggested: 5, Range: 5 ~ 8" for BfrmQLimter and
 * "Suggested: 3, Range: 3 ~ 6" for PfrmQLimter (draft/ds/venc.txt:4186-4195).
 * Validating against the field width alone would accept 0, 1, 2 and everything
 * above the range, so these are the ranges enforced instead.
 */
#define VENC_RC_BFRM_Q_LIM_MIN		5
#define VENC_RC_BFRM_Q_LIM_MAX		8
#define VENC_RC_PFRM_Q_LIM_MIN		3
#define VENC_RC_PFRM_Q_LIM_MAX		6
#define VENC_RC_BFRM_Q_LIM_SUGGESTED	5
#define VENC_RC_PFRM_Q_LIM_SUGGESTED	3

/*
 * Rate control scratch memory.  Both of these are DRAM ADDRESS registers, not
 * parameter registers: RC_INFO_DRAM_ADDR_DIV16[27:0] is "Initial DRAM byte
 * address of RC info. for loading and saving divided by 16"
 * (draft/ds/venc.txt:5134-5140) and RC_CODE_DRAM_ADDR_DIV16[27:0] is "Initial
 * DRAM byte address of RC code divided by 16" (draft/ds/venc.txt:5068-5075).
 * Both are DIV16 and 28 bits wide.
 *
 * An earlier revision of this driver wrote the intra-VOP rate, a bare small
 * integer, into VENC_RC_INFO_DRAM_ADDR.  That is not a rate control setting at
 * all -- it pointed the rate control scratch pointer at DRAM address
 * intra_vop_rate * 16 and would have had the hardware load its state from
 * whatever happened to live there.
 */
#define VENC_RC_CODE_DRAM_ADDR		0x08c	/* DIV16 address */
#define VENC_RC_INFO_DRAM_ADDR		0x090	/* DIV16 address */

/*
 * Bitstream length.  This is the authoritative "bytes used" for an H.264
 * frame: the data sheet defines PIC_BITSTREAM_BYTE_CNT[23:0] as "Number of
 * bytes in coded bitstream of one frame".  The same field is re-purposed for
 * VP8, where it counts only the residual partition, with the VP8 header
 * counted separately in VENC_PIC_BITSTREAM_BYTE_CNT1.  See the VP8 note in the
 * .c file for why the driver does not use the VP8 form.
 *
 * VENC_STUFFING_REPORT at +0x0A0 is *not* a byte count; it reports bitstream
 * stuffing, and using it as bytesused is a bug.
 */
#define VENC_PIC_BITSTREAM_BYTE_CNT	0x098
#define VENC_PIC_BITSTREAM_BYTE_CNT1	0x0e8

/*
 * Interrupt bits.  VENC_IRQ_STATUS at +0x05C and VENC_IRQ_ACK at +0x060 carry
 * the same six bit positions with the same names (ENC_SPS/SPS_ACK 0,
 * ENC_PPS/PPS_ACK 1, ENC_FRM/FRM_ACK 2, BS_DRAM_FULL 3, PAUSE_FRM 4,
 * VP8_HEADER_BS_DRAM_FULL 5); see draft/ds/venc.txt:4440-4538.  Both are
 * WO/write-1-to-clear style: the ack register takes "Set to 1 to clear the
 * current bit".
 *
 * VENC_IRQ_MASK_ALL is 0x3f, which is exactly bits 0..5 -- the complete set of
 * defined sources in the pair, and nothing beyond it.  There is no second
 * IRQ_STATUS1/ACK1 pair in chapter 60: "IRQ_STATUS1" appears nowhere in the
 * extract, and VENC_PIC_BITSTREAM_BYTE_CNT1 at +0x0E8 is a byte counter, not an
 * interrupt register.
 *
 * These bit values are independently corroborated by the vendor driver
 * (videocodec_kernel_driver.c:129-134), which defines SPS 0x1, PPS 0x2,
 * FRM 0x4, DRAM 0x8, PAUSE 0x10, DRAM_VP8 0x20 and acks FRM as 0x4
 * (videocodec_kernel_driver.c:412, 440-442).
 */
#define VENC_IRQ_STATUS_SPS		BIT(0)
#define VENC_IRQ_STATUS_PPS		BIT(1)
#define VENC_IRQ_STATUS_FRM		BIT(2)
#define VENC_IRQ_STATUS_DRAM		BIT(3)
#define VENC_IRQ_STATUS_PAUSE		BIT(4)
#define VENC_IRQ_STATUS_DRAM_VP8	BIT(5)
#define VENC_IRQ_MASK_ALL		0x3f

/* ---- chapter 61: MPEG-4 encoder, the "hybrid" datapath ----
 *
 * The MPEG-4 encoder is a separate block inside the same VENC window starting at
 * +0x600.  It has its own interrupt pair, its own DMA control and its own
 * address registers, which is why the vendor driver treats MPEG-4 as a
 * different driver type from H.264/VP8 and why its ISR acks a different
 * register.
 *
 * Unlike chapter 60, these address registers hold plain byte addresses with an
 * 8-byte alignment requirement (ch.61, "VENC_MP4_SRCADR_Y": "This address is
 * 8-byte aligned") in a full 32-bit BASE_ADDR field.  No DIV16 here.
 */

/*
 * Frame start trigger: encode from the frame's first MB.  Bit 0 is TRIGGER, a
 * write-only "Writing 1 to it will trigger hardware initialization process and
 * encoding from the frame's first MB.  Writing 0 to it has no effect"
 * (draft/ds/mp4.txt:6408-6410).  So this is a self-consuming one-shot: write 1
 * and nothing else -- a preceding write of 0 is a no-op, not a clear.
 */
#define VENC_MP4_FRAME_START		0x600
/* Slice resume trigger: encode from the last stopped MB position. */
#define VENC_MP4_SLICE_START		0x604
/* Source buffer width in number of macroblocks. */
#define VENC_MP4_MBX_LMT		0x608
/* Source buffer height in number of macroblocks. */
#define VENC_MP4_MBY_LMT		0x60c
/* Horizontal stopping MB index of encoding. */
#define VENC_MP4_MBX_STOP		0x610
/* Vertical stopping MB index of encoding. */
#define VENC_MP4_MBY_STOP		0x614
/* Switch of MPEG-4 / H.263 encoding. */
#define VENC_MP4_VOP_TYPE		0x618
#define VENC_MP4_SHRT_VIDEO_HDR		0x61c
/* Forward Fcode for motion vectors. */
#define VENC_MP4_FCODE			0x620
/* Backward Fcode for motion vectors. */
#define VENC_MP4_BCODE			0x624
/* Rounding control for motion compensation. */
#define VENC_MP4_RND_CTRL		0x628
/* Base address of the per-MB side information buffer (16-byte aligned). */
#define VENC_MP4_SIDE_ADDR		0x638

#define VENC_MP4_SRCADR_Y		0x63c	/* source luma */
#define VENC_MP4_SRCADR_CB		0x640	/* source Cb */
#define VENC_MP4_SRCADR_CR		0x644	/* source Cr */
#define VENC_MP4_RECADR_Y		0x648	/* reconstructed luma */
#define VENC_MP4_RECADR_CB		0x64c	/* reconstructed Cb */
#define VENC_MP4_RECADR_CR		0x650	/* reconstructed Cr */
#define VENC_MP4_REFADR_Y		0x654	/* reference luma */
#define VENC_MP4_REFADR_CB		0x658	/* reference Cb */
#define VENC_MP4_REFADR_CR		0x65c	/* reference Cr */
#define VENC_MP4_BITADR			0x660	/* bitstream output */
#define VENC_MP4_ENC_STATUS		0x664	/* HW_BUSY is bit 13 */
#define VENC_MP4_IRQ_EN			0x668	/* ENABLE_DONE bit 0, ENABLE_FULL bit 4 */
#define VENC_MP4_MC_CTRL		0x66c
#define VENC_MP4_BSDMA_CTRL		0x670
/*
 * Zero-coefficient counters for the even and odd slice threads.  All three are
 * read-only status, NOT acknowledgements: the data sheet summary table calls
 * 0x674 and 0x698 "Acknowledgement to Hardware's IRQ Signal", which is a
 * copy-paste slip from 0x678 -- the bit tables for both give Type RO and define
 * an *_SLICE_COUNTER / NON_ZERO_COEF_COUNT pair, which is not an
 * acknowledgement register.  The only MPEG-4 ack register is 0x678, whose fields
 * really are ACKNOWLEDGE_DONE / ACKNOWLEDGE_FULL.
 */
#define VENC_MP4_ZERO_COEF_COUNT2	0x674	/* even slice, RO */
/* ACKNOLEDGE_DONE is bit 0, ACKNOLEDGE_FULL is bit 4. */
#define VENC_MP4_IRQ_ACK		0x678
#define VENC_MP4_IRQ_STATUS		0x67c
#define VENC_MP4_BYTE_COUNT		0x680	/* bytes the write DMA has emitted */
#define VENC_MP4_BIT_COUNT		0x684	/* bits encoded so far this frame */
#define VENC_MP4_ZERO_COEF_COUNT	0x688	/* non-zero AC coefficients */
#define VENC_MP4_QP			0x68c	/* frame / slice quantiser scale */
#define VENC_MP4_RESET			0x690	/* hard reset bit 4, soft reset bit 0 */
#define VENC_MP4_CDMA_CTRL		0x694
#define VENC_MP4_ZERO_COEF_COUNT3	0x698	/* odd slice, RO */
#define VENC_MP4_MVQP_STATUS		0x6e4

/*
 * VENC_MP4_IRQ_STATUS is a read-only bitfield, not a state value: bit 0 is
 * SLICE_IRQ, bit 1 FRAME_IRQ and bit 4 BITSTREAM_IRQ.  The vendor driver acks
 * the DONE path by writing 1 to VENC_MP4_IRQ_ACK
 * (videocodec_kernel_driver.c:530) and decodes these very bits by name in its
 * status dump (videocodec_kernel_driver.c:560-564), so a frame completion is
 * "FRAME_IRQ is set", not "the register reads back as 2".
 */
#define VENC_MP4_IRQ_STATUS_SLICE	BIT(0)
#define VENC_MP4_IRQ_STATUS_FRAME	BIT(1)
#define VENC_MP4_IRQ_STATUS_FULL	BIT(4)
#define VENC_MP4_IRQ_STATUS_MASK	0x11

/* VENC_MP4_IRQ_EN / VENC_MP4_IRQ_ACK bit assignments. */
#define VENC_MP4_IRQ_EN_DONE		BIT(0)
#define VENC_MP4_IRQ_EN_FULL		BIT(4)
#define VENC_MP4_IRQ_ACK_DONE		BIT(0)
#define VENC_MP4_IRQ_ACK_FULL		BIT(4)

/*
 * Frame completion for the MPEG-4 datapath: FRAME_IRQ on its own.  The
 * bitstream-full condition is deliberately excluded, because that interrupt
 * means the output buffer overflowed and the frame is not usable.
 */
#define VENC_MP4_IRQ_STATUS_DONE	VENC_MP4_IRQ_STATUS_FRAME

/*
 * Slice control.  The encoder splits a picture into rows of macroblocks and
 * dispatches each slice to the hardware on its own thread, so the slice count is
 * a function of picture size and the MBX/MBY limits rather than a single field.
 * The MB limits are in macroblocks, not pixels, and the data sheet constrains
 * MB_XLIMIT to "bigger than 0 and smaller than 255".
 */
#define VENC_MP4_MBX_LMT_MAX		254
#define VENC_MP4_MBY_LMT_MAX		254

/* ---- VDEC (base 0x16020000, 0x29000) ----
 *
 * The data sheet has NO register-definition section for the decoder (chapter 59
 * stops at the block diagram, and "16000000" does not appear anywhere in it).
 * These offsets come from the vendor LDVT register-level test harness,
 * kernel/drivers/ldvt/vdec/hal/vdec_hw_common.h, which is 27k lines of real
 * per-codec register definitions.  The base is mt_reg_base.h VDEC_BASE
 * 0xF6020000, which is 0x16020000 after the +0xE0000000 remap - note this is NOT
 * 0x16000000, which is the vdecsys clock controller.
 */

/* Sub-block bases, relative to the decoder base. */
#define VDEC_MISC_BASE			0x0000	/* top level control and status */
#define VDEC_VLD_BASE			0x1000	/* bitstream parser */
#define VDEC_MC_BASE			0x2000	/* motion compensation */
#define VDEC_AVC_VLD_BASE		0x3000	/* H.264 bitstream parser */
#define VDEC_AVC_MV_BASE		0x4000	/* H.264 motion vector */

/*
 * Frame completion.  The decoder has no status/ack register pair: frame end is
 * bit 16 of MISC word 41, and the interrupt is cleared by setting bits 0 and 4
 * then writing the original value back.  This is confirmed twice, by the vendor
 * LDVT harness (vdec_hal_if_avs.c:1098-1106) and by the vendor driver
 * (videocodec_kernel_driver.c:346-360).
 */
#define VDEC_MISC_FRAME_END		(41 * 4)
#define VDEC_FRAME_END_BIT		BIT(16)
#define VDEC_FRAME_END_SET		BIT(4)
#define VDEC_FRAME_END_ACK		BIT(0)

/* H.264 prediction weight table, MT6589 specific (vdec_hal_if_h264.c:568). */
#define VDEC_MISC_WEIGHT_TABLE		(60 * 4)
#define VDEC_WEIGHT_TABLE_VALID		BIT(0)

/* Bitstream parser top-level status (vdec_hw_common.h:162). */
#define VDEC_VLD_BARL			0x00
#define VDEC_VLD_TOP_BASE		(VLD_REG_OFFSET0 + 0x800)

#endif /* _MTK_VCODEC_MT6589_REG_H */
