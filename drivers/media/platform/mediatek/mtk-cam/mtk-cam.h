/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6589 CAM/ISP top-level (control + DMA) driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * Only the registers and fields that are legible in the MT6589 data sheet
 * are recorded here.  Anything not verifiable is deliberately absent rather
 * than guessed; see mtk-cam.c.
 */

#ifndef _MTK_CAM_H
#define _MTK_CAM_H

#include <linux/mutex.h>
#include <linux/types.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>

/*
 * CAM block base 0x15004000.  Registers transcribed from the data sheet.
 *
 * The block splits its control registers three ways, and this matters when
 * writing to them:
 *
 *   - plain read/write shadow registers (CAM_CTL_START and friends)
 *   - write-only set/clear registers (CAM_CTL_EN1_SET / _CLR), which have
 *     no readback and must not be written directly
 *   - the readable status copies of the same bits
 */
#define CAM_CTL_START			0x00
#define CAM_CTL_EN1			0x04	/* shadow; use _SET/_CLR */
#define CAM_CTL_EN2			0x08	/* shadow; use _SET/_CLR */
#define CAM_CTL_DMA_EN			0x0c	/* shadow; use _SET/_CLR */
#define CAM_CTL_FMT_SEL			0x10
#define CAM_CTL_SEL			0x18
#define CAM_CTL_PIX_ID			0x1c
#define CAM_CTL_INT_EN			0x20
#define CAM_CTL_INT_STATUS		0x24
#define CAM_CTL_DMA_INT			0x28
#define CAM_CTL_TPIPE			0x50
#define CAM_CTL_TCM_EN			0x54
#define CAM_CTL_SRAM_CFG		0x58
#define CAM_CTL_SW_CTL			0x5c	/* software reset control */

/* Write-only set/clear companions of the EN1/EN2/DMA_EN shadows. */
#define CAM_CTL_EN1_SET			0x80
#define CAM_CTL_EN1_CLR			0x84
#define CAM_CTL_EN2_SET			0x88
#define CAM_CTL_EN2_CLR			0x8c
#define CAM_CTL_DMA_EN_SET		0x90
#define CAM_CTL_DMA_EN_CLR		0x94

/*
 * CAM_CTL_START (0x15004000), reset value 0x00000000.  All six documented
 * bits are type WO start strobes (data sheet page 1949-1950); there is no
 * read/write state bit in this register.
 */
#define CAM_CTL_START_CQ0B_START	BIT(6)
#define CAM_CTL_START_CQ0_START		BIT(5)
#define CAM_CTL_START_PASS2C_START	BIT(4)
#define CAM_CTL_START_FMT_START		BIT(3)
#define CAM_CTL_START_PASS2B_START	BIT(1)
#define CAM_CTL_START_PASS2_START	BIT(0)

/*
 * The individual CAM_CTL_EN1 sub-module bits.
 *
 * These ARE documented: the MT6589 data sheet page 1977 carries the
 * CAM_CTL_EN1_SET bit table, page 1978 the matching CAM_CTL_EN1_CLR table.
 * Every one of them is type WO, described as "0: No effect, 1: Set this bit
 * to 1" for _SET and "0: No effect, 1: Set this bit to 1" for _CLR.  So the
 * two registers are write-one-to-set and write-one-to-clear mirrors of
 * CAM_CTL_EN1 -- writing a 1 into _SET sets that enable, writing a 1 into
 * _CLR clears it.  There is no write-zero-to-clear anywhere in this block.
 *
 * The block-level enable the driver actually needs to touch is CAM_EN (bit
 * 30); the rest are CPIPE stages and are not enabled until someone writes
 * the pipeline itself.
 */
#define CAM_CTL_EN1_CAM_EN			BIT(30)

/*
 * CAM_CTL_DMA_EN (0x1500400c) bit 0, IMGO_EN.
 *
 * Data sheet page 1952-1953 carries the CAM_CTL_DMA_EN bit table; bit 0 is
 * named IMGO_EN, "Enables IMGO.  IMGO can ouput from CDRZ, packing, TG1, MFB,
 * and RGB R plane, depending on the scenario, submode and cam_out_fmt.  Double
 * buffer."  The vendor agrees independently: isp_function.h:396
 *
 *	#define CAM_CTL_DMA_EN_IMGO_EN  (1L<<0)
 *
 * Note the datasheet and the vendor differ on where the *other* channels are
 * (PostProcPipe.cpp:56-65 maps IMGI to TCM_EN bit 1, i.e. a TCM table, not a
 * DMA_EN table), so only the IMGO bit -- the one both sources agree on -- is
 * defined here.
 */
