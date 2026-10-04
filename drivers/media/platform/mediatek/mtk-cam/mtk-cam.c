// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 CAM/ISP top-level (control + IMGO DMA) driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * WHAT THIS DRIVER IS
 * -------------------
 * The MT6589 CAM block at 0x15004000 is the ISP.  This driver drives the part
 * of it that can be driven from documentation alone: the control plane (the
 * software-reset handshake, the block enable, the start/stop strobes) and the
 * IMGO output DMA engine, which is what actually moves a processed frame into
 * DRAM.  IMGO is the engine behind a V4L2 video node, so frames have somewhere
 * to go and userspace has something to open.
 *
 * WHAT IS STILL NOT HERE
 * ----------------------
 * The image processing itself -- the CPIPE stages, the memory sequencer, the
 * pixel-rate meters, the 3A statistics -- is a large register space with no
 * upstream MediaTek ISP driver to port and no way to test here.  None of the
 * CPIPE stage enables are programmed, because turning them on without writing
 * the pipeline would run blocks against unconfigured registers.  So CAM's
 * output is not a processed picture; the plumbing around the DMA engine is
 * complete and inert.
 *
 * WHAT THIS BOARD ACTUALLY HAS
 * ---------------------------
 * There is no camera sensor wired to this board, so there is no MIPI CSI-2
 * clock and lane data and no D-PHY.  The consequence for this driver is
 * concrete and is worth stating up front: mtk_cam_start() programs the engine
 * and strobes the start bits, but with no frame arriving from the sensor
 * nothing ever completes, so mtk_cam_irq() is never called and no buffer is
 * ever dequeued.  The node opens, formats negotiate, REQBUFS allocates, and
 * STREAMON succeeds; DQBUF simply blocks until userspace closes the file.
 * That is the intended behaviour for a pipeline with no source, not a defect,
 * and it is what makes this driver safe to load.
 *
 * THE IMGO BASE ADDRESS, AND A DATASHEET/CODE DIVERGENCE
 * -----------------------------------------------------
 * CAM_IMGO_BASE_ADDR is written here directly, at 0x15004300, which the data
 * sheet documents at cam.txt:1633-1635 as "CAM_IMGO_BASE_ADDR / DMA base
 * addres register".
 *
 * The vendor does NOT write it that way, and it is worth recording why, because
 * it looks like a contradiction and is not:
 *
 *   - Both direct writes in the vendor are commented out --
 *     isp_function.cpp:2110 (read-only path) and :2129 (physical path):
 *
 *	//ISP_IOCTL_WRITE_REG(..., CAM_IMGO_BASE_ADDR, this->dma_cfg.memBuf.base_pAddr);
 *	//ISP_WRITE_REG(m_pIspReg, CAM_IMGO_BASE_ADDR, this->dma_cfg.memBuf.base_pAddr);
 *
 *     so DMAO_B::_config() programs XSIZE, YSIZE, STRIDE, CON, CON2 and CROP
 *     and deliberately leaves the base address alone.
 *
 *   - On the vendor's real-time-buffer-control path the base address is
 *     instead delivered through a command queue held in DRAM
 *     (isp_drv.cpp:2471):
 *
 *	pcqrtbcring_va->rtbc_ring[i].cq_rtbc.imgo.inst = 0x00004300; //ISP_DRV_CQ_DUMMY_WR_TOKEN; //reg_4300
 *
 *     i.e. a CQ instruction whose operand is the register offset 0x4300, which
 *     is CAM_IMGO_BASE_ADDR.
 *
 *   - But the vendor's own CQ module table says IMGO is only registers
 *     0x4304..0x431C -- seven registers, starting *past* the base address.
 *     isp_drv.cpp:394 reads "{CAM_DMA_IMGO, 0x4304, 7}", and isp_drv.h:469
 *     spells out why in a comment on the same line:
 *
 *	CAM_DMA_IMGO,        // 7    15004304~1500431C  //remove base_addr
 *
 *     The same comment appears on CAM_DMA_IMG2O, which is the same situation.
 *
 * So on the vendor's *normal* (non-RTBC) pass2 path, CAM_IMGO_BASE_ADDR is
 * never written at all, by either mechanism.  That is a genuine gap in the
 * vendor code as shipped in this tree, not something this driver can reproduce
 * faithfully, and the honest choices were:
 *
 *   (a) skip the write, which means the engine reads whatever the reset value
 *       of 0x15004300 is and would DMA into address 0;
 *   (b) write the documented register, which is what the data sheet says the
 *       register is for.
 *
 * (b) is what this driver does.  It is the datasheet-documented behaviour and
 * it is the only choice that can produce a coherent buffer address, and the
 * vendor's own RTBC path proves the register is writable this way.  It has not
 * been run against hardware and is flagged as such in mtk_cam_config_imgo().
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/videodev2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include "mtk-cam.h"

static inline void cam_write(struct mtk_cam *cam, u32 reg, u32 val)
{
	writel(val, cam->regs + reg);
}

static inline u32 cam_read(struct mtk_cam *cam, u32 reg)
{
	return readl(cam->regs + reg);
}

/*
 * Bounded wait for the software-reset status flag.
 *
 * SW_RST_ST is documented as "0: DMA is busy, 1: DMA is idle.  HW reset can
 * be done", so after triggering we poll it until the block reports it is
 * idle.  The register's reset value is 0x00000002, i.e. SW_RST_ST already
 * reads 1 out of reset, so a plain blocking poll could in principle spin
 * forever on a block that never asserts it.  Bound it.
 */
#define CAM_SW_RST_TIMEOUT_US	100000	/* 100 ms */
#define CAM_SW_RST_POLL_US	10

/*
 * Perform the documented CAM software-reset handshake.
 *
 * This follows the vendor sequence, which the data sheet describes in prose
 * and which two independent vendor implementations agree on:
 *
 *   camera_isp.c:1144-1161   1 -> poll bit1 -> 0x5 -> 0x4 -> 0
 *   gdma_drv_6589_ctl.c:40   1 -> poll bit1 ->      0x4 -> 0
 *   isp_function.h:131-139   [0]=1, wait [1]==1, [2]=1, delay, [0]=0, [2]=0
 *
 * 1. assert  SW_RST_Trig (write-only bit 0 = 1)
 * 2. poll    SW_RST_ST (read-only bit 1) until it reads 1, bounded
 * 3. assert  HW_RST (bit 2) alongside the still-asserted trigger, then
 *    leave HW_RST asserted on its own
 * 4. clear everything (0)
 *
 * Steps 3 and 4 are the "async HW reset.  Resets all CAM modules, except for
 * register-setting modules" part of the sequence.  Never leaving the trigger
 * asserted matters: it holds the block in reset.
 *
 * The order of steps 2 and 3 is the point of the timeout check.  SW_RST_ST is
 * documented as "0: DMA is busy, 1: DMA is idle.  HW reset can be done", so
 * if the poll expires the DMA is still running and the block says so.  On
 * timeout nothing is written at all: the trigger is left as it is and the
 * caller is told the reset was not confirmed.
 *
 * Note the vendor poll loop in camera_isp.c:1157 is itself buggy -- it reads
 * "while ((!Reg) & ISP_REG_SW_CTL_SW_RST_STATUS)", which applies & before !
 * and so always exits immediately.  The copy in gdma_drv_6589_ctl.c:53 is the
 * correct one and is what the loop below follows, with the unbounded vendor
 * spin replaced by a timeout.
 *
 * TODO(unverified): the handshake is the documented and vendor-confirmed one,
 * but it has not been run against hardware.  The poll deadline is a guess.
 */
