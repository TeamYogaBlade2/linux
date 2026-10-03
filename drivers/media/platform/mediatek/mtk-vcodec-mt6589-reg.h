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
 * vendor driver uses VENC_IRQ_STATUS at +0x05C and VENC_IRQ_ACK at +0x060, and
 * so does the data sheet.
 *
 * VENC (base 0x17002000, 4 KiB)
 */
#ifndef _MTK_VCODEC_MT6589_REG_H
#define _MTK_VCODEC_MT6589_REG_H

#include <linux/bitops.h>

/* ---- chapter 60: H.264 / VP8 encoder, shared front end ---- */

/* Chapter 60 documents VENC_IRQ_STATUS here; matches the vendor driver. */
#define VENC_IRQ_STATUS			0x05c
/* Chapter 60 documents VENC_IRQ_ACK here; matches the vendor driver. */
#define VENC_IRQ_ACK			0x060

#define VENC_CODEC_CTRL			0x058	/* VIDEO_CODEC_CONTROL */
#define VENC_STUFFING_REPORT		0x0a0	/* bitstream stuffing count */
#define VENC_IRQ_MODE_SEL		0x0a4	/* irq vs dma completion mode */
#define VENC_SW_HRST_N			0x0a8	/* 1 = encoder out of soft reset */
#define VENC_SW_PAUSE			0x0ac	/* pause/resume the engine */
#define VENC_PAUSE_MODE_INFO		0x0b0	/* read-only pause reason */

/*
 * Frame and bitstream buffers are plain physical DRAM addresses.  The encoder
 * is fed from the current frame and reconstructs into a reference frame, which
 * is what makes it an inter-frame encoder.
 */
#define VENC_BITSTREAM_BUF_ADDR		0x064
#define VENC_BITSTREAM_BUF_SIZE		0x068
#define VENC_FRM_CUR_Y_ADDR		0x06c	/* source luma */
#define VENC_FRM_CUR_UV_ADDR		0x070	/* source chroma */
#define VENC_FRM_REF_Y_ADDR		0x074	/* reference luma */
#define VENC_FRM_REF_UV_ADDR		0x078	/* reference chroma */
#define VENC_FRM_REC_Y_ADDR		0x07c	/* reconstructed luma */
#define VENC_FRM_REC_UV_ADDR		0x080	/* reconstructed chroma */

/* Rate control scratch memory and per-codec encoder info blocks. */
#define VENC_RC_CODE_DRAM_ADDR		0x08c
#define VENC_RC_INFO_DRAM_ADDR		0x090
#define VENC_H264_ENC_INFO_0		0x030
#define VENC_H264_ENC_INFO_1		0x034
#define VENC_VP8_ENC_INFO_0		0x040
#define VENC_VP8_ENC_INFO_1		0x044
#define VENC_RATECONTROL_INFO(n)		(0x048 + 0x4 * (n))

/* Interrupt bits, as used by the vendor driver (videocodec_kernel_driver.c:129). */
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
 */

/* Frame start trigger, shared with chapter 60's VENC_MP4_FRAME_STAR at +0x1a4. */
#define VENC_MP4_FRAME_START		0x600

#define VENC_MP4_SLICE_START		0x600
#define VENC_MP4_MBX_LMT		0x604
#define VENC_MP4_MBY_LMT		0x608
#define VENC_MP4_MBX_STOP		0x60c
#define VENC_MP4_MBY_STOP		0x610
#define VENC_MP4_VOP_TYPE		0x614	/* I / P / B VOP */
#define VENC_MP4_SHRT_VIDEO_HDR		0x618
#define VENC_MP4_FCODE			0x61c
#define VENC_MP4_BCODE			0x620
#define VENC_MP4_RND_CTRL		0x624

#define VENC_MP4_SRCADR_Y		0x638	/* source luma */
#define VENC_MP4_SRCADR_CB		0x63c	/* source Cb */
#define VENC_MP4_SRCADR_CR		0x640	/* source Cr */
#define VENC_MP4_RECADR_Y		0x644	/* reconstructed luma */
#define VENC_MP4_RECADR_CB		0x648
#define VENC_MP4_RECADR_CR		0x64c
#define VENC_MP4_REFADR_Y		0x650
#define VENC_MP4_REFADR_CB		0x654
#define VENC_MP4_REFADR_CR		0x658
#define VENC_MP4_BITADR			0x65c	/* bitstream output */
#define VENC_MP4_ENC_STATUS		0x660

/*
 * Chapter 60 documents VENC_MP4_IRQ_EN at +0x660 and VENC_MP4_IRQ_ACK at +0x670;
 * the vendor driver uses +0x668 for IRQ enable and +0x678 for the MPEG-4 ack.
 * These two sources disagree by 8 bytes, and because the pair is adjacent in
 * both, either a stray "0x60" or "0x70" in one of them is the likely cause.
 * This is unresolved: see NOTES.md.  The driver below uses the vendor values,
 * since those are what has actually been run on hardware.
 */
#define VENC_MP4_IRQ_EN			0x668	/* vendor; datasheet says 0x660 */
#define VENC_MP4_IRQ_ACK		0x678	/* vendor; datasheet says 0x670 */
#define VENC_MP4_IRQ_STATUS		0x67c
#define VENC_MP4_BYTE_COUNT		0x680	/* bytes written this frame */
#define VENC_MP4_ZERO_COEF_COUNT	0x688
#define VENC_MP4_QP			0x684
#define VENC_MP4_RESET			0x688

/*
 * The MPEG-4 ISR in the vendor driver validates the status word read at the
 * first returned register equals 2 before treating the interrupt as a real
 * frame completion (videocodec_kernel_driver.c:497-503), so 2 is the
 * frame/slice-done encoding rather than an arbitrary constant.
 */
#define VENC_MP4_IRQ_STATUS_DONE	0x2

/*
 * Slice control.  The encoder splits a picture into rows of macroblocks and
 * dispatches each slice to the hardware on its own thread, so the slice count is
 * a function of picture size and MBX/MBY limits rather than a single field.
 */
#define VENC_MP4_MBX_LMT_DEFAULT	0x0

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