#define CAM_CTL_DMA_EN_IMGO_EN			BIT(0)

/*
 * CAM_CTL_SW_CTL (0x1500405c): one trigger/status handshake for the whole CAM
 * block, not a per-module reset mask.  HW_RST resets every module except the
 * register-setting ones, SW_RST_ST reads 1 once DMA is idle, SW_RST_TRIG
 * starts it.  Bits 31:3 are undocumented and the register does not reset to
 * zero, so only these three are ever written.
 */
#define CAM_SW_CTL_HW_RST			BIT(2)
#define CAM_SW_CTL_SW_RST_ST			BIT(1)
#define CAM_SW_CTL_SW_RST_TRIG			BIT(0)

/*
 * CAM_CTL_INT_EN: DMA_ERR_EN is block-wide (the block has no per-channel
 * equivalent), IMGO_ERR_EN covers this channel's overrun.  Both are needed
 * for a stalled buffer to reach the shared ISP interrupt line.
 *
 * CAM_CTL_INT_STATUS mirrors those positions.  With INT_WCLR_EN
 * (CAM_CTL_INT_EN bit 31) left at 0 the register is read-clear, so the read
 * in mtk_cam_irq() is also the acknowledgement.
 */
#define CAM_CTL_INT_EN_DMA_ERR_EN		BIT(30)
#define CAM_CTL_INT_EN_IMGO_ERR_EN		BIT(20)
#define CAM_CTL_INT_STATUS_IMGO_ERR		BIT(20)
#define CAM_CTL_INT_STATUS_DMA_ERR		BIT(30)

/*
 * CAM_CTL_DMA_INT: bits 31:16 are RW enables, bits 15:0 RO status.  Status is
 * read-clear for the same reason as above.
 */
#define CAM_CTL_DMA_INT_IMGO_DONE_EN		BIT(16)
#define CAM_CTL_DMA_INT_IMGO_DONE_ST		BIT(0)

/*
 * The three command-queue base-address registers.
 *
 * Data sheet chapter 54.3 lists all of them and, unlike the IMGO DMA
 * registers, actually documents their one field:
 *
 *   150040A8  CAM_CTL_CQ0_BASEADDR  "CTL_CQ0_BASEADDR[31:0]"  "CQ0 base address"
 *   150040B8  CAM_CTL_CQ0B_BASEADDR
 *   150040BC  CAM_CTL_CQ0C_BASEADDR
 *
 * (data sheet page 1985 carries the full bit table for CQ0, one RW field
 * CTL_CQ0_BASEADDR[31:0] described as "CQ0 base address"; CQ0B is on page
 * 1987 and CQ0C on the same, with the same 32-bit RW field.)  The offsets are
 * corroborated by the vendor register block at isp_reg.h:8866-8871, whose
 * comments read "// 40A8", "// 40B8" and "// 40BC".
 */
#define CAM_CTL_CQ0_BASEADDR		0x0a8
#define CAM_CTL_CQ0B_BASEADDR		0x0b8
#define CAM_CTL_CQ0C_BASEADDR		0x0bc

/*
 * CAM_CTL_FMT_SEL: scenario 3:0, sub_mode 7:4, cam_in_fmt 11:8,
 * cam_out_fmt 15:12.
 *
 * sub_mode and cam_in_fmt are one decision, not two: sub_mode selects which
 * of the data sheet's input tables cam_in_fmt is read against.  This driver
 * programs the RAW pair - sub_mode = CAM_SUB_MODE_RAW with
 * cam_in_fmt = CAM_FMT_SEL_BAYER10 - because SCAM passes the sensor's Bayer
 * payload through unconverted.
 *
 * cam_out_fmt is YUV422 1 plane: it is the only value the data sheet allows
 * in every scenario, and it is the packed layout the IMGO size arithmetic
 * below assumes.
 */