static int mtk_cam_sw_reset(struct mtk_cam *cam)
{
	unsigned int timeout;

	lockdep_assert_held(&cam->lock);

	cam_write(cam, CAM_CTL_SW_CTL, CAM_SW_CTL_SW_RST_TRIG);

	for (timeout = 0; timeout < CAM_SW_RST_TIMEOUT_US;
	     timeout += CAM_SW_RST_POLL_US) {
		if (cam_read(cam, CAM_CTL_SW_CTL) & CAM_SW_CTL_SW_RST_ST)
			break;

		udelay(CAM_SW_RST_POLL_US);
	}

	/*
	 * Decide before touching anything else.  Reaching here means the block
	 * never reported itself idle, so the DMA may still be mid-transfer and
	 * HW_RST would land in the middle of it.
	 */
	if (timeout == CAM_SW_RST_TIMEOUT_US) {
		dev_warn(cam->dev,
			 "CAM_SW_RST_ST still read 0 after %u us; not asserting HW_RST, reset not confirmed\n",
			 CAM_SW_RST_TIMEOUT_US);
		return -ETIMEDOUT;
	}

	/* DMA is idle: it is now safe to reset the block. */
	cam_write(cam, CAM_CTL_SW_CTL,
		  CAM_SW_CTL_SW_RST_TRIG | CAM_SW_CTL_HW_RST);
	cam_write(cam, CAM_CTL_SW_CTL, CAM_SW_CTL_HW_RST);
	cam_write(cam, CAM_CTL_SW_CTL, 0);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Format helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * The single fourcc CAM's video node offers.
 *
 * CAM_OUT_FMT = 2 is YUV422 in one plane (cam.txt:5100-5104, and the vendor's
 * own _FMT_YUV422_1P_ == 2 in isp_function.h:455), i.e. two bytes per pixel,
 * packed, no chroma subsampling in the vertical direction and no second
 * plane.  In V4L2 fourcc terms that is V4L2_PIX_FMT_UYVY.  One format is the
 * minimum viable state; the point of the driver is the DMA path, not a format
 * zoo, and offering a format the engine cannot produce would be worse.
 */
#define CAM_PIX_FMT	V4L2_PIX_FMT_UYVY

/*
 * Image size for one IMGO frame, in bytes.
 *
 * With a single packed plane this is the frame area times the pixel size,
 * which is exactly what VIDIOC_S_FMT's sizeimage has to be.
 */
static u32 mtk_cam_image_size(u32 width, u32 height)
{
	return width * height * CAM_IMGO_BYTES_PER_PIXEL;
}

/*
 * IMGO_XSIZE, in quarter-pixel units.
 *
 * The vendor computes it (isp_function.cpp:2141) as
 *
 *	((((w * pixel_byte + 3) >> CAM_ISP_PIXEL_BYTE_FP) + 1) >> 1 << 1) - 1
 *
 * broken down: convert the line to quarter-pixel units with the documented
 * shift (isp_function.h:485 defines CAM_ISP_PIXEL_BYTE_FP as 2), add one so the
 * count is "from" rather than "to", round up to an even number, then subtract
 * one.  The even-rounding is the engine's bus-width requirement: the data
 * sheet's only statement about XSIZE granularity anywhere is
 * "CAM_IMGI_XSIZE (from 0) - multiple of data bus width", and with a two-byte
 * pixel the granularity is a 16-bit pair.
 *
 * The data sheet has NO bit table for CAM_IMGO_XSIZE (chapter 54.3 lists the
 * register as "DMA XSIZE" and stops, cam.txt:1644), so this whole derivation is
 * vendor-sourced.  It is reproduced rather than replaced because there is
 * nothing to replace it with: inventing a simpler "width in pixels" would
 * produce a value the engine reads as something else.
 */
static u32 mtk_cam_imgo_xsize(u32 width)
{
	u32 pixels = width * CAM_IMGO_BYTES_PER_PIXEL;

	pixels = (pixels + (1 << CAM_ISP_PIXEL_BYTE_FP) - 1) >> CAM_ISP_PIXEL_BYTE_FP;

	/* Round the line up to a whole number of 16-bit pairs. */
	pixels = ((pixels + 1) >> 1) << 1;

	return pixels - 1;
}

/*
 * IMGO_YSIZE, in lines from zero.  The vendor writes h - 1 (isp_function.cpp
 * :2142) with no rounding, because a line count has no bus-width constraint.
 */
static u32 mtk_cam_imgo_ysize(u32 height)
{
	return height - 1;
}

/*
 * IMGO_STRIDE, in quarter-pixel units.
 *
 * The vendor writes (stride_in_pixels * pixel_byte) >> 2 (isp_function.cpp
 * :2143).  With a two-byte pixel and the shift being 2, the register value
 * equals the byte stride exactly, but the shift is kept so the arithmetic
 * still matches the vendor line it came from.
 */
static u32 mtk_cam_imgo_stride(u32 width)
{
	return (width * CAM_IMGO_BYTES_PER_PIXEL) >> CAM_ISP_PIXEL_BYTE_FP;
}

/* ------------------------------------------------------------------ */
/* Pad operations                                                     */
/* ------------------------------------------------------------------ */

/*
 * Seed both pads with a format the driver can actually honour.
 *
 * The sink takes the sensor's bus format, which is SBGGR10_1X10 because that
 * is what SCAM advertises on its source pad (mtk-scam.c:385) and SCAM does not
 * convert the payload.  The source takes the processed format, which is the
 * single-plane YUV422 this driver programs.
 *
 * internal_ops->init_state exists at all only because media_entity_pads_init()
 * ran before v4l2_subdev_init_finalize() in probe: the core allocates
 * state->pads only when entity.num_pads is non-zero.  That is the same
 * arrangement mtk-scam.c uses and the same reason.
 */
static int mtk_cam_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	struct v4l2_mbus_framefmt *fmt;

	lockdep_assert_held(&cam->lock);

	fmt = v4l2_subdev_state_get_format(state, CAM_PAD_SINK);
	if (!fmt)
		return -EINVAL;
	fmt->width = CAM_DEFAULT_WIDTH;
	fmt->height = CAM_DEFAULT_HEIGHT;
	fmt->code = CAM_MBUS_CODE_SINK;

	fmt = v4l2_subdev_state_get_format(state, CAM_PAD_SRC);
	if (!fmt)
		return -EINVAL;
	fmt->width = CAM_DEFAULT_WIDTH;
	fmt->height = CAM_DEFAULT_HEIGHT;
	fmt->code = CAM_MBUS_CODE_SRC;

	return 0;
}

/*
 * Clamp a requested frame into what the engine can address, and snap the
 * width to the 16-bit pair the DMA bus needs so that sizeimage and the XSIZE
 * the engine is programmed with agree.
 */
static void mtk_cam_clamp_fmt(struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = clamp_t(u32, fmt->width, CAM_MIN_WIDTH, CAM_MAX_WIDTH);
	fmt->height = clamp_t(u32, fmt->height, CAM_MIN_HEIGHT, CAM_MAX_HEIGHT);

	/*
	 * CAM_IMGO_XSIZE counts 16-bit pairs (see mtk_cam_imgo_xsize()), so an
	 * odd width would make the register value disagree with the byte count
	 * VIDIOC_S_FMT reports.  Round up rather than down so the buffer is
	 * never smaller than the programmed geometry.
	 */
	fmt->width = (fmt->width + 1) & ~1u;
}

/*
 * Forward declaration so mtk_cam_get_fmt() below can delegate to the one
 * implementation of "resolve this pad's format into the state".
 */
static int mtk_cam_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format);

/*
 * Hand out a format.
 *
 * For the sink this is the sensor's bus code, passed straight through: CAM
 * cannot know what the sensor will produce, and SCAM hands on whatever the
 * sensor negotiated without converting it (see mtk-scam.c).  For the source
 * this is the single format this driver programs.
 */
static int mtk_cam_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct mtk_cam *cam = to_mtk_cam(sd);

	lockdep_assert_held(&cam->lock);

	if (format->pad > CAM_PAD_SRC)
		return -EINVAL;

	/*
	 * Delegate to the single implementation of "resolve this pad's format
	 * into the state".  That is what makes a bare VIDIOC_SUBDEV_G_FMT return
	 * a usable answer for a caller that did not pre-fill the struct: the
	 * cached values win wherever the caller's are out of range.
	 */
	return mtk_cam_set_fmt(sd, state, format);
}

/*
 * Accept a format.
 *
 * Both pads clamp independently and the result is stored per pad, so a change
 * on one end cannot be quietly undone by the other.  Nothing is programmed
 * into hardware here: the registers are written at STREAMON, from whatever
 * format is current then, which is the standard arrangement and is what lets
 * S_FMT succeed while streaming is off.
 */
static int mtk_cam_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *format)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;
	struct v4l2_mbus_framefmt *cached;

	lockdep_assert_held(&cam->lock);

	if (format->pad > CAM_PAD_SRC)
		return -EINVAL;

	/*
	 * CAM_MBUS_CODE_SINK must survive untouched: it is the code SCAM's
	 * source pad advertises, and rejecting it would break the existing
	 * csi2-rx -> scam -> cam chain.  Only the source code is this driver's
	 * to choose, so an unknown code there is corrected to the one format
	 * the engine is programmed for.
	 */
	if (format->pad == CAM_PAD_SINK) {
		if (fmt->code != CAM_MBUS_CODE_SINK)
			fmt->code = CAM_MBUS_CODE_SINK;
	} else {
		if (fmt->code != CAM_MBUS_CODE_SRC)
			fmt->code = CAM_MBUS_CODE_SRC;
	}

	mtk_cam_clamp_fmt(fmt);

	cached = v4l2_subdev_state_get_format(state, format->pad);
	if (!cached)
		return -EINVAL;
	*cached = *fmt;

	return 0;
}

/*
 * Exactly one bus code per pad.
 *
 * Without these the core returns -ENOIOCTLCMD for a missing op (see
 * v4l2_subdev_call() in v4l2-subdev.c), so a userspace negotiation loop that
 * enumerates rather than guesses would skip CAM entirely -- the same
 * inconsistency mtk-scam.c records for itself.  With them, media-ctl and a
 * v4l2_subdev-based pipeline builder can discover that there is something to
 * negotiate on both pads.
 */
static int mtk_cam_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad > CAM_PAD_SRC)
		return -EINVAL;

	if (code->index)
		return -EINVAL;

	code->code = code->pad == CAM_PAD_SINK ? CAM_MBUS_CODE_SINK :
						 CAM_MBUS_CODE_SRC;

	return 0;
}

static int mtk_cam_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	u64 expected = fse->pad == CAM_PAD_SINK ? CAM_MBUS_CODE_SINK :
						   CAM_MBUS_CODE_SRC;

	if (fse->pad > CAM_PAD_SRC)
		return -EINVAL;

	if (fse->code != expected)
		return -EINVAL;

	if (fse->index)
		return -EINVAL;

	fse->min_width = CAM_MIN_WIDTH;
	fse->max_width = CAM_MAX_WIDTH;
	fse->min_height = CAM_MIN_HEIGHT;
	fse->max_height = CAM_MAX_HEIGHT;

	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_cam_pad_ops = {
	.enum_mbus_code = mtk_cam_enum_mbus_code,
	.enum_frame_size = mtk_cam_enum_frame_size,
	.get_fmt = mtk_cam_get_fmt,
	.set_fmt = mtk_cam_set_fmt,
};

/* ------------------------------------------------------------------ */
/* IMGO DMA programming                                               */
/* ------------------------------------------------------------------ */