#define CAM_CTL_FMT_SEL_SCENARIO_SHIFT		0
#define CAM_CTL_FMT_SEL_SUB_MODE_SHIFT		4
#define CAM_CTL_FMT_SEL_CAM_IN_FMT_SHIFT	8
#define CAM_CTL_FMT_SEL_CAM_OUT_FMT_SHIFT	12
#define CAM_CTL_FMT_SEL_CAM_IN_FMT_MASK		GENMASK(11, 8)
#define CAM_CTL_FMT_SEL_CAM_OUT_FMT_MASK	GENMASK(15, 12)

/*
 * CAM_CTL_FMT_SEL values.  cam_in_fmt and sub_mode are one decision: which of
 * the three input tables in the data sheet (page 1955-1956) applies is fixed by
 * sub_mode, so these two are only meaningful together.
 */

/* cam_in_fmt values, valid when sub_mode is RAW. */
#define CAM_FMT_SEL_BAYER10			1
/* cam_out_fmt values, from the YUV output table. */
#define CAM_FMT_OUT_YUV422_1P			2

/* CAM_CTL_FMT_SEL sub_mode: 0 connects a RAW sensor, 1 a YUV sensor. */
#define CAM_SUB_MODE_RAW			0

/* ------------------------------------------------------------------ */
/* IMGO DMA engine                                                    */
/* ------------------------------------------------------------------ */

/*
 * The IMGO output channel.  Offsets are from the data sheet's CAM register
 * map, chapter 54.3, page 1931:
 *
 *   15004300  CAM_IMGO_BASE_ADDR  "DMA base addres register" (sic)
 *   15004304  CAM_IMGO_OFST_ADDR  "DMA offset address register"
 *   15004308  CAM_IMGO_XSIZE      "DMA XSIZE"
 *   1500430C  CAM_IMGO_YSIZE      "DMA YSIZE"
 *   15004310  CAM_IMGO_STRIDE     "DMA stride"
 *   15004314  CAM_IMGO_CON        "DMA control register"
 *   15004318  CAM_IMGO_CON2       "DMA control register 2"
 *   1500431C  CAM_IMGO_CROP       "DMA crop function register"
 *
 * The offsets are corroborated exactly by the vendor register block
 * hardware/include/mtkcam/drv/isp_reg.h:8973-8980, whose comments read
 * "// 4300" through "// 431C" in the same order.
 */
#define CAM_IMGO_BASE_ADDR		0x300
#define CAM_IMGO_OFST_ADDR		0x304
#define CAM_IMGO_XSIZE			0x308
#define CAM_IMGO_YSIZE			0x30c
#define CAM_IMGO_STRIDE			0x310
#define CAM_IMGO_CON			0x314
#define CAM_IMGO_CON2			0x318
#define CAM_IMGO_CROP			0x31c

/*
 * CAM_IMGO_CON is written as one opaque word: the data sheet documents the
 * register as "DMA control register" and gives no bit table for it, so its
 * fields cannot be named here.  0x08505050 is the value the vendor source
 * documents as the generic recipe (isp_function.h:128, "max_burst_len =
 * {1,2,4,8}"); the tuned variants in isp_function.cpp set fields no source
 * here explains, so they are not copied.
 */
#define CAM_IMGO_CON_VALUE		0x08505050u

/*
 * CAM_IMGO_CON2.  The vendor sets it to 0 for the "ultra-high" case
 * (isp_function.cpp:2117) and to a nonzero value alongside the tuned CON
 * (isp_function.cpp:2123).  There is no field table for it in chapter 54 either.
 * Zero is the value the vendor pairs with the CON this driver uses, and it is
 * the register's documented reset value, so it is written as a plain 0.
 */
#define CAM_IMGO_CON2_VALUE		0x00000000u

/*
 * The pixel-granularity shift.
 *
 * The vendor's XSIZE and STRIDE arithmetic divides by 4: XSIZE is computed
 * as (((w * pixel_byte + 3) >> 2) + 1 >> 1 << 1) - 1 and STRIDE as
 * (stride * pixel_byte) >> 2 (isp_function.cpp:2141-2142).  The shift
 * constant is named in isp_function.h:485:
 *
 *	#define CAM_ISP_PIXEL_BYTE_FP 2
 *
 * i.e. the register counts in quarter-pixel units.  With CAM_ISP_PIXEL_BYTE
 * at 2 (one 16-bit YUV422 pair, which is what CAM_OUT_FMT = 2 selects), a
 * quarter-pixel unit is exactly one byte, so the register values in bytes and
 * the byte counts agree -- but the shift is kept explicit below so the
 * arithmetic still reads as the vendor wrote it.
 */