/*
 * Program the IMGO engine for one buffer at one geometry.
 *
 * Order follows the vendor's own "DMA programming guide" in
 * isp_function.h:105-128, which lists: enable the channel in CAM_CTL_DMA_EN,
 * enable the error and done interrupts, then write BASE_ADDR, OFST_ADDR, XSIZE,
 * YSIZE, STRIDE, CON, CON2 and CROP.  Enabling the channel before the
 * geometry is written is what the vendor does, and it is safe only because the
 * engine is not running yet -- this is called from STREAMON, before the start
 * strobes.
 */
static void mtk_cam_config_imgo(struct mtk_cam *cam, dma_addr_t addr,
				u32 width, u32 height)
{
	unsigned long flags;

	lockdep_assert_held(&cam->lock);

	/*
	 * Channel enable.  CAM_CTL_DMA_EN is a shadow, but the data sheet
	 * drives it through write-one-to-set/_clear companions (see the header),
	 * so the shadow itself is never written.
	 */
	cam_write(cam, CAM_CTL_DMA_EN_SET, CAM_CTL_DMA_EN_IMGO_EN);

	/* Interrupts: the block-wide DMA error plus this channel's overrun. */
	cam_write(cam, CAM_CTL_INT_EN, CAM_CTL_INT_EN_DMA_ERR_EN |
					  CAM_CTL_INT_EN_IMGO_ERR_EN);

	/* And the per-channel done interrupt, whose status bit is bit 0. */
	cam_write(cam, CAM_CTL_DMA_INT, CAM_CTL_DMA_INT_IMGO_DONE_EN);

	/*
	 * The buffer address.  See the long note in the file header: the data
	 * sheet documents this register at cam.txt:1633 and the vendor's own
	 * RTBC path writes it with a CQ instruction of 0x00004300
	 * (isp_drv.cpp:2471), but the vendor's normal path never writes it at
	 * all.  Writing the documented register directly is the only choice
	 * that yields a coherent address.
	 *
	 * The register is 32 bits wide, so the address has to fit in 32.  Truncating
	 * silently would mean DMAing into the low half of a high address, which on
	 * this SoC is not mapped -- so refuse rather than truncate.  An address above
	 * 4 GiB means the buffer did not come from a mapping the CAM MMU can reach,
	 * which is a configuration error worth reporting instead of papering over.
	 *
	 * NOTE: the vendor resolves buffers through its own imem/MMU layer
	 * (IMemDrv::mapPhyAddr, called at isp_function.cpp:4529).  With the mainline
	 * DMA API the dma_addr the vb2 dma-contig allocator returns is already the
	 * device address for this device, so it is what belongs here -- but whether it
	 * is in fact bounded by 32 bits is exactly the kind of thing that cannot be
	 * confirmed without hardware, hence the explicit check rather than a bare cast.
	 */
	if (addr > U32_MAX) {
		dev_err(cam->dev,
			"IMGO buffer address 0x%llx does not fit CAM_IMGO_BASE_ADDR (32 bits); the CAM MMU mapping needs review\n",
			(unsigned long long)addr);
		return;
	}
	cam_write(cam, CAM_IMGO_BASE_ADDR, (u32)addr);

	/*
	 * CAM_IMGO_OFST_ADDR is left at its reset value of 0.  Every other
	 * channel's OFST register is written by the vendor as
	 * base_pAddr + ofst_addr folded into BASE_ADDR instead (e.g.
	 * isp_function.cpp:1816 writes CAM_IMGI_BASE_ADDR as the sum and never
	 * touches CAM_IMGI_OFST_ADDR), so 0 is the consistent value here too.
	 */
	cam_write(cam, CAM_IMGO_OFST_ADDR, 0);

	cam_write(cam, CAM_IMGO_XSIZE, mtk_cam_imgo_xsize(width));
	cam_write(cam, CAM_IMGO_YSIZE, mtk_cam_imgo_ysize(height));
	cam_write(cam, CAM_IMGO_STRIDE, mtk_cam_imgo_stride(width));
	cam_write(cam, CAM_IMGO_CON, CAM_IMGO_CON_VALUE);
	cam_write(cam, CAM_IMGO_CON2, CAM_IMGO_CON2_VALUE);

	/*
	 * CAM_IMGO_CROP is the crop *origin*, and the vendor writes
	 * (crop.y << 16) | crop.x (isp_function.cpp:2147).  There is no crop
	 * here -- the whole buffer is the frame -- so both are 0.
	 */
	cam_write(cam, CAM_IMGO_CROP, 0);

	/*
	 * Drain any interrupt status left over from a previous stream, BEFORE
	 * the caller strobes the start bits.
	 *
	 * CAM_CTL_INT_EN bit 31 (INT_WCLR_EN) is left at its reset value of 0,
	 * which the data sheet documents as "0: Read clear" (see the header), so
	 * this plain read is what clears CAM_CTL_DMA_INT.  It has to happen here
	 * because nothing else clears a status bit that was raised and then never
	 * serviced: mtk_cam_stop() only clears the *enable* (it writes
	 * CAM_CTL_DMA_INT = 0), and the status is otherwise cleared only by the
	 * read in mtk_cam_irq().
	 *
	 * A status bit surviving the STREAMOFF/STREAMON boundary is exactly the
	 * stale-IRQ case: the first interrupt of the new stream would find
	 * IMGO_DONE_ST already set, together with an active_buf that has just
	 * been programmed, and mtk_cam_irq() would then hand userspace a buffer
	 * that no frame was ever DMA'd into.  Nothing is started yet, so a
	 * genuine completion cannot be discarded by this read.
	 *
	 * Taken under irq_lock, the lock mtk_cam_irq() uses, so the drain cannot
	 * interleave with an interrupt that is already being serviced.  cam->lock
	 * is held here and is the outer lock, and the IRQ path takes only
	 * irq_lock, so cam->lock -> irq_lock is the only order that occurs.
	 */
	spin_lock_irqsave(&cam->irq_lock, flags);
	(void)cam_read(cam, CAM_CTL_DMA_INT);
	spin_unlock_irqrestore(&cam->irq_lock, flags);
}

/*
 * Disable IMGO.
 *
 * The enable is cleared with the write-one-to-clear companion, and the done
 * interrupt enable with a plain zero write (CAM_CTL_DMA_INT is a normal RW
 * register, not a shadow, so zero really does mean "no interrupts").
 */
static void mtk_cam_disable_imgo(struct mtk_cam *cam)
{
	lockdep_assert_held(&cam->lock);

	cam_write(cam, CAM_CTL_DMA_EN_CLR, CAM_CTL_DMA_EN_IMGO_EN);
	cam_write(cam, CAM_CTL_DMA_INT, 0);

	/*
	 * CAM_CTL_INT_EN is shared with every other channel, so it is not zeroed
	 * here -- that would disarm interrupts belonging to stages this driver
	 * does not own.  The two bits it set are left as they are; they are
	 * harmless once the channel is disabled, and the block is reset on the
	 * next start.
	 */
}

/* ------------------------------------------------------------------ */
/* Streaming                                                          */
/* ------------------------------------------------------------------ */

/*
 * Bring the block up and start it.
 *
 * The order is reset, then format selection, then channel enable, then start.
 * The vendor's pass2 path resets the block (isp_function.cpp:3159 area sets
 * CAM_CTL_START CQ0_START only after cam_cq_cfg() has pointed CAM_CTL_CQ0B_
 * BASEADDR at a descriptor in DRAM); this driver has no command queue to arm,
 * because the CQ descriptor format is only documented in a vendor header that
 * is not in this tree, so the format and geometry are written directly instead.
 *
 * The format select is programmed before anything is enabled, because
 * cam_out_fmt and cam_in_fmt determine what the whole pipeline does and an
 * engine enabled against the reset value of 0 -- which cam.txt:4955 documents
 * as "0: Reserved" for a YUV cam_out_fmt -- would be running an unconfigured
 * path.
 */
static int mtk_cam_start(struct mtk_cam *cam, u32 width, u32 height,
			 dma_addr_t addr)
{
	int ret;

	lockdep_assert_held(&cam->lock);

	ret = mtk_cam_sw_reset(cam);
	if (ret)
		return ret;

	/*
	 * CAM_CTL_FMT_SEL: scenario stays 0 (its reset value, meaning "no
	 * special scenario"), sub_mode is YUV, cam_in_fmt is 422 1-plane and
	 * cam_out_fmt is 422 1-plane.  See the header for each field's
	 * provenance.  The register is written whole rather than with
	 * read-modify-write because every field in it is named in the data
	 * sheet (scenario, sub_mode, cam_in_fmt, cam_out_fmt, tg1_fmt, tg2_fmt,
	 * two_pix, two_pix2, tg1_sw, tg2_sw) and the reset value 0 is the
	 * value the vendor leaves the untouched fields at.
	 */
	cam_write(cam, CAM_CTL_FMT_SEL,
		  CAM_SUB_MODE_YUV << CAM_CTL_FMT_SEL_SUB_MODE_SHIFT |
		  CAM_FMT_SEL_YUV422_1P << CAM_CTL_FMT_SEL_CAM_IN_FMT_SHIFT |
		  CAM_FMT_OUT_YUV422_1P << CAM_CTL_FMT_SEL_CAM_OUT_FMT_SHIFT);

	/*
	 * Enable the CAM block itself.  EN1 is a read/write shadow, but the data
	 * sheet drives it through write-one-to-set and write-one-to-clear
	 * companion registers (all bits type WO), so the shadow is never
	 * written directly.
	 *
	 * The remaining EN1 bits are the individual CPIPE stages.  They are
	 * deliberately left alone: turning them on without programming the
	 * pipeline would run blocks against unconfigured registers, and the
	 * pipeline is not implemented here anyway.
	 */
	cam_write(cam, CAM_CTL_EN1_SET, CAM_CTL_EN1_CAM_EN);

	/* Geometry and destination, now that the block is enabled and idle. */
	mtk_cam_config_imgo(cam, addr, width, height);

	/*
	 * Start.  Only PASS2 is strobed: PASS2 is the path that reads DRAM and
	 * writes DRAM (isp_function.h:74-75 describes the pass2 data flow as
	 * "Dram -> isp(raw) -> isp(rgb) -> isp(yuv) -> cdrz -> prz/vrz ->
	 * dispo/vido/vrzo -> Dram"), and it is the one whose output DMAs this
	 * driver programs.  PASS2B, FMT and PASS2C are separate paths for
	 * frame-buffer control and stereo, and CQ0/CQ0B are for a command queue
	 * this driver does not arm.
	 */
	cam_write(cam, CAM_CTL_START, CAM_CTL_START_PASS2_START);

	cam->streaming = true;

	return 0;
}

static int mtk_cam_stop(struct mtk_cam *cam)
{
	lockdep_assert_held(&cam->lock);

	cam->streaming = false;

	cam_write(cam, CAM_CTL_START, 0);

	mtk_cam_disable_imgo(cam);

	/*
	 * Clear the enable.  These are write-one-to-clear registers (see the
	 * header): the bit to clear is written as a 1, not a 0.
	 */
	cam_write(cam, CAM_CTL_EN1_CLR, CAM_CTL_EN1_CAM_EN);
	cam_write(cam, CAM_CTL_EN2_CLR, 0);

	return 0;
}

/*
 * Streaming entry point.
 *
 * The lock is taken here, not assumed.  call_s_stream() in v4l2-subdev.c calls
 * the driver callback directly, with no state object and so no lock of any
 * kind, so the lockdep_assert_held() in mtk_cam_sw_reset() -- which is what
 * protects the reset handshake and the enable writes below -- used to assert a
 * mutex nobody held.  Taking cam->lock here is what makes that assert true, and
 * it also serialises the CAM control strobes against any other path that
 * reaches them.
 *
 * A plain mutex is right: mtk_cam_sw_reset() busy-waits with udelay() over up
 * to 100 ms, so this path both sleeps and must not be in atomic context.
 */
static int mtk_cam_s_stream(struct v4l2_subdev *sd, int on)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	int ret = 0;

	mutex_lock(&cam->lock);

	if (on == cam->streaming)
		goto out_unlock;

	if (on)
		ret = mtk_cam_start(cam, CAM_DEFAULT_WIDTH, CAM_DEFAULT_HEIGHT,
				    cam->active_dma_addr);
	else
		ret = mtk_cam_stop(cam);

out_unlock:
	mutex_unlock(&cam->lock);
	return ret;
}

static const struct v4l2_subdev_video_ops mtk_cam_video_ops = {
	.s_stream = mtk_cam_s_stream,
};

/*
 * Defined below, alongside the .registered and .release callbacks that have to
 * live next to the video-node registration.  mtk_cam_subdev_ops needs the
 * pointer here, so the type has to be named before its definition.
 */
static const struct v4l2_subdev_internal_ops mtk_cam_internal_ops;

static const struct v4l2_subdev_ops mtk_cam_subdev_ops = {
	.video = &mtk_cam_video_ops,
	.pad = &mtk_cam_pad_ops,
};

/* ------------------------------------------------------------------ */
/* Videobuf2 queue                                                    */
/* ------------------------------------------------------------------ */

static inline struct mtk_cam_vb2_buf *mtk_cam_vb2_to_buf(struct vb2_buffer *vb)
{
	return container_of(vb, struct mtk_cam_vb2_buf, buf);
}

/*
 * Fill in the single format and size from the negotiated geometry.
 *
 * CAM has exactly one format and one plane, so this is nearly a constant.  The
 * work is turning the negotiated bus geometry into the bytesperline, framesize
 * and sizeimage that VIDIOC_S_FMT and VIDIOC_REQBUFS have to agree on, which is
 * what makes a buffer allocated here big enough for what the DMA engine is
 * later programmed to write.
 */
static int mtk_cam_vb2_queue_setup(struct vb2_queue *vq,
				    unsigned int *num_buffers,
				    unsigned int *num_planes,
				    unsigned int sizes[],
				    struct device *alloc_devs[])
{
	struct mtk_cam *cam = vq->drv_priv;
	u32 width, height;

	lockdep_assert_held(vq->lock);

	/*
	 * Take the geometry from the SOURCE pad, because that is the pad the
	 * buffer is sized for: IMGO is programmed from cam->src_fmt, and sizing
	 * from the sink would produce a buffer that does not match the registers.
	 * mtk_cam_clamp_fmt() has already been applied to both, so the width here
	 * is even and agrees with mtk_cam_imgo_xsize().
	 */
	width = cam->src_fmt.width;
	height = cam->src_fmt.height;

	/*
	 * VIDIOC_CREATE_BUFS() arrives here too, with the caller proposing its
	 * own sizes.  Reject anything smaller than what the engine is going to
	 * be programmed to write, rather than accepting a buffer that would then
	 * be overrun by the DMA.
	 */
	if (*num_planes) {
		if (*num_planes != 1)
			return -EINVAL;
		if (sizes[0] < mtk_cam_image_size(width, height))
			return -EINVAL;

		return 0;
	}

	*num_planes = 1;
	sizes[0] = mtk_cam_image_size(width, height);

	/*
	 * One contiguous allocation per buffer, from the device the queue is
	 * bound to.  alloc_devs[0] is supplied by the core and is what the
	 * dma-contig allocator uses; passing NULL here would make it allocate
	 * against the wrong device.
	 */
	if (alloc_devs)
		alloc_devs[0] = vq->dev;

	return 0;
}

static void mtk_cam_vb2_return_buffers(struct mtk_cam *cam,
				       enum vb2_buffer_state state)
{
	struct vb2_queue *q = &cam->vq;
	unsigned int num, i;

	lockdep_assert_held(&cam->lock);

	/*
	 * vb2_buffer_done() is only legal for a buffer the core still considers
	 * active, so the count has to come from vb2_get_num_buffers() -- this vb2
	 * version tracks allocated buffers in a bitmap and has no num_buffers
	 * field -- and each buffer has to be fetched with vb2_get_buffer(),
	 * which returns NULL for an index that is not allocated.
	 */
	num = vb2_get_num_buffers(q);

	for (i = 0; i < num; i++) {
		struct vb2_buffer *vb = vb2_get_buffer(q, i);

		if (!vb || vb->state != VB2_BUF_STATE_ACTIVE)
			continue;

		vb2_buffer_done(vb, state);
	}
}

/*
 * Return one buffer that userspace actually queued, or NULL if there is none.
 *
 * The set of queued buffers is what VB2_BUF_STATE_ACTIVE means: vb2_core_qbuf()
 * puts a QBUF'd buffer in that state and the core hands it to the driver in
 * buf_queue(), which is only reached for buffers userspace queued.  So this is
 * the list of QBUF'd indices, maintained by the core itself, and the driver does
 * not need to keep a second one.
 *
 * The scan cannot fail at STREAMON time: vb2_start_streaming() moves every
 * buffer on the queue's queued_list into the driver (via __enqueue_in_driver())
 * before calling us, and min_queued_buffers is 1, so at least one buffer is
 * ACTIVE whenever this runs.
 */
static struct vb2_buffer *mtk_cam_vb2_get_queued(struct vb2_queue *vq)
{
	struct mtk_cam *cam = vq->drv_priv;
	unsigned int num, i;

	lockdep_assert_held(&cam->lock);

	num = vb2_get_num_buffers(vq);

	for (i = 0; i < num; i++) {
		struct vb2_buffer *vb = vb2_get_buffer(vq, i);

		if (vb && vb->state == VB2_BUF_STATE_ACTIVE)
			return vb;
	}

	return NULL;
}

/*
 * Start or stop whatever feeds CAM's sink.
 *
 * v4l2_subdev_enable_streams() is NOT a call that walks the pipeline: it acts
 * on exactly one subdev and one of ITS pads.  Which is why the call this
 * replaces was guaranteed to fail.  It passed CAM_PAD_SINK, and the very first
 * sanity check in v4l2_subdev_enable_streams() is
 *
 *	if (!(sd->entity.pads[pad].flags & MEDIA_PAD_FL_SOURCE))
 *		return -EOPNOTSUPP;
 *
 * (v4l2-subdev.c:2321), and CAM_PAD_SINK is a SINK pad (mtk_cam_probe() sets
 * MEDIA_PAD_FL_SINK on it).  So every STREAMON returned -EOPNOTSUPP, and since
 * start_streaming() returning an error makes vb2_core_streamon() fail, the node
 * could never stream at all -- and the bug was invisible, because with no sensor
 * attached a failing STREAMON looks exactly like the documented "nothing ever
 * completes".
 *
 * Enabling CAM's own sink pad would also have been the wrong thing even if the
 * direction check passed.  Streams are enabled at the SOURCE of the pipeline
 * and the enable propagates *downstream* through each driver's own
 * enable_streams callback (see stm32_csi_enable_streams(),
 * drivers/media/platform/st/stm32/stm32-csi.c:702, which enables its own
 * upstream subdev and is itself called in turn from the next entity down).
 * The entity that starts the chain is therefore the one feeding CAM, found from
 * the remote end of CAM's sink link -- the same idiom rkcif-stream.c:280-285 and
 * dcmipp-input.c:420-437 use.
 *
 * Returns 0 when there is nothing to enable, which is a real case here and not
 * an error: CAM's sink has no peer when the graph has no upstream link (the
 * camera bridge's receiver sink is deliberately unlinked on this board), and
 * with no sensor there is no clock or lane data to start either.  CAM's own
 * registers are programmed by mtk_cam_start() regardless, so the IMGO engine
 * still comes up and the stream is simply inert, which is the documented
 * behaviour for a pipeline with no source.
 */