#define CAM_ISP_PIXEL_BYTE_FP		2

/*
 * IMGO_XSIZE is a count *from* zero, so it is one less than the pixel count,
 * and the vendor additionally rounds the pixel count up to an even number
 * before subtracting one.  See mtk_cam_imgo_xsize() in mtk-cam.c for the
 * derivation and for why the rounding is there.
 */

/* ------------------------------------------------------------------ */
/* Pads                                                               */
/* ------------------------------------------------------------------ */

/*
 * Media pad ids for the CAM/ISP subdev.
 *
 * The order is load-bearing and is fixed here for the same three reasons
 * mtk-scam.h gives: the DT's cam_in is port@0 and so is pad 0 under the
 * core's default v4l2_subdev_get_fwnode_pad_1_to_1(); pads[] below is
 * initialised in this order and media_entity_pads_init() assigns each pad the
 * index it has there; and every pad_ops handler in mtk-cam.c indexes by pad
 * number.
 *
 * The source pad is the ISP's processed output: IMGO writes it into a
 * videobuf2 buffer, which the video node exposes as /dev/videoN.
 */
enum {
	CAM_PAD_SINK = 0,	/* from the SCAM adaptor */
	CAM_PAD_SRC,		/* to the video node, via IMGO */
	CAM_PAD_NUM,
};

/*
 * The bus code CAM and SCAM agree on.
 *
 * mtk-scam.c uses MEDIA_BUS_FMT_SBGGR10_1X10 (mtk-scam.c:385) and documents
 * that SCAM parses the CSD without converting the payload, so the sink and
 * source codes are the same.  CAM takes the same code on its sink, so
 * S_FMT on CAM's sink cannot conflict with SCAM's source, and CAM answers
 * CAM_OUT_FMT (post-CDP, processed) on its source pad.
 *
 * The sink code is the sensor's, and it is Bayer 10-bit packed: the only
 * sensor this platform ever shipped is the A5142, which is
 * SENSOR_OUTPUT_FORMAT_RAW_B over MIPI with a 10-bit payload
 * Per data sheet ch. 54.1 the block takes RAW in and emits YUV to DRAM, which
 * CAM_CTL_FMT_SEL's sub_mode = RAW agrees with.
 */
#define CAM_MBUS_CODE_SINK			MEDIA_BUS_FMT_SBGGR10_1X10
#define CAM_MBUS_CODE_SRC			MEDIA_BUS_FMT_YUYV8_1X16

/*
 * Geometry bounds.
 *
 * The MT6589 data sheet's TPIPE width/height fields (CAM_CTL_TPIPE,
 * page 1967-1968) are 10-bit and 12-bit fields but the CAM_CTL_*_SIZE
 * registers are described in chapter 54.3's register summary as "DMA XSIZE" /
 * "DMA YSIZE" with no stated limit, so there is no datasheet-derived bound to
 * quote here.  The upper bound below is instead a self-imposed one: 2048 is the
 * documented range of the neighbouring G2D block's destination scan window
 * ("WIDTH ... Unit: Pixel. 12-bit unsigned integer, range:[1, 2048]" and the
 * same for HEIGHT, G2D_DST_CON, data sheet page 1919), and it is the widest
 * geometry this driver's own untested DMA programming should be trusted with.
 * It is a guard rail, not a hardware limit, and nothing in the data sheet says
 * the IMGO engine could not do more.  The 1280x960 seed matches mtk-scam.h's
 * SCAM_DEFAULT_* so the graph starts out consistent across the bridge.
 */
#define CAM_MIN_WIDTH			16
#define CAM_MIN_HEIGHT			16
#define CAM_MAX_WIDTH			2048
#define CAM_MAX_HEIGHT			2048
#define CAM_DEFAULT_WIDTH		1280
#define CAM_DEFAULT_HEIGHT		960