static int mtk_cam_set_upstream_streaming(struct mtk_cam *cam, bool on)
{
	struct media_pad *remote;
	struct v4l2_subdev *src_sd;
	u32 src_pad;
	int ret;

	lockdep_assert_held(&cam->lock);

	remote = media_pad_remote_pad_first(&cam->pads[CAM_PAD_SINK]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity))
		return 0;

	src_sd = media_entity_to_v4l2_subdev(remote->entity);
	src_pad = remote->index;

	if (on)
		ret = v4l2_subdev_enable_streams(src_sd, src_pad, BIT(0));
	else
		ret = v4l2_subdev_disable_streams(src_sd, src_pad, BIT(0));

	return ret;
}

/*
 * Program the engine for the first queued buffer and mark the pipeline
 * streaming.
 *
 * One buffer is programmed up front rather than one per frame, because with no
 * sensor on this board nothing ever completes: mtk_cam_irq() below is the only
 * thing that would advance to the next buffer, and it needs a frame that will
 * not arrive.  Programming exactly one buffer means the engine always has a
 * coherent, in-range destination address, which is the property that makes
 * STREAMON safe here; arming a ring of them would be several valid addresses of
 * which none is ever used.
 */
static int mtk_cam_vb2_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct mtk_cam *cam = vq->drv_priv;
	struct vb2_buffer *vb;
	struct media_pipeline *pipe;
	u32 width, height;
	unsigned long flags;
	int ret;

	lockdep_assert_held(&cam->lock);

	if (!count)
		return -ENODEV;

	/*
	 * Take a buffer the engine is allowed to write into, i.e. one userspace
	 * queued.  It used to be vq->bufs[0], which is the first *allocated*
	 * buffer, not the first *queued* one: VB2 only guarantees that some buffer
	 * is queued before STREAMON, so with REQBUFS 4 / QBUF 2 / STREAMON,
	 * bufs[0] may be a buffer userspace is still holding, and the DMA would
	 * land in memory that is not even mapped for the engine.
	 */
	vb = mtk_cam_vb2_get_queued(vq);
	if (!vb)
		return -ENODEV;

	mtk_cam_vb2_to_buf(vb)->dma_addr =
		vb2_dma_contig_plane_dma_addr(vb, 0);

	width = cam->src_fmt.width;
	height = cam->src_fmt.height;

	/*
	 * Start the media pipeline BEFORE the hardware.  The reverse order would
	 * let a frame arrive for a pad the graph does not yet consider
	 * streaming; the framework's contract is that the pipeline is marked
	 * first so every entity in it agrees about what is happening.
	 *
	 * This uses the explicit-pad forms rather than video_device_pipeline_*
	 * because those helpers require the video node's own entity to have
	 * exactly one pad -- see v4l2-dev.c:1192-1204, which returns -ENODEV
	 * otherwise.  CAM's subdev has two pads, so the helpers would always
	 * fail.
	 */
	pipe = &cam->pipe;
	ret = media_pipeline_start(&cam->pads[CAM_PAD_SRC], pipe);
	if (ret)
		return ret;

	ret = video_device_pipeline_start(&cam->vdev_dev, pipe);
	if (ret) {
		media_pipeline_stop(&cam->pads[CAM_PAD_SRC]);
		return ret;
	}

	/*
	 * Now start the upstream chain, which is what turns the receiver and
	 * SCAM on.  This is the entity feeding CAM, not CAM itself; see
	 * mtk_cam_set_upstream_streaming() for why the pad and the direction
	 * matter here.
	 *
	 * Started before mtk_cam_start() so that, if the upstream entity fails
	 * to start, CAM's engine is never strobed at all.
	 */
	ret = mtk_cam_set_upstream_streaming(cam, true);
	if (ret)
		goto err_pipeline;

	ret = mtk_cam_start(cam, width, height,
			    mtk_cam_vb2_to_buf(vb)->dma_addr);
	if (ret)
		goto err_upstream;

	/*
 * Retain the buffer so it is not handed back to userspace while the engine may
 * still be writing to it.  Without a sensor it never completes, so this is the
 * buffer that sits in VB2_BUF_STATE_ACTIVE until STREAMOFF, and
 * stop_streaming() is what returns it.
 *
 * Published under irq_lock, which is the lock mtk_cam_irq() uses, so the two
 * cannot race.  cam->lock is held for this whole function, so the buffer
 * cannot be freed while the field is being set.
 */
	spin_lock_irqsave(&cam->irq_lock, flags);
	cam->active_buf = mtk_cam_vb2_to_buf(vb);
	cam->active_dma_addr = cam->active_buf->dma_addr;
	spin_unlock_irqrestore(&cam->irq_lock, flags);

	return 0;

err_upstream:
	mtk_cam_set_upstream_streaming(cam, false);
err_pipeline:
	video_device_pipeline_stop(&cam->vdev_dev);
	media_pipeline_stop(&cam->pads[CAM_PAD_SRC]);

	return ret;
}

static void mtk_cam_vb2_stop_streaming(struct vb2_queue *vq)
{
	struct mtk_cam *cam = vq->drv_priv;
	unsigned long flags;

	lockdep_assert_held(&cam->lock);

	mtk_cam_stop(cam);

	/*
	 * Return the unfinished buffers as ERROR, not DONE.  No frame completed
	 * -- mtk_cam_irq() is the only thing that marks a buffer DONE and with no
	 * sensor feeding this board it never runs -- so DONE here would hand
	 * userspace a buffer it would read as a complete, valid picture when it
	 * holds a partial or empty DMA.  ERROR is the honest report: the operation
	 * on this buffer ended without producing a frame, and V4L2 says as much.
	 *
	 * It used to be DONE, on the reasoning that nothing had gone wrong so ERROR
	 * would wrongly claim corruption.  But DONE is equally a claim about the
	 * frame's contents (vb2 fills bytesused/flags and userspace is expected to
	 * consume it), and that claim is the one that is actually false.
	 */
	mtk_cam_vb2_return_buffers(cam, VB2_BUF_STATE_ERROR);

	/*
	 * Stop the upstream chain before the pipeline is torn down, in the
	 * reverse of the start order.  The return value is dropped on purpose:
	 * vb2 stop_streaming() returns void, so there is nowhere to report a
	 * teardown failure, and the remaining steps (stopping the pipeline and
	 * clearing active_buf) must happen regardless.  The engine itself has
	 * already been stopped above, so the worst case is an upstream entity
	 * left running with nothing consuming it, which is recoverable by the
	 * next STREAMON or by closing the file.
	 */
	mtk_cam_set_upstream_streaming(cam, false);

	video_device_pipeline_stop(&cam->vdev_dev);
	media_pipeline_stop(&cam->pads[CAM_PAD_SRC]);

	/*
	 * Cleared under irq_lock for the same reason it is set there: this is the
	 * lock mtk_cam_irq() takes, and a matching pair is what makes the two
	 * paths safe against each other.  The hardware is already stopped and the
	 * buffers returned above, so nothing can be in flight.
	 */
	spin_lock_irqsave(&cam->irq_lock, flags);
	cam->active_buf = NULL;
	cam->active_dma_addr = 0;
	spin_unlock_irqrestore(&cam->irq_lock, flags);
}

/*
 * A buffer reached the engine.
 *
 * Called from VB2's QBUF path, i.e. when userspace offers a buffer.  All this
 * does is cache the DMA address; the registers are not touched.
 *
 * The destination is deliberately NOT reprogrammed here while streaming.
 * This used to be
 *
 *	if (cam->streaming)
 *		mtk_cam_config_imgo(cam, cam_buf->dma_addr, ...);
 *
 * which rewrites CAM_IMGO_BASE_ADDR from a QBUF, i.e. in the middle of a frame,
 * with nothing establishing that the engine is not mid-transfer into the old
 * address.  A buffer arriving there may belong to a frame already in flight,
 * and moving the destination under it splits one frame across two buffers with
 * no way for userspace to know.
 *
 * Deferring the reprogram to the frame-end path instead was considered and
 * rejected: mtk_cam_irq() runs in atomic context, while mtk_cam_config_imgo()
 * asserts cam->lock (a mutex) and can call dev_err(), so programming from there
 * would need a second, lock-free variant of the function and a next-buffer
 * selection policy the driver does not otherwise have.  Not programming at all
 * is both smaller and correct for this driver: the engine is programmed once,
 * from start_streaming(), for the single buffer that stream uses (see the note
 * there), and that buffer is retained until STREAMOFF, so the destination stays
 * valid and in-range for the whole stream.  A later QBUF simply replaces what
 * would have been used after the next frame.
 */
static void mtk_cam_vb2_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct mtk_cam *cam = vq->drv_priv;
	struct mtk_cam_vb2_buf *cam_buf = mtk_cam_vb2_to_buf(vb);

	lockdep_assert_held(&cam->lock);

	cam_buf->dma_addr = vb2_dma_contig_plane_dma_addr(vb, 0);
}

static const struct vb2_ops mtk_cam_vb2_ops = {
	.queue_setup	= mtk_cam_vb2_queue_setup,
	.buf_queue	= mtk_cam_vb2_buf_queue,
	.start_streaming = mtk_cam_vb2_start_streaming,
	.stop_streaming	= mtk_cam_vb2_stop_streaming,
};

/* ------------------------------------------------------------------ */
/* Video node                                                         */
/* ------------------------------------------------------------------ */

static int mtk_cam_querycap(struct file *file, void *priv,
			    struct v4l2_capability *cap)
{
	strscpy(cap->driver, "mtk-cam", sizeof(cap->driver));
	strscpy(cap->card, "MT6589 CAM/ISP (IMGO)", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:mtk-cam", sizeof(cap->bus_info));
	cap->version = KERNEL_VERSION(0, 1, 0);
	cap->device_caps = video_devdata(file)->device_caps;

	return 0;
}

static int mtk_cam_enum_fmt_video(struct file *file, void *priv,
				  struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = CAM_PIX_FMT;

	/*
	 * This kernel's struct v4l2_fmtdesc (uapi/linux/videodev2.h) has no
	 * .field and no .colorspace member -- it is
	 * index/type/flags/description[32]/pixelformat/mbus_code/reserved[3] --
	 * so there is nowhere here to report that the format is progressive-only.
	 * The equivalent information is carried in the VIDIOC_S_FMT reply instead,
	 * where v4l2_format.fmt.pix does have .field and .colorspace; see
	 * mtk_cam_s_fmt().
	 */
	strscpy(f->description, "YUV422 packed (UYVY)", sizeof(f->description));

	return 0;
}

/*
 * Settle on a concrete geometry and report it.
 *
 * One format, one plane, so bytesperline and sizeimage follow from the clamped
 * width and height.  The clamp is applied and then stored, so a caller that
 * asks for something the engine cannot do is told what it actually got rather
 * than silently receiving a buffer too small for the registers.
 *
 * A format change is refused while the queue holds buffers, because the buffer
 * sizes and the DMA geometry come from the SAME field and are computed at
 * different times: mtk_cam_vb2_queue_setup() sizes an allocation with
 * mtk_cam_image_size(cam->src_fmt...) at REQBUFS time, while the engine is
 * programmed with mtk_cam_config_imgo(cam, addr, width, height) at STREAMON
 * time.  Overwriting src_fmt in between makes the programmed scan window larger
 * than the buffer the engine was handed, i.e. a DMA overrun, with nothing in the
 * sequence able to notice.
 */
static int mtk_cam_s_fmt(struct file *file, void *priv,
			 struct v4l2_format *v4l2_fmt)
{
	struct mtk_cam *cam = to_mtk_cam_vdev(video_devdata(file));
	struct v4l2_pix_format *fmt = &v4l2_fmt->fmt.pix;
	struct v4l2_mbus_framefmt mbus;

	if (v4l2_fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	/*
	 * Reject the change rather than deferring it.  This node advertises
	 * V4L2_CAP_STREAMING, so userspace may issue VIDIOC_S_FMT at any point,
	 * including in the middle of a running stream, and there is nothing to
	 * "apply it later" -- the only thing the driver could do with a new
	 * geometry is reprogram the engine mid-frame, which mtk_cam_vb2_buf_queue()
	 * explicitly declines to do (see the comment there).  -EBUSY is the in-tree
	 * convention for exactly this and is what makes the error actionable:
	 * STREAMOFF (or VIDIOC_REQBUFS with count 0) then S_FMT succeeds.
	 *
	 * vb2_is_busy() is the check the rest of this tree uses
	 * (rkcif-stream.c:364, ti/vpe/vpe.c:1739).  It is true from the moment
	 * REQBUFS allocates until REQBUFS(0)/queue release drops them, so it covers
	 * a running stream AND the window between REQBUFS and STREAMON where
	 * buffers are already allocated and sized but nothing is running.  The
	 * latter is the case that matters most here: that is exactly when a size
	 * change silently invalidates the existing allocations, and it is not
	 * covered by cam->streaming.
	 *
	 * Safe to read without further locking: VIDIOC_S_FMT is serialised by
	 * vdev_dev.lock, which is cam->lock, and q->is_busy is only written under
	 * the vb2 queue lock, which is that same mutex (see mtk_cam_register()).
	 */
	if (vb2_is_busy(&cam->vq))
		return -EBUSY;

	if (fmt->pixelformat != CAM_PIX_FMT)
		fmt->pixelformat = CAM_PIX_FMT;

	mbus.width = fmt->width;
	mbus.height = fmt->height;
	mbus.code = CAM_MBUS_CODE_SRC;
	mtk_cam_clamp_fmt(&mbus);

	cam->src_fmt = mbus;

	fmt->width = mbus.width;
	fmt->height = mbus.height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt->bytesperline = mtk_cam_imgo_stride(mbus.width);
	fmt->sizeimage = mtk_cam_image_size(mbus.width, mbus.height);

	v4l2_fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return 0;
}

static int mtk_cam_g_fmt(struct file *file, void *priv,
			 struct v4l2_format *v4l2_fmt)
{
	struct mtk_cam *cam = to_mtk_cam_vdev(video_devdata(file));
	struct v4l2_pix_format *fmt = &v4l2_fmt->fmt.pix;

	if (v4l2_fmt->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	memset(fmt, 0, sizeof(*fmt));
	fmt->width = cam->src_fmt.width;
	fmt->height = cam->src_fmt.height;
	fmt->pixelformat = CAM_PIX_FMT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt->bytesperline = mtk_cam_imgo_stride(cam->src_fmt.width);
	fmt->sizeimage = mtk_cam_image_size(cam->src_fmt.width,
					    cam->src_fmt.height);

	v4l2_fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return 0;
}

/*
 * Try a format, reporting the clamped result back through v4l2_fmt itself.
 *
 * This kernel's handler takes three arguments, not the four-argument
 * (file, priv, in, out) shape newer kernels use: see
 * media/v4l2-ioctl.h:376, where vidioc_try_fmt_vid_cap is declared as
 *
 *	int (*vidioc_try_fmt_vid_cap)(struct file *file, void *priv,
 *				      struct v4l2_format *f);
 *
 * so the supported format is reported by writing it back into *f.  That is
 * also why it must NOT simply call mtk_cam_s_fmt(): S_FMT commits the format to
 * the driver, whereas TRY_FMT is required to leave the driver's state alone.
 * So the clamp runs on a local and only the caller's struct is touched.
 */
static int mtk_cam_try_fmt(struct file *file, void *priv,
			   struct v4l2_format *v4l2_fmt)
{
	struct v4l2_pix_format *fmt = &v4l2_fmt->fmt.pix;
	struct v4l2_mbus_framefmt mbus;

	mbus.width = fmt->width;
	mbus.height = fmt->height;
	mbus.code = CAM_MBUS_CODE_SRC;
	mtk_cam_clamp_fmt(&mbus);

	fmt->width = mbus.width;
	fmt->height = mbus.height;
	fmt->pixelformat = CAM_PIX_FMT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_DEFAULT;
	fmt->bytesperline = mtk_cam_imgo_stride(mbus.width);
	fmt->sizeimage = mtk_cam_image_size(mbus.width, mbus.height);