/*
 * Bytes per pixel on the IMGO path.
 *
 * CAM_OUT_FMT = 2 is documented as "422 1 plane" for a YUV output (data sheet
 * page 1955, and the vendor's own _FMT_YUV422_1P_ == 2 at
 * isp_function.h:455), i.e. two bytes per pixel packed into one plane.  That is
 * the format of CAM's *output*, which is what IMGO writes, so it is what sizes
 * the buffer.
 *
 * Note this is NOT the same quantity as the sensor's input pixel: cam_in_fmt is
 * Bayer 10 (see CAM_FMT_SEL_BAYER10 above) and the two are deliberately
 * different, because the bytes that reach DRAM come out of the pipeline, not in
 * off the sensor.  Nothing in between is implemented, so on this board the
 * engine never actually converts one into the other.
 *
 * Rests on the cam_out_fmt encoding above (YUV422 1 plane), not on a vendor
 * constant: the vendor's IMG2O channel is configured separately.
 */
#define CAM_IMGO_BYTES_PER_PIXEL	2

/* ------------------------------------------------------------------ */
/* Driver state                                                       */
/* ------------------------------------------------------------------ */

struct mtk_cam;

/*
 * A videobuf2 buffer queued by userspace, with the DMA address the engine was
 * programmed from.  Container_of() off the vb2 buffer; buf_struct_size in the
 * queue is what makes that safe.
 */
struct mtk_cam_vb2_buf {
	struct vb2_buffer buf;
	dma_addr_t dma_addr;
};

struct mtk_cam {
	struct device *dev;
	void __iomem *regs;
	int irq;

	struct v4l2_subdev sd;
	struct mutex lock;

	/*
	 * A separate spinlock for the interrupt path.
	 *
	 * cam->lock cannot be used there: it is a mutex, and the register writes
	 * it guards include a bounded udelay() poll in the software-reset
	 * handshake, which is not permitted in atomic context.  This one guards
	 * only the two fields the IRQ handler races with the streaming paths --
	 * streaming and active_buf -- plus the read-clear of the interrupt status
	 * register, so it is held for a handful of instructions and never across
	 * a sleeping call.
	 */
	spinlock_t irq_lock;

	/*
	 * The two media pads, indexed as CAM_PAD_* above.  Flags are filled
	 * in in probe, before media_entity_pads_init() gives it its index.
	 */
	struct media_pad pads[CAM_PAD_NUM];

	/*
	 * The video node, embedded rather than separately allocated.
	 *
	 * It is registered from internal_ops->registered(), which is the first
	 * point at which both the v4l2_device and the media_device are known --
	 * see mtk_cam_register().  Embedding it means video_set_drvdata() and
	 * video_devdata() can both recover cam with a plain container_of(),
	 * which is what to_mtk_cam_vdev() below does; there is no second
	 * allocation to keep in step and nothing to get wrong at unregister.
	 */
	struct video_device vdev_dev;

	/*
	 * The sink pad belonging to the VIDEO NODE, not to the subdev.
	 * video_register_device() only stores the pad array pointer inside the
	 * video_device, so the pad itself has to outlive registration, and it
	 * has to live in the driver rather than in the core.
	 */
	struct media_pad vdev_pad;

	/*
	 * video_device::queue is a *pointer*, not an embedded struct, and the
	 * core never allocates it: the driver owns the queue and assigns the
	 * pointer.  Keeping the queue here rather than separately allocated
	 * means there is exactly one lifetime to reason about.
	 */
	struct vb2_queue vq;

	/* The buffer currently programmed into the engine, if any. */
	struct mtk_cam_vb2_buf *active_buf;
	dma_addr_t active_dma_addr;

	/*
	 * The negotiated geometry for the video node, and the media pipeline
	 * that a stream runs over.
	 *
	 * Driver-global rather than per-file, which is the same trade-off
	 * mali-c55-capture.c makes for the same reason: the vb2_queue and its
	 * lock are global, so a per-file format would be negotiated against a
	 * shared queue and two openers could not both get buffers they had
	 * agreed on.  S_FMT is serialised against everything else by
	 * vdev_dev.lock, which is cam->lock.
	 */
	struct v4l2_mbus_framefmt sink_fmt;
	struct v4l2_mbus_framefmt src_fmt;
	struct media_pipeline pipe;

	bool streaming;
};

#define to_mtk_cam(sd) container_of(sd, struct mtk_cam, sd)
#define to_mtk_cam_vdev(vdev) container_of(vdev, struct mtk_cam, vdev_dev)

#endif /* _MTK_CAM_H */