	v4l2_fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	return 0;
}

/*
 * The format ioctls, under the names this kernel uses.
 *
 * This tree's struct v4l2_ioctl_ops (media/v4l2-ioctl.h:296-380) spells the
 * single-planar handlers per *buffer type*, not the older flat names:
 * vidioc_enum_fmt_vid_cap / _g_fmt_vid_cap / _s_fmt_vid_cap /
 * _try_fmt_vid_cap, with separate _out variants.  The _cap ones are the correct
 * choice here because the queue is V4L2_BUF_TYPE_VIDEO_CAPTURE; the _out
 * variants exist for a node that produces video and are left NULL, which the
 * core reports as -ENOTTY rather than accepting the wrong ioctl.
 */
static const struct v4l2_ioctl_ops mtk_cam_ioctl_ops = {
	.vidioc_querycap		= mtk_cam_querycap,
	.vidioc_enum_fmt_vid_cap	= mtk_cam_enum_fmt_video,
	.vidioc_g_fmt_vid_cap		= mtk_cam_g_fmt,
	.vidioc_s_fmt_vid_cap		= mtk_cam_s_fmt,
	.vidioc_try_fmt_vid_cap		= mtk_cam_try_fmt,
};

static const struct v4l2_file_operations mtk_cam_v4l2_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.release	= v4l2_fh_release,
	.open		= v4l2_fh_open,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

/*
 * Hand back a completed buffer.
 *
 * This is the one path that could deliver a frame, and on this board it never
 * runs: the interrupt it keys off is CAM_CTL_DMA_INT.IMGO_DONE_ST, which the
 * engine sets only when a frame has actually been DMA'd, and with no sensor
 * there is no frame.  It is written anyway, so that the moment a sensor and a
 * D-PHY land the path exists and the only remaining question is whether the DMA
 * programming itself is right.
 *
 * Returns true only when an IMGO completion was actually consumed, which is
 * what lets mtk_cam_dev_irq() report IRQ_NONE for an interrupt that was not
 * ours.  Note the asymmetry with the read: the status register is read (and so
 * cleared) on every call regardless of the return value, but a completion is
 * only ever acted on when the status bit was set.
 */
static bool mtk_cam_irq(struct mtk_cam *cam)
{
	unsigned long flags;
	u32 status;
	struct vb2_buffer *vb;
	dma_addr_t dma_addr;

	spin_lock_irqsave(&cam->irq_lock, flags);

	/*
	 * A plain read.  CAM_CTL_INT_EN bit 31 (INT_WCLR_EN) is left at its reset
	 * value of 0, which the data sheet documents as "0: Read clear"
	 * (cam.txt:5770-5780), so reading CAM_CTL_DMA_INT is what clears the
	 * status bits.  Reading into a local and testing that local is what makes
	 * the clearing happen exactly once, and it has to happen before anything
	 * else so an interrupt for another channel is still cleared.
	 */
	status = cam_read(cam, CAM_CTL_DMA_INT);

	if (!(status & CAM_CTL_DMA_INT_IMGO_DONE_ST) || !cam->active_buf) {
		spin_unlock_irqrestore(&cam->irq_lock, flags);
		return false;
	}

	/*
	 * Take a reference on the in-flight buffer and clear the pointer before
	 * dropping the lock, so a concurrent QBUF cannot reprogram the engine to
	 * a buffer that is about to be handed to userspace.
	 */
	dma_addr = cam->active_dma_addr;
	vb = &cam->active_buf->buf;
	cam->active_buf = NULL;

	vb2_set_plane_payload(vb, 0,
			     mtk_cam_image_size(cam->src_fmt.width,
						cam->src_fmt.height));
	/*
	 * struct vb2_buffer has no .sequence in this kernel (media/videobuf2-core.h
	 * :252); the frame counter a DQBUF reports lives in vb2_buffer.timestamp,
	 * which vb2 fills from the queue's timestamp_flags.  The buffer address
	 * is read here only to keep the programmed destination visible in a
	 * debugger; the register was already written by start_streaming(), and
	 * nothing rewrites it while the stream runs.
	 */
	(void)dma_addr;

	spin_unlock_irqrestore(&cam->irq_lock, flags);

	/*
	 * Done outside the lock: vb2_buffer_done() wakes a waiter, and a woken
	 * DQBUF runs immediately in the waiter's own context, so doing that here
	 * would run userspace-triggered work under this driver's spinlock.
	 */
	vb2_buffer_done(vb, VB2_BUF_STATE_DONE);

	return true;
}

/* ------------------------------------------------------------------ */
/* Registration                                                       */
/* ------------------------------------------------------------------ */

/*
 * The IRQ handler.
 *
 * The line is shared (probe passes IRQF_SHARED), so the return value matters:
 * returning IRQ_HANDLED for an interrupt this driver did not consume steals it
 * from every other handler on the same line and keeps the line from being
 * re-asserted, which on a shared ISP interrupt can wedge the other channel
 * permanently.  IRQ_NONE is the right answer when there is no IMGO completion
 * of ours to act on, and it lets the next handler on the line run.
 *
 * The status register is still READ for every interrupt on the line whether or
 * not it concerns us, because the read is what performs the read-clear the data
 * sheet describes (see mtk_cam_irq()); a status bit raised for a channel this
 * driver does not own is thereby cleared instead of left to re-fire forever.
 *
 * "No IMGO completion" covers two cases, both of which must report IRQ_NONE:
 * no status bit at all (a spurious or foreign interrupt), and a status bit with
 * no active buffer (STREAMOFF already happened, so there is nothing to hand
 * back and the completion is stale by construction).
 */
static irqreturn_t mtk_cam_dev_irq(int irq, void *dev_id)
{
	struct mtk_cam *cam = dev_id;

	return mtk_cam_irq(cam) ? IRQ_HANDLED : IRQ_NONE;
}

/*
 * Register the video node.
 *
 * Called from internal_ops->registered(), which is the ONLY point at which the
 * node can be created.  That callback runs inside
 * __v4l2_device_register_subdev() at v4l2-device.c:149-152, which is after the
 * core has set sd->v4l2_dev (:133) and after it has registered the entity into
 * the media_device (:142-145).  Both have to be true before
 * video_register_device() can be called, because it creates the interface link
 * that puts the node in the media graph:
 *
 *	if (vdev->v4l2_dev->mdev) {
 *		link = media_create_intf_link(...);
 *
 * Registering from probe instead would need the notifier's v4l2_device, which
 * this driver does not have, and would try to create that link before the
 * entity existed.
 *
 * The matching teardown is .release, which the framework calls from
 * v4l2_subdev_release() (v4l2-device.c:175-183) on both paths: from
 * v4l2_device_unregister_subdev() when a devnode exists (:290-292), and
 * directly after .unregistered when one does not.  So the node is torn down by
 * the graph driver's v4l2_device_unregister() with no ordering games here.
 */
static int mtk_cam_register(struct v4l2_subdev *sd)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	struct vb2_queue *vq = &cam->vq;
	int ret;

	/*
	 * The video node gets exactly one pad, and it is a SINK: it consumes what
	 * CAM's source pad produces.  This entity is what makes the IMGO DMA
	 * output a real endpoint in the graph rather than a pad with nothing
	 * attached to it.
	 *
	 * It must be a sink and not the same entity as the subdev: the subdev
	 * already has CAM_PAD_SINK, and the interface this creates links the
	 * video node to the subdev's source, which is the shape the V4L2
	 * subdev-plus-video-node topology requires.
	 */
	cam->vdev_pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&cam->vdev_dev.entity, 1,
				     &cam->vdev_pad);
	if (ret)
		return ret;

	cam->vdev_dev.entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;

	/*
	 * The queue is a single global capture queue.  vb2_queue_init() (not
	 * vb2_init_queue(), which does not exist in this tree) sets up the
	 * queue against a driver-global drv_priv and lock, which is the same
	 * arrangement mali-c55-capture.c uses for the same reason.
	 *
	 * video_device::queue is a pointer the core never fills in, so it must
	 * be assigned here before anything reads it.  Reading it first is a NULL
	 * dereference: the core dereferences it while registering the node.
	 */
	cam->vdev_dev.queue = vq;

	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_DMABUF;
	vq->drv_priv = cam;
	vq->mem_ops = &vb2_dma_contig_memops;
	vq->ops = &mtk_cam_vb2_ops;
	vq->buf_struct_size = sizeof(struct mtk_cam_vb2_buf);
	vq->min_queued_buffers = 1;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	/*
	 * The queue lock is the same mutex as vdev_dev.lock, so a VIDIOC_S_FMT
	 * racing a VIDIOC_STREAMON cannot leave the DMA programmed for a
	 * geometry the buffers were not sized for.
	 */
	vq->lock = &cam->lock;
	vq->dev = cam->dev;

	ret = vb2_queue_init(vq);
	if (ret)
		goto err_entity;

	strscpy(cam->vdev_dev.name, "mtk-cam-isp", sizeof(cam->vdev_dev.name));

	cam->vdev_dev.fops = &mtk_cam_v4l2_fops;
	cam->vdev_dev.ioctl_ops = &mtk_cam_ioctl_ops;
	cam->vdev_dev.release = video_device_release_empty;
	cam->vdev_dev.minor = -1;
	cam->vdev_dev.lock = &cam->lock;
	cam->vdev_dev.v4l2_dev = cam->sd.v4l2_dev;
	/*
	 * No V4L2_CAP_READWRITE: there is no .read in mtk_cam_v4l2_fops and no
	 * vidioc_read in the ioctl ops, so read() on this node returns an error
	 * from the core rather than frames.  Advertising it is a false claim -
	 * userspace reads that bit as "streaming readback is available".
	 */
	cam->vdev_dev.device_caps = V4L2_CAP_VIDEO_CAPTURE |
				     V4L2_CAP_STREAMING |
				     V4L2_CAP_IO_MC;
	video_set_drvdata(&cam->vdev_dev, cam);

	/*
	 * There is no formats, ctrl_handler or internal_ops for the video node:
	 * it has one format and no controls of its own.  The controls a capture
	 * node would want (exposure, gain, test pattern) belong to the sensor,
	 * which is not in this tree.
	 */
	ret = video_register_device(&cam->vdev_dev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_queue;

	dev_info(cam->dev,
		 "registered CAM/ISP video node /dev/video%d on IMGO\n",
		 cam->vdev_dev.minor);

	return 0;

err_queue:
	vb2_queue_release(vq);
err_entity:
	media_entity_cleanup(&cam->vdev_dev.entity);

	return ret;
}

static void mtk_cam_unregister(struct v4l2_subdev *sd)
{
	struct mtk_cam *cam = to_mtk_cam(sd);
	struct vb2_queue *vq = &cam->vq;

	/*
	 * Stop the hardware before the node goes away.  The reverse order would
	 * leave a queue that can still be handed buffers with the engine no
	 * longer enabled.
	 */
	mutex_lock(&cam->lock);
	if (cam->streaming)
		mtk_cam_stop(cam);
	mutex_unlock(&cam->lock);

	video_unregister_device(&cam->vdev_dev);
	vb2_queue_release(vq);
	media_entity_cleanup(&cam->vdev_dev.entity);
}

/*
 * .registered and .release alongside .init_state.
 *
 * This replaces the earlier mtk_cam_internal_ops, which had init_state only:
 * registering the video node needs a registered/release pair, and init_state
 * still belongs here because media_entity_pads_init() ran before
 * v4l2_subdev_init_finalize(), so the core has state->pads to seed.
 */
static const struct v4l2_subdev_internal_ops mtk_cam_internal_ops = {
	.init_state	= mtk_cam_init_state,
	.registered	= mtk_cam_register,
	.release	= mtk_cam_unregister,
};

static int mtk_cam_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_cam *cam;
	int ret;

	cam = devm_kzalloc(dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->dev = dev;
	platform_set_drvdata(pdev, cam);
	dev_set_drvdata(dev, cam);
	mutex_init(&cam->lock);
	spin_lock_init(&cam->irq_lock);

	cam->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cam->regs))
		return PTR_ERR(cam->regs);

	/*
	 * v4l2_subdev_init() first, then everything it leaves to the driver.
	 *
	 * It has to come first because it is the only thing that initialises
	 * sd.name, and __v4l2_device_register_subdev() in
	 * drivers/media/v4l2-core/v4l2-device.c rejects any subdev whose name
	 * is empty:
	 *
	 *	if (!v4l2_dev || !sd || sd->v4l2_dev || !sd->name[0])
	 *		return -EINVAL;
	 *
	 * so a subdev registered without it never joins the V4L2 device, and
	 * because that is the async notifier's path the media graph would never
	 * see this node either -- whatever else was fixed in probe.  Assigning
	 * the subdev fields by hand, as this used to, left sd.name all zeroes
	 * because none of those fields alias sd.name.
	 *
	 * What v4l2_subdev_init() already does, and so must not be repeated:
	 * it zeroes sd.name, points sd.ops at the ops passed here, sets
	 * sd.v4l2_dev = NULL, resets sd.flags and sd.grp_id, clears
	 * dev_priv/host_priv/privacy_led, initialises sd.list and
	 * sd.async_subdev_endpoint_list, and -- because CONFIG_MEDIA_CONTROLLER
	 * is set -- makes sd.entity.name *point at* sd.name and sets
	 * sd.entity.obj_type = MEDIA_ENT_TYPE_V4L2_SUBDEV and
	 * sd.entity.function = MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN.
	 *
	 * Note in particular that entity.name is not a separate copy: it is the
	 * same array as sd.name, so writing dev_name() into entity.name is
	 * exactly what fills sd.name in, which is why this used to appear to
	 * work right up until registration refused it.  Setting sd.name
	 * explicitly below and leaving entity.name alone is therefore both
	 * sufficient and non-duplicative.
	 *
	 * internal_ops goes on sd->ops, so v4l2_subdev_init_finalize() below
	 * picks it up and __v4l2_device_register_subdev() calls .registered() on
	 * it -- which is where the video node is created.
	 */
	v4l2_subdev_init(&cam->sd, &mtk_cam_subdev_ops);
	strscpy(cam->sd.name, dev_name(dev), sizeof(cam->sd.name));

	/*
	 * internal_ops is a field of struct v4l2_subdev, not of
	 * v4l2_subdev_ops, and v4l2_subdev_init() does NOT clear it -- it only
	 * zeroes the fields the core owns (name, ops, v4l2_dev, flags, ...).
	 * The devm allocation already zeroed it, so it has to be set here or
	 * .init_state, .registered and .release never run and no video node is
	 * ever created.
	 *
	 * v4l2_subdev_init_finalize() below then picks this up, and
	 * __v4l2_device_register_subdev() calls .registered() on it, which is
	 * where the video node is created.
	 */
	cam->sd.internal_ops = &mtk_cam_internal_ops;

	/*
	 * sd.dev is what makes this subdev matchable.  See the long note in
	 * mtk-scam.c: v4l2_async_register_subdev() only derives sd->fwnode from
	 * dev_fwnode(sd->dev) when sd->dev is set, so leaving it NULL would leave
	 * the fwnode NULL, and a subdev with no fwnode is skipped by
	 * v4l2_async_find_match() and never joins the graph -- which for CAM
	 * specifically means the SCAM -> CAM link could never be created even
	 * though CAM's own sink pad exists.  The same fwnode is what
	 * v4l2_create_fwnode_links() walks to derive that link.
	 */
	cam->sd.dev = dev;

	/*
	 * CAM has pad_ops and a video node now, and both are driven through
	 * cam->lock, which is what state_lock points at.  v4l2_subdev_init()
	 * does not touch state_lock, so this is still the driver's to set.
	 */
	cam->sd.state_lock = &cam->lock;

	/*
	 * CAM is the ISP: it is where the sensor's frames are meant to be
	 * processed, so MEDIA_ENT_F_PROC_VIDEO_ISP is the honest description, and
	 * it is the same value the other in-tree ISP subdevs use
	 * (mali-c55-isp.c:584).
	 */
	cam->sd.entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;

	/*
	 * Create both pads.  The DT's cam_in is port@0, which resolves to pad
	 * index 0, so the sink is the one SCAM's source endpoint links to, and
	 * the source is pad 1.
	 *
	 * The source pad is new.  CAM used to be sink-only, which made it the
	 * terminal entity of the graph with nothing able to consume what it
	 * produced.  With a source pad, the video node registered from .registered
	 * has something to attach to and the IMGO DMA has a destination that
	 * means something.
	 */
	cam->pads[CAM_PAD_SINK].flags = MEDIA_PAD_FL_SINK;
	cam->pads[CAM_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&cam->sd.entity, CAM_PAD_NUM, cam->pads);
	if (ret)
		return dev_err_probe(dev, ret, "failed to init entity pads\n");

	ret = v4l2_subdev_init_finalize(&cam->sd);
	if (ret)
		return dev_err_probe(dev, ret, "subdev init error\n");

	/*
	 * Seed the driver-global geometry, so a caller that opens the node and
	 * goes straight to REQBUFS without an S_FMT gets a sane buffer size
	 * rather than a zero-length one.  These are the same values
	 * init_state() puts in the subdev state, which is what keeps the subdev
	 * and the node agreeing on the starting geometry.
	 */
	cam->sink_fmt.width = CAM_DEFAULT_WIDTH;
	cam->sink_fmt.height = CAM_DEFAULT_HEIGHT;
	cam->sink_fmt.code = CAM_MBUS_CODE_SINK;
	cam->src_fmt.width = CAM_DEFAULT_WIDTH;
	cam->src_fmt.height = CAM_DEFAULT_HEIGHT;
	cam->src_fmt.code = CAM_MBUS_CODE_SRC;

	/*
	 * CAM_CTL_START resets to 0.  Checking it catches a wrong reg property at
	 * probe rather than at first frame.
	 */
	ret = cam_read(cam, CAM_CTL_START);
	if (ret)
		dev_warn(dev,
			 "CAM_CTL_START reads 0x%08x, expected 0 on reset; check the reg property\n",
			 ret);

	/*
	 * The interrupt is optional.  There is no sensor on this board, so the
	 * only thing that would raise it is an IMGO done interrupt, which needs
	 * a frame; but the node has to be able to report a frame if one ever does
	 * arrive, and a DT without "interrupts" must not fail probe.
	 */
	cam->irq = platform_get_irq(pdev, 0);
	if (cam->irq < 0)
		cam->irq = 0;

	ret = v4l2_async_register_subdev(&cam->sd);
	if (ret)
		goto err_subdev_cleanup;

	if (cam->irq) {
		ret = devm_request_irq(dev, cam->irq, mtk_cam_dev_irq,
				       IRQF_SHARED, dev_name(dev), cam);
		if (ret)
			goto err_unregister_subdev;
	}

	/*
	 * The video node is NOT registered here.  It is registered from
	 * .registered, which fires inside v4l2_async_register_subdev() -> the
	 * notifier's bind -> __v4l2_device_register_subdev(); see
	 * mtk_cam_register() for why that is the only point where both
	 * v4l2_dev and the media_device exist.
	 *
	 * So whether a node appears at all depends on this subdev binding to the
	 * camera graph, which is the same dependency the pads and the links
	 * already have.
	 */
	dev_warn(dev,
		 "registering a CAM/ISP scaffold: control plane, IMGO DMA and a video node; the CPIPE pipeline itself is not implemented\n");

	return 0;

err_unregister_subdev:
	v4l2_async_unregister_subdev(&cam->sd);
err_subdev_cleanup:
	/*
	 * Release the active state allocated by v4l2_subdev_init_finalize().
	 * The pads live in the devm allocation and go with it.  Every error
	 * return above the pads_init() call can return directly.
	 */
	v4l2_subdev_cleanup(&cam->sd);
	return ret;
}

static void mtk_cam_remove(struct platform_device *pdev)
{
	struct mtk_cam *cam = platform_get_drvdata(pdev);

	/*
	 * Stop the hardware before anything is torn down.
	 *
	 * v4l2_async_unregister_subdev() then detaches CAM from the graph
	 * driver's notifier, which runs v4l2_device_unregister_subdev() and so
	 * unregisters the entity from the media_device, drops the links into CAM,
	 * and -- via v4l2_subdev_release() -- calls .release, which is what
	 * unregisters the video node and releases the vb2 queue.
	 *
	 * v4l2_subdev_cleanup() afterwards releases the active state allocated by
	 * v4l2_subdev_init_finalize().  Without it that state leaks on every
	 * unbind/rebind.
	 *
	 * The pads live in the devm allocation of cam and go with it.
	 */
	mutex_lock(&cam->lock);
	if (cam->streaming)
		mtk_cam_stop(cam);
	mutex_unlock(&cam->lock);

	v4l2_async_unregister_subdev(&cam->sd);
	v4l2_subdev_cleanup(&cam->sd);
}

static const struct of_device_id mtk_cam_of_match[] = {
	{ .compatible = "mediatek,mt6589-cam" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_cam_of_match);

static struct platform_driver mtk_cam_driver = {
	.probe = mtk_cam_probe,
	.remove = mtk_cam_remove,
	.driver = {
		.name = "mtk-cam",
		.of_match_table = mtk_cam_of_match,
	},
};
module_platform_driver(mtk_cam_driver);

MODULE_DESCRIPTION("MediaTek MT6589 CAM/ISP control plane and IMGO DMA");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");