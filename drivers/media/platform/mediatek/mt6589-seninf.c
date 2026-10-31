// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 camera receive path: SENINF, MIPI-RX and the raw part of
 * the ISP pass 1, for the front camera of the Lenovo B8000-H.
 *
 * - MCLK: the TG1 phase counter of SENINF drives the CMMCLK pin.  It is a
 *   common clock here, which the sensor driver enables like any other.
 * - Capture: the sensor (OV9760, one lane) is wired to the CSI1 port, the
 *   "sub" sensor of the stock stack, whose data runs CSI1 -> SENINF2 -> TG2
 *   -> pass 1 (TG_SEL = TG2) -> IMGO DMA.  IMGO writes either 8-bit Bayer
 *   (the sensor's top 8 bits, a byte a pixel: V4L2's SBGGR8, the default)
 *   or RAW10 packed the MediaTek way (PAK: four pixels in five bytes, LSB
 *   first).  Pass 1 has no unpacked 16-bit output.  The buffer is
 *   physically contiguous: the M4U is not used, so the IMGO port of SMI
 *   larb4 works with physical addresses.
 *
 * The register sequences follow the stock Android 4.4 userspace
 * (libcamdrv SeninfDrvImp::initTg2CSI2/setTg2CSI2/setTg2*,
 * setTg1PhaseCounter; ISP_TOP_CTRL/DMAO_B of the MediaTek KK camera source)
 * and the stock kernel's camera_isp.c (IRQ, buffer switch at SOF).  See
 * docs/camera-front.md of device-lenovo-b8000h.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mc.h>
#include <media/videobuf2-dma-contig.h>

/*
 * MediaTek packed 10-bit Bayer: the fourcc of the out-of-tree mtk-isp P1
 * (MT8183 camsys) series, not in mainline's uapi.
 */
#ifndef V4L2_PIX_FMT_MTISP_SBGGR10
#define V4L2_PIX_FMT_MTISP_SBGGR10 v4l2_fourcc('M', 'B', 'B', 'A')
#endif

/* SENINF, 0x15008000 */
#define SENINF_TOP		0x000
#define SENINF_TOP_PCLK_EN	(BIT(10) | BIT(11))	/* SENINF2, SENINF1 */
#define SENINF2_CTRL		0x090
#define  SCTRL_MUX_SW_RST	BIT(0)
#define  SCTRL_IRQ_SW_RST	BIT(1)
#define  SCTRL_PIX_SEL		BIT(8)
#define  SCTRL_VSYNC_POL	BIT(9)
#define  SCTRL_HSYNC_POL	BIT(10)
#define  SCTRL_SRC_SEL		GENMASK(15, 12)
#define  SCTRL_FIFO_PUSH_EN	GENMASK(21, 16)
#define  SCTRL_FIFO_FLUSH_EN	GENMASK(27, 22)
#define  SCTRL_PAD2CAM		GENMASK(30, 28)
#define SENINF2_DEBUG_4		0x0ac	/* measured size: H [31:16], V [15:0] */
#define SENINF2_CSI2_CTRL	0x180
#define SENINF2_CSI2_DELAY	0x184
#define SENINF2_CSI2_INTEN	0x188
#define SENINF2_CSI2_INTSTA	0x18c
#define SENINF2_CSI2_DBG	0x198
#define  CSI2_DBG_HSRXDB_EN	(BIT(27) | BIT(28) | BIT(29))
#define TG1_PH_CNT		0x300
#define  PH_TGCLK_SEL		GENMASK(1, 0)
#define  PH_CLKFL_POL		BIT(2)
#define  PH_ADCLK_EN		BIT(29)
#define  PH_PCEN		BIT(31)
#define TG1_SEN_CK		0x304
#define  SEN_CK_CLKFL		GENMASK(5, 0)
#define  SEN_CK_CLKRS		GENMASK(13, 8)
#define  SEN_CK_CLKCNT		GENMASK(21, 16)

/* ISP, 0x15004000 */
#define CAM_CTL_EN1		0x004
#define  EN1_TG2_EN		BIT(1)
#define  EN1_PAK_EN		BIT(12)
#define  EN1_PAK2_EN		BIT(13)
#define  EN1_CAM_EN		BIT(30)
#define CAM_CTL_EN2		0x008
#define CAM_CTL_DMA_EN		0x00c
#define  DMA_IMGO_EN		BIT(0)
#define CAM_CTL_FMT_SEL		0x010
#define  FMT_SCENARIO		GENMASK(2, 0)
#define  FMT_CAM_OUT		GENMASK(15, 12)
#define  FMT_TG2_FMT		GENMASK(22, 20)
#define  FMT_TWO_PIX2		BIT(25)
#define CAM_CTL_SEL		0x018
#define  SEL_TDR_SEL		BIT(6)
#define  SEL_TG_SEL		BIT(8)
#define  SEL_EIS_RAW_SEL	BIT(16)
#define CAM_CTL_PIX_ID		0x01c
#define CAM_CTL_INT_EN		0x020
#define CAM_CTL_INT_STATUS	0x024	/* read to clear */
#define  INT_VS1		BIT(0)
#define  INT_TG1_ERR		BIT(4)
#define  INT_VS2		BIT(5)
#define  INT_TG2_ERR		BIT(9)
#define  INT_PASS1_DON		BIT(10)
#define  INT_PASS1_TG2_DON	BIT(11)
#define  INT_SOF1		BIT(12)
#define  INT_IMGO_ERR		BIT(20)
#define  INT_DMA_ERR		BIT(30)
#define CAM_CTL_DMA_INT		0x028	/* status [15:0] read to clear, enables [31:16] */
#define  DMA_IMGO_DONE_ST	BIT(0)
#define  DMA_IMGO_DONE_EN	BIT(16)
#define  DMA_SOF2_EN		BIT(26)
#define CAM_CTL_SW_CTL		0x05c
#define CAM_CTL_SPARE3		0x06c
#define CAM_CTL_MUX_SEL		0x074
#define CAM_CTL_MUX_SEL2	0x078
#define CAM_CTL_SRAM_MUX_CFG	0x07c
#define CAM_CTL_IMGO_FBC	0x0f4
#define CAM_CTL_CLK_EN		0x150
#define CAM_IMGO_BASE_ADDR	0x300
#define CAM_IMGO_OFST_ADDR	0x304
#define CAM_IMGO_XSIZE		0x308
#define CAM_IMGO_YSIZE		0x30c
#define CAM_IMGO_STRIDE		0x310
#define CAM_IMGO_CON		0x314
#define CAM_IMGO_CON2		0x318
#define CAM_IMGO_CROP		0x31c
#define CAM_IMGO_ERR_STAT	0x3c4
#define CAM_TG2_SEN_MODE	0x4b0
#define  TG_CMOS_EN		BIT(0)
#define  TG_DBL_DATA_BUS	BIT(1)
#define  TG_SOT_MODE		BIT(2)
#define  TG_SOF_SRC		GENMASK(9, 8)
#define CAM_TG2_VF_CON		0x4b4
#define  VF_VFDATA_EN		BIT(0)
#define  VF_SINGLE_MODE		BIT(1)
#define  VF_SP_DELAY		GENMASK(10, 8)
#define  VF_SPDELAY_MODE	BIT(12)
#define CAM_TG2_GRAB_PXL	0x4b8
#define CAM_TG2_GRAB_LIN	0x4bc
#define CAM_TG2_PATH_CFG	0x4c0
#define  PATH_SEN_IN_LSB	GENMASK(1, 0)
#define  PATH_JPGINF_EN		BIT(4)

/* MIPI-RX analog, 0x10012800 */
#define CSI1_CLK_LANE		0x14
#define CSI1_DATA_LANE0		0x18
#define CSI1_DATA_LANE1		0x1c
#define  LANE_LDO_OUT_EN	BIT(0)
#define  LANE_SUB_SEL		BIT(4)	/* CSI1 lanes to the sub sensor pads */
#define CSI_LDO_CORE		0x20
#define  CSI1_LDO_CORE_EN	BIT(1)
#define CSI_BG_CORE		0x24
#define  CSI_BG_CORE_EN		BIT(0)

/* MIPI-RX calibration, 0x1500C000 */
#define MIPI_RX_HW_CAL		0x38
#define  HW_CAL_START		BIT(2)
#define MIPI_RX_CSI1_CAL_ST	0x4c

#define MCLK_MAX_DIV		64
#define CSI2_SETTLE		14	/* OV9760: MIPIDataLowPwr2HighSpeedSettleDelayCount */
#define SENINF_MIN_BUFFERS	3

static const char * const seninf_clk_names[] = {
	"cam", "tg", "sen", "cam-smi", "larb", "smi",
};

struct seninf_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct mt6589_seninf {
	struct device *dev;
	void __iomem *base;		/* SENINF */
	void __iomem *isp;
	void __iomem *rx;		/* MIPI-RX analog */
	void __iomem *rxcal;		/* MIPI-RX calibration */
	int irq;
	struct clk_bulk_data clks[ARRAY_SIZE(seninf_clk_names)];
	struct clk_hw mclk;
	unsigned int div;		/* MCLK = tg / div, 2..64 */

	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;
	struct v4l2_subdev *sensor;
	u32 sensor_pad;

	struct video_device vdev;
	struct media_pad pad;
	struct vb2_queue queue;
	struct mutex lock;		/* vdev and queue */
	struct v4l2_pix_format fmt;

	spinlock_t irqlock;		/* the lists and the two buffer pointers */
	struct list_head queued;
	struct seninf_buffer *active;	/* IMGO writes it this frame */
	unsigned int sequence;
	bool streaming;
	bool resume_streaming;	/* stopped by system sleep, started on wake */
	u32 irq_status, dma_status;	/* seen while streaming, for dev_dbg */
	unsigned int frames, dropped, errors;
};

#define to_seninf(_hw) container_of(_hw, struct mt6589_seninf, mclk)

static void rmw(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

/* --- MCLK --------------------------------------------------------------- */

static unsigned long seninf_mclk_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	return parent_rate / to_seninf(hw)->div;
}

static unsigned int seninf_mclk_div(unsigned long rate, unsigned long parent)
{
	unsigned int div = DIV_ROUND_CLOSEST(parent, max(rate, 1UL));

	return clamp(div, 2U, MCLK_MAX_DIV);
}

static int seninf_mclk_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	req->rate = req->best_parent_rate /
		    seninf_mclk_div(req->rate, req->best_parent_rate);
	return 0;
}

static int seninf_mclk_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	to_seninf(hw)->div = seninf_mclk_div(rate, parent_rate);
	return 0;
}

static int seninf_mclk_prepare(struct clk_hw *hw)
{
	struct mt6589_seninf *s = to_seninf(hw);
	unsigned int cnt = s->div - 1;
	int ret;

	ret = pm_runtime_resume_and_get(s->dev);
	if (ret)
		return ret;

	rmw(s->base + SENINF_TOP, 0, SENINF_TOP_PCLK_EN);
	writel(FIELD_PREP(SEN_CK_CLKFL, (cnt + 1) / 2) |
	       FIELD_PREP(SEN_CK_CLKRS, 0) |
	       FIELD_PREP(SEN_CK_CLKCNT, cnt), s->base + TG1_SEN_CK);
	rmw(s->base + TG1_PH_CNT, PH_TGCLK_SEL | PH_CLKFL_POL,
	    FIELD_PREP(PH_TGCLK_SEL, 1) | ((cnt & 1) ? 0 : PH_CLKFL_POL) |
	    PH_ADCLK_EN | PH_PCEN);
	return 0;
}

static void seninf_mclk_unprepare(struct clk_hw *hw)
{
	struct mt6589_seninf *s = to_seninf(hw);

	rmw(s->base + TG1_PH_CNT, PH_ADCLK_EN | PH_PCEN, 0);
	pm_runtime_put(s->dev);
}

static const struct clk_ops seninf_mclk_ops = {
	.prepare = seninf_mclk_prepare,
	.unprepare = seninf_mclk_unprepare,
	.recalc_rate = seninf_mclk_recalc_rate,
	.determine_rate = seninf_mclk_determine_rate,
	.set_rate = seninf_mclk_set_rate,
};

static int seninf_register_mclk(struct mt6589_seninf *s)
{
	struct clk_init_data init = {};
	const char *parent;
	int ret;

	/* "tg" is image_sen_tg, fed by camtg_sel at 48 MHz. */
	parent = __clk_get_name(s->clks[1].clk);
	init.name = devm_kasprintf(s->dev, GFP_KERNEL, "%s-mclk", dev_name(s->dev));
	if (!init.name)
		return -ENOMEM;
	init.ops = &seninf_mclk_ops;
	init.parent_names = &parent;
	init.num_parents = 1;
	/* The divider is written at prepare: change it only while gated. */
	init.flags = CLK_SET_RATE_GATE;
	s->mclk.init = &init;
	s->div = 2;	/* 24 MHz until a consumer asks */

	ret = devm_clk_hw_register(s->dev, &s->mclk);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(s->dev, of_clk_hw_simple_get, &s->mclk);
}

/* --- Runtime PM: the clock gates of the camera block -------------------- */

static int seninf_runtime_resume(struct device *dev)
{
	struct mt6589_seninf *s = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(ARRAY_SIZE(s->clks), s->clks);
}

static int seninf_runtime_suspend(struct device *dev)
{
	struct mt6589_seninf *s = dev_get_drvdata(dev);

	rmw(s->base + SENINF_TOP, SENINF_TOP_PCLK_EN, 0);
	clk_bulk_disable_unprepare(ARRAY_SIZE(s->clks), s->clks);
	return 0;
}

/* --- Hardware: CSI1 -> SENINF2 -> TG2 -> IMGO ---------------------------- */

/* DMAO_B::_config: bytes a line, an even count. */
static u32 imgo_bytes(u32 fourcc, u32 width)
{
	if (fourcc == V4L2_PIX_FMT_SBGGR8)
		return ALIGN(width, 2);
	/* BAYER10: 1.25 bytes a pixel. */
	return ((((width * 5 + 3) >> 2) + 1) >> 1) << 1;
}

/* SeninfDrvImp::initTg2CSI2(1) for the sub sensor. */
static void seninf_rx_on(struct mt6589_seninf *s)
{
	u32 cal;

	rmw(s->base + SENINF2_CSI2_INTSTA, 0, 0x80);	/* CSI2_SPARE |= 4 */
	rmw(s->rx + CSI1_CLK_LANE, 0, LANE_SUB_SEL);
	rmw(s->rx + CSI1_DATA_LANE0, 0, LANE_SUB_SEL);
	rmw(s->rx + CSI1_DATA_LANE1, 0, LANE_SUB_SEL);
	rmw(s->rx + CSI_BG_CORE, 0, CSI_BG_CORE_EN);
	udelay(30);
	rmw(s->rx + CSI_LDO_CORE, 0, CSI1_LDO_CORE_EN);
	rmw(s->rx + CSI1_CLK_LANE, 0, LANE_LDO_OUT_EN);
	udelay(1);
	rmw(s->rx + CSI1_DATA_LANE0, 0, LANE_LDO_OUT_EN);
	rmw(s->rx + CSI1_DATA_LANE1, 0, LANE_LDO_OUT_EN);

	rmw(s->base + SENINF2_CSI2_DBG, 0, CSI2_DBG_HSRXDB_EN);
	rmw(s->rxcal + MIPI_RX_HW_CAL, 0, HW_CAL_START);
	usleep_range(500, 600);
	cal = readl(s->rxcal + MIPI_RX_CSI1_CAL_ST);
	if (!(cal & 0x10001))
		dev_warn(s->dev, "CSI1 calibration did not finish (%08x)\n", cal);
	rmw(s->base + SENINF2_CSI2_DBG, CSI2_DBG_HSRXDB_EN, 0);

	/* SeninfDrvImp::setTg2CSI2(0, 14, 0, 1, 0, 1, 1, 0): 1 lane, ECC. */
	rmw(s->base + SENINF2_CSI2_CTRL, BIT(0), 0);
	writel(CSI2_SETTLE << 16, s->base + SENINF2_CSI2_DELAY);
	rmw(s->base + SENINF2_CSI2_INTEN, 0, 0x7);
	rmw(s->base + SENINF2_CSI2_INTSTA, 0, 0x10);
	writel(0x2431, s->base + SENINF2_CSI2_CTRL);
}

static void seninf_rx_off(struct mt6589_seninf *s)
{
	rmw(s->base + SENINF2_CSI2_CTRL, BIT(0), 0);
	rmw(s->rx + CSI1_DATA_LANE1, LANE_LDO_OUT_EN | LANE_SUB_SEL, 0);
	rmw(s->rx + CSI1_DATA_LANE0, LANE_LDO_OUT_EN | LANE_SUB_SEL, 0);
	rmw(s->rx + CSI1_CLK_LANE, LANE_LDO_OUT_EN | LANE_SUB_SEL, 0);
	rmw(s->rx + CSI_LDO_CORE, CSI1_LDO_CORE_EN, 0);
	rmw(s->rx + CSI_BG_CORE, CSI_BG_CORE_EN, 0);
}

/* setTg2GrabRange, SensorModeCfg, InputCfg, ViewFinderMode. */
static void seninf_tg2_config(struct mt6589_seninf *s, u32 w, u32 h)
{
	void __iomem *isp = s->isp;

	writel(w << 16, isp + CAM_TG2_GRAB_PXL);
	writel(h << 16, isp + CAM_TG2_GRAB_LIN);

	rmw(s->base + SENINF2_CTRL, SCTRL_HSYNC_POL | SCTRL_VSYNC_POL, 0);
	rmw(isp + CAM_TG2_SEN_MODE, 0, TG_CMOS_EN | TG_SOT_MODE);

	rmw(s->base + SENINF2_CTRL, SCTRL_PAD2CAM | SCTRL_SRC_SEL, 0);	/* MIPI */
	rmw(isp + CAM_TG2_PATH_CFG, PATH_SEN_IN_LSB | PATH_JPGINF_EN, 0);
	rmw(isp + CAM_TG2_SEN_MODE, TG_SOF_SRC | TG_DBL_DATA_BUS, 0);
	rmw(s->base + SENINF2_CTRL,
	    SCTRL_FIFO_FLUSH_EN | SCTRL_FIFO_PUSH_EN | SCTRL_PIX_SEL,
	    FIELD_PREP(SCTRL_FIFO_FLUSH_EN, 0x1b) |
	    FIELD_PREP(SCTRL_FIFO_PUSH_EN, 0x1f));
	rmw(s->base + SENINF2_CTRL, 0, SCTRL_MUX_SW_RST | SCTRL_IRQ_SW_RST);
	rmw(s->base + SENINF2_CTRL, SCTRL_MUX_SW_RST | SCTRL_IRQ_SW_RST, 0);

	rmw(isp + CAM_TG2_VF_CON, VF_SINGLE_MODE | VF_SP_DELAY, VF_SPDELAY_MODE);
}

static int seninf_isp_reset(struct mt6589_seninf *s)
{
	u32 st;
	int ret;

	writel(1, s->isp + CAM_CTL_SW_CTL);
	ret = readl_poll_timeout(s->isp + CAM_CTL_SW_CTL, st, st & BIT(1), 10, 10000);
	writel(5, s->isp + CAM_CTL_SW_CTL);
	writel(4, s->isp + CAM_CTL_SW_CTL);
	writel(0, s->isp + CAM_CTL_SW_CTL);
	return ret;
}

/* ISP_TOP_CTRL::_config for VSS RAW on TG2, without the command queue. */
static void seninf_pass1_config(struct mt6589_seninf *s, dma_addr_t first)
{
	void __iomem *isp = s->isp;
	u32 w = s->fmt.width, h = s->fmt.height;
	bool raw8 = s->fmt.pixelformat == V4L2_PIX_FMT_SBGGR8;

	writel(EN1_TG2_EN | EN1_PAK_EN | EN1_PAK2_EN | EN1_CAM_EN, isp + CAM_CTL_EN1);
	writel(0, isp + CAM_CTL_EN2);
	writel(DMA_IMGO_EN, isp + CAM_CTL_DMA_EN);
	/*
	 * The sensor sends RAW10 (TG2_FMT 1); CAM_OUT_FMT is what IMGO writes:
	 * BAYER8 (0) keeps the top 8 bits, BAYER10 (1) packs all ten.  Without
	 * PAK there is no unpacked output: IMGO stops with a DMA error.
	 */
	writel(FIELD_PREP(FMT_SCENARIO, 6) | FIELD_PREP(FMT_CAM_OUT, raw8 ? 0 : 1) |
	       FIELD_PREP(FMT_TG2_FMT, 1), isp + CAM_CTL_FMT_SEL);
	/*
	 * No PASS1_DB_EN: with it the stock stack has the command queue load
	 * the double-buffered pass-1 registers; without a queue IMGO never gets
	 * its sizes and stops with a DMA error at the first frame.  Registers
	 * then take effect at once, so the buffer is switched between frames.
	 */
	writel(SEL_TDR_SEL | SEL_EIS_RAW_SEL | SEL_TG_SEL, isp + CAM_CTL_SEL);
	writel(0x00100008, isp + CAM_CTL_MUX_SEL);	/* AA_SEL_EN, AA_SEL */
	writel(0x40080108, isp + CAM_CTL_MUX_SEL2);	/* raw out before HRZ */
	writel(0x11000000, isp + CAM_CTL_SRAM_MUX_CFG);
	writel(0, isp + CAM_CTL_PIX_ID);	/* B first (OV9760 RAW_B) */
	writel(0x80, isp + CAM_CTL_SPARE3);
	writel(0x0001ffff, isp + CAM_CTL_CLK_EN);

	writel(0, isp + CAM_CTL_IMGO_FBC);	/* no FBC: switch buffers at SOF */
	writel(lower_32_bits(first), isp + CAM_IMGO_BASE_ADDR);
	writel(0, isp + CAM_IMGO_OFST_ADDR);
	writel(imgo_bytes(s->fmt.pixelformat, w) - 1, isp + CAM_IMGO_XSIZE);
	writel(h - 1, isp + CAM_IMGO_YSIZE);
	writel(s->fmt.bytesperline, isp + CAM_IMGO_STRIDE);
	writel(0x08141450, isp + CAM_IMGO_CON);
	writel(0x00141400, isp + CAM_IMGO_CON2);
	writel(0, isp + CAM_IMGO_CROP);

	readl(isp + CAM_CTL_INT_STATUS);	/* clear */
	readl(isp + CAM_CTL_DMA_INT);
	writel(INT_TG2_ERR | INT_IMGO_ERR | INT_DMA_ERR, isp + CAM_CTL_INT_EN);
	writel(DMA_IMGO_DONE_EN, isp + CAM_CTL_DMA_INT);
}

static void seninf_pass1_stop(struct mt6589_seninf *s)
{
	rmw(s->isp + CAM_TG2_VF_CON, VF_VFDATA_EN, 0);
	msleep(70);	/* let the frame in flight end: two frames at 30/s */
	writel(0, s->isp + CAM_CTL_INT_EN);
	writel(0, s->isp + CAM_CTL_DMA_INT);
	writel(0, s->isp + CAM_CTL_DMA_EN);
	writel(0, s->isp + CAM_CTL_EN1);
	rmw(s->isp + CAM_TG2_SEN_MODE, TG_CMOS_EN, 0);
	readl(s->isp + CAM_CTL_INT_STATUS);
	readl(s->isp + CAM_CTL_DMA_INT);
}

/* --- Buffers and the interrupt ------------------------------------------ */

static struct seninf_buffer *to_seninf_buffer(struct vb2_v4l2_buffer *vb)
{
	return container_of(vb, struct seninf_buffer, vb);
}

static struct seninf_buffer *seninf_take_queued(struct mt6589_seninf *s)
{
	struct seninf_buffer *b;

	b = list_first_entry_or_null(&s->queued, struct seninf_buffer, list);
	if (b)
		list_del(&b->list);
	return b;
}

static dma_addr_t seninf_buf_addr(struct seninf_buffer *b)
{
	return vb2_dma_contig_plane_dma_addr(&b->vb.vb2_buf, 0);
}

/*
 * Stop IMGO at once (in the IRQ, with irqlock held): after an IMGO or DMA
 * error the DMA must not keep writing, which could run past the buffer into
 * other memory. The program gets an error on its queue. (SENINF2_DEBUG_4
 * cannot check the frame size here: at IMGO done its line count already
 * reads 0.)
 */
static void seninf_halt(struct mt6589_seninf *s, const char *why, u32 val)
{
	rmw(s->isp + CAM_TG2_VF_CON, VF_VFDATA_EN, 0);
	rmw(s->isp + CAM_CTL_DMA_EN, DMA_IMGO_EN, 0);
	s->streaming = false;
	dev_err(s->dev, "capture stopped: %s (%08x)\n", why, val);
	vb2_queue_error(&s->queue);
}

static irqreturn_t seninf_irq(int irq, void *data)
{
	struct mt6589_seninf *s = data;
	struct seninf_buffer *done = NULL;
	u32 st, dma;

	st = readl(s->isp + CAM_CTL_INT_STATUS);
	dma = readl(s->isp + CAM_CTL_DMA_INT) & 0xffff;
	if (!st && !dma)
		return IRQ_NONE;

	spin_lock(&s->irqlock);
	s->irq_status |= st;
	s->dma_status |= dma;
	if (st & (INT_TG2_ERR | INT_IMGO_ERR | INT_DMA_ERR)) {
		/* The error bits stay set until the next reset: stop listening. */
		if (!s->errors++)
			dev_warn(s->dev, "pass 1 error: int %08x dma %04x imgo %08x\n",
				 st, dma, readl(s->isp + CAM_IMGO_ERR_STAT));
		rmw(s->isp + CAM_CTL_INT_EN, st & (INT_TG2_ERR | INT_IMGO_ERR | INT_DMA_ERR), 0);
		if (s->streaming && (st & (INT_IMGO_ERR | INT_DMA_ERR)))
			seninf_halt(s, "IMGO/DMA error", readl(s->isp + CAM_IMGO_ERR_STAT));
	}

	if (!s->streaming)
		goto out;


	/*
	 * IMGO done: the frame is in memory. Only then the buffer goes back
	 * (PASS1_TG2_DON may come before IMGO has written its last line).
	 * Between frames IMGO takes the next buffer at once; without one it
	 * writes the same buffer again and that frame is dropped.
	 */
	if (dma & DMA_IMGO_DONE_ST) {
		struct seninf_buffer *next = seninf_take_queued(s);

		if (next) {
			done = s->active;
			s->active = next;
			writel(lower_32_bits(seninf_buf_addr(next)),
			       s->isp + CAM_IMGO_BASE_ADDR);
		} else {
			s->dropped++;
		}
	}

	if (done) {
		done->vb.vb2_buf.timestamp = ktime_get_ns();
		done->vb.sequence = s->sequence++;
		done->vb.field = V4L2_FIELD_NONE;
		vb2_buffer_done(&done->vb.vb2_buf, s->errors ? VB2_BUF_STATE_ERROR :
							VB2_BUF_STATE_DONE);
		s->frames++;
	}
out:
	spin_unlock(&s->irqlock);
	return IRQ_HANDLED;
}

static void seninf_return_all(struct mt6589_seninf *s, enum vb2_buffer_state state)
{
	struct seninf_buffer *b, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&s->irqlock, flags);
	list_for_each_entry_safe(b, tmp, &s->queued, list) {
		list_del(&b->list);
		vb2_buffer_done(&b->vb.vb2_buf, state);
	}
	if (s->active)
		vb2_buffer_done(&s->active->vb.vb2_buf, state);
	s->active = NULL;
	spin_unlock_irqrestore(&s->irqlock, flags);
}

/* --- vb2 ----------------------------------------------------------------- */

static int seninf_queue_setup(struct vb2_queue *q, unsigned int *nbuffers,
			      unsigned int *nplanes, unsigned int sizes[],
			      struct device *alloc_devs[])
{
	struct mt6589_seninf *s = vb2_get_drv_priv(q);

	if (*nplanes)
		return sizes[0] < s->fmt.sizeimage ? -EINVAL : 0;
	*nplanes = 1;
	sizes[0] = s->fmt.sizeimage;
	return 0;
}

static int seninf_buf_prepare(struct vb2_buffer *vb)
{
	struct mt6589_seninf *s = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < s->fmt.sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, s->fmt.sizeimage);
	return 0;
}

static void seninf_buf_queue(struct vb2_buffer *vb)
{
	struct mt6589_seninf *s = vb2_get_drv_priv(vb->vb2_queue);
	struct seninf_buffer *b = to_seninf_buffer(to_vb2_v4l2_buffer(vb));
	unsigned long flags;

	spin_lock_irqsave(&s->irqlock, flags);
	list_add_tail(&b->list, &s->queued);
	spin_unlock_irqrestore(&s->irqlock, flags);
}

static int seninf_check_sensor_format(struct mt6589_seninf *s)
{
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = s->sensor_pad,
	};
	int ret;

	ret = v4l2_subdev_call_state_active(s->sensor, pad, get_fmt, &fmt);
	if (ret)
		return ret;
	if (fmt.format.width != s->fmt.width || fmt.format.height != s->fmt.height ||
	    fmt.format.code != MEDIA_BUS_FMT_SBGGR10_1X10) {
		dev_dbg(s->dev, "sensor %ux%u/%04x, video node %ux%u\n",
			fmt.format.width, fmt.format.height, fmt.format.code,
			s->fmt.width, s->fmt.height);
		return -EPIPE;
	}
	return 0;
}

/*
 * The receive path, the ISP and the sensor's stream, from the active buffer
 * on: the part of a start (and of a wake from system sleep) after the clocks
 * are on and a buffer is taken.
 */
static int seninf_hw_start(struct mt6589_seninf *s)
{
	unsigned long flags;
	int ret;

	rmw(s->base + SENINF_TOP, 0, SENINF_TOP_PCLK_EN);
	ret = seninf_isp_reset(s);
	if (ret)
		dev_warn(s->dev, "ISP reset did not finish\n");
	seninf_rx_on(s);
	seninf_tg2_config(s, s->fmt.width, s->fmt.height);
	seninf_pass1_config(s, seninf_buf_addr(s->active));

	spin_lock_irqsave(&s->irqlock, flags);
	s->streaming = true;
	spin_unlock_irqrestore(&s->irqlock, flags);
	enable_irq(s->irq);

	ret = v4l2_subdev_enable_streams(s->sensor, s->sensor_pad, BIT_ULL(0));
	if (ret) {
		spin_lock_irqsave(&s->irqlock, flags);
		s->streaming = false;
		spin_unlock_irqrestore(&s->irqlock, flags);
		seninf_pass1_stop(s);
		disable_irq(s->irq);
		seninf_rx_off(s);
		return ret;
	}

	rmw(s->isp + CAM_TG2_VF_CON, 0, VF_VFDATA_EN);
	return 0;
}

static void seninf_hw_stop(struct mt6589_seninf *s)
{
	unsigned long flags;

	rmw(s->isp + CAM_TG2_VF_CON, VF_VFDATA_EN, 0);
	v4l2_subdev_disable_streams(s->sensor, s->sensor_pad, BIT_ULL(0));

	spin_lock_irqsave(&s->irqlock, flags);
	s->streaming = false;
	spin_unlock_irqrestore(&s->irqlock, flags);

	seninf_pass1_stop(s);
	disable_irq(s->irq);
	seninf_rx_off(s);
}

static int seninf_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct mt6589_seninf *s = vb2_get_drv_priv(q);
	unsigned long flags;
	int ret;

	ret = s->sensor ? seninf_check_sensor_format(s) : -ENODEV;
	if (ret)
		goto err_return;

	ret = video_device_pipeline_alloc_start(&s->vdev);
	if (ret)
		goto err_return;

	ret = pm_runtime_resume_and_get(s->dev);
	if (ret)
		goto err_pipeline;

	spin_lock_irqsave(&s->irqlock, flags);
	s->active = seninf_take_queued(s);
	s->sequence = 0;
	s->frames = s->dropped = s->errors = 0;
	s->irq_status = s->dma_status = 0;
	spin_unlock_irqrestore(&s->irqlock, flags);
	if (!s->active) {
		ret = -ENOBUFS;
		goto err_pm;
	}

	ret = seninf_hw_start(s);
	if (ret)
		goto err_pm;
	return 0;

err_pm:
	pm_runtime_put(s->dev);
err_pipeline:
	video_device_pipeline_stop(&s->vdev);
err_return:
	seninf_return_all(s, VB2_BUF_STATE_QUEUED);
	return ret;
}

static void seninf_stop_streaming(struct vb2_queue *q)
{
	struct mt6589_seninf *s = vb2_get_drv_priv(q);
	u32 size;

	size = readl(s->base + SENINF2_DEBUG_4);
	/* Stopped by system sleep: the hardware is off already. */
	if (!s->resume_streaming)
		seninf_hw_stop(s);
	s->resume_streaming = false;
	pm_runtime_put(s->dev);
	video_device_pipeline_stop(&s->vdev);

	dev_dbg(s->dev, "stopped: %u frames, %u dropped, %u errors, int %08x dma %04x, SENINF2 saw %ux%u\n",
		s->frames, s->dropped, s->errors, s->irq_status, s->dma_status,
		size >> 16, size & 0xffff);
	seninf_return_all(s, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops seninf_vb2_ops = {
	.queue_setup = seninf_queue_setup,
	.buf_prepare = seninf_buf_prepare,
	.buf_queue = seninf_buf_queue,
	.start_streaming = seninf_start_streaming,
	.stop_streaming = seninf_stop_streaming,
};

/* --- V4L2 ioctls --------------------------------------------------------- */

static const struct {
	u32 fourcc;
	const char *name;
} seninf_formats[] = {
	{ V4L2_PIX_FMT_SBGGR8, "8-bit Bayer BGGR" },
	{ V4L2_PIX_FMT_MTISP_SBGGR10, "10-bit Bayer BGGR (MediaTek packed)" },
};

static void seninf_fill_fmt(struct v4l2_pix_format *pix, u32 w, u32 h)
{
	if (pix->pixelformat != V4L2_PIX_FMT_MTISP_SBGGR10)
		pix->pixelformat = V4L2_PIX_FMT_SBGGR8;
	pix->width = clamp_t(u32, ALIGN(w, 4), 64, 1472);
	pix->height = clamp_t(u32, ALIGN(h, 2), 48, 1104);
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = imgo_bytes(pix->pixelformat, pix->width);
	pix->sizeimage = pix->bytesperline * pix->height;
	pix->colorspace = V4L2_COLORSPACE_RAW;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	pix->quantization = V4L2_QUANTIZATION_DEFAULT;
	pix->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int seninf_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, "mt6589-seninf", sizeof(cap->driver));
	strscpy(cap->card, "MT6589 camera (front)", sizeof(cap->card));
	return 0;
}

static int seninf_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(seninf_formats) ||
	    (f->mbus_code && f->mbus_code != MEDIA_BUS_FMT_SBGGR10_1X10))
		return -EINVAL;
	f->pixelformat = seninf_formats[f->index].fourcc;
	strscpy(f->description, seninf_formats[f->index].name,
		sizeof(f->description));
	return 0;
}

static int seninf_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	unsigned int i;

	if (fsize->index)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(seninf_formats); i++)
		if (seninf_formats[i].fourcc == fsize->pixel_format)
			break;
	if (i == ARRAY_SIZE(seninf_formats))
		return -EINVAL;
	/* The limits of seninf_fill_fmt(). */
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 64;
	fsize->stepwise.max_width = 1472;
	fsize->stepwise.step_width = 4;
	fsize->stepwise.min_height = 48;
	fsize->stepwise.max_height = 1104;
	fsize->stepwise.step_height = 2;
	return 0;
}

static int seninf_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct mt6589_seninf *s = video_drvdata(file);

	f->fmt.pix = s->fmt;
	return 0;
}

static int seninf_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	seninf_fill_fmt(&f->fmt.pix, f->fmt.pix.width, f->fmt.pix.height);
	return 0;
}

static int seninf_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct mt6589_seninf *s = video_drvdata(file);

	if (vb2_is_busy(&s->queue))
		return -EBUSY;
	seninf_fill_fmt(&f->fmt.pix, f->fmt.pix.width, f->fmt.pix.height);
	s->fmt = f->fmt.pix;
	return 0;
}

static int seninf_enum_input(struct file *file, void *fh, struct v4l2_input *inp)
{
	if (inp->index)
		return -EINVAL;
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	strscpy(inp->name, "Front camera", sizeof(inp->name));
	return 0;
}

static int seninf_g_input(struct file *file, void *fh, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int seninf_s_input(struct file *file, void *fh, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static const struct v4l2_ioctl_ops seninf_ioctl_ops = {
	.vidioc_querycap = seninf_querycap,
	.vidioc_enum_fmt_vid_cap = seninf_enum_fmt,
	.vidioc_enum_framesizes = seninf_enum_framesizes,
	.vidioc_g_fmt_vid_cap = seninf_g_fmt,
	.vidioc_try_fmt_vid_cap = seninf_try_fmt,
	.vidioc_s_fmt_vid_cap = seninf_s_fmt,
	.vidioc_enum_input = seninf_enum_input,
	.vidioc_g_input = seninf_g_input,
	.vidioc_s_input = seninf_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations seninf_fops = {
	.owner = THIS_MODULE,
	.open = v4l2_fh_open,
	.release = vb2_fop_release,
	.unlocked_ioctl = video_ioctl2,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int seninf_link_validate(struct media_link *link)
{
	return 0;	/* the format is checked against the sensor at stream on */
}

static const struct media_entity_operations seninf_entity_ops = {
	.link_validate = seninf_link_validate,
};

static int seninf_register_video(struct mt6589_seninf *s)
{
	struct video_device *vdev = &s->vdev;
	struct vb2_queue *q = &s->queue;
	int ret;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = s;
	q->buf_struct_size = sizeof(struct seninf_buffer);
	q->ops = &seninf_vb2_ops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->min_queued_buffers = SENINF_MIN_BUFFERS;
	q->lock = &s->lock;
	q->dev = s->dev;
	ret = vb2_queue_init(q);
	if (ret)
		return ret;

	s->pad.flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	vdev->entity.ops = &seninf_entity_ops;
	ret = media_entity_pads_init(&vdev->entity, 1, &s->pad);
	if (ret)
		return ret;

	strscpy(vdev->name, "mt6589-seninf", sizeof(vdev->name));
	vdev->v4l2_dev = &s->v4l2_dev;
	vdev->fops = &seninf_fops;
	vdev->ioctl_ops = &seninf_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &s->lock;
	vdev->queue = q;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_IO_MC;
	vdev->vfl_dir = VFL_DIR_RX;
	video_set_drvdata(vdev, s);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		media_entity_cleanup(&vdev->entity);
	return ret;
}

/* --- The sensor ---------------------------------------------------------- */

static int seninf_notify_bound(struct v4l2_async_notifier *notifier,
			       struct v4l2_subdev *sd,
			       struct v4l2_async_connection *asc)
{
	struct mt6589_seninf *s = container_of(notifier, struct mt6589_seninf,
					       notifier);
	int pad;

	pad = media_entity_get_fwnode_pad(&sd->entity, asc->match.fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		return pad;
	s->sensor = sd;
	s->sensor_pad = pad;
	return media_create_pad_link(&sd->entity, pad, &s->vdev.entity, 0,
				     MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
}

static void seninf_notify_unbind(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *sd,
				 struct v4l2_async_connection *asc)
{
	struct mt6589_seninf *s = container_of(notifier, struct mt6589_seninf,
					       notifier);

	s->sensor = NULL;
}

static int seninf_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mt6589_seninf *s = container_of(notifier, struct mt6589_seninf,
					       notifier);

	return v4l2_device_register_subdev_nodes(&s->v4l2_dev);
}

static const struct v4l2_async_notifier_operations seninf_notify_ops = {
	.bound = seninf_notify_bound,
	.unbind = seninf_notify_unbind,
	.complete = seninf_notify_complete,
};

static int seninf_register_media(struct mt6589_seninf *s)
{
	struct v4l2_async_connection *asc;
	struct fwnode_handle *ep;
	int ret;

	s->mdev.dev = s->dev;
	strscpy(s->mdev.model, "MT6589 SENINF", sizeof(s->mdev.model));
	media_device_init(&s->mdev);
	s->v4l2_dev.mdev = &s->mdev;
	ret = v4l2_device_register(s->dev, &s->v4l2_dev);
	if (ret)
		goto err_media;

	ret = seninf_register_video(s);
	if (ret)
		goto err_v4l2;

	v4l2_async_nf_init(&s->notifier, &s->v4l2_dev);
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(s->dev), 0, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		ret = dev_err_probe(s->dev, -ENODEV, "no sensor endpoint\n");
		goto err_video;
	}
	asc = v4l2_async_nf_add_fwnode_remote(&s->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		ret = PTR_ERR(asc);
		goto err_nf;
	}
	s->notifier.ops = &seninf_notify_ops;
	ret = v4l2_async_nf_register(&s->notifier);
	if (ret)
		goto err_nf;

	ret = media_device_register(&s->mdev);
	if (ret)
		goto err_nf_unreg;
	return 0;

err_nf_unreg:
	v4l2_async_nf_unregister(&s->notifier);
err_nf:
	v4l2_async_nf_cleanup(&s->notifier);
err_video:
	vb2_video_unregister_device(&s->vdev);
err_v4l2:
	v4l2_device_unregister(&s->v4l2_dev);
err_media:
	media_device_cleanup(&s->mdev);
	return ret;
}

static int mt6589_seninf_probe(struct platform_device *pdev)
{
	struct mt6589_seninf *s;
	int i, ret;

	s = devm_kzalloc(&pdev->dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = &pdev->dev;
	platform_set_drvdata(pdev, s);
	mutex_init(&s->lock);
	spin_lock_init(&s->irqlock);
	INIT_LIST_HEAD(&s->queued);
	s->fmt.pixelformat = V4L2_PIX_FMT_SBGGR8;
	seninf_fill_fmt(&s->fmt, 1280, 720);

	s->base = devm_platform_ioremap_resource_byname(pdev, "seninf");
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);
	s->isp = devm_platform_ioremap_resource_byname(pdev, "isp");
	if (IS_ERR(s->isp))
		return PTR_ERR(s->isp);
	s->rx = devm_platform_ioremap_resource_byname(pdev, "mipi-rx");
	if (IS_ERR(s->rx))
		return PTR_ERR(s->rx);
	s->rxcal = devm_platform_ioremap_resource_byname(pdev, "mipi-rx-cal");
	if (IS_ERR(s->rxcal))
		return PTR_ERR(s->rxcal);

	for (i = 0; i < ARRAY_SIZE(s->clks); i++)
		s->clks[i].id = seninf_clk_names[i];
	ret = devm_clk_bulk_get(s->dev, ARRAY_SIZE(s->clks), s->clks);
	if (ret)
		return dev_err_probe(s->dev, ret, "clocks\n");

	s->irq = platform_get_irq(pdev, 0);
	if (s->irq < 0)
		return s->irq;
	/* Enabled only while streaming, when the camera block is clocked. */
	ret = devm_request_irq(s->dev, s->irq, seninf_irq, IRQF_NO_AUTOEN,
			       dev_name(s->dev), s);
	if (ret)
		return dev_err_probe(s->dev, ret, "IRQ\n");

	ret = dma_set_mask_and_coherent(s->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(s->dev);
	if (ret)
		return ret;

	ret = seninf_register_mclk(s);
	if (ret)
		return dev_err_probe(s->dev, ret, "MCLK\n");

	return seninf_register_media(s);
}

static void mt6589_seninf_remove(struct platform_device *pdev)
{
	struct mt6589_seninf *s = platform_get_drvdata(pdev);

	/* First: stops a running stream and waits for its users. */
	vb2_video_unregister_device(&s->vdev);
	media_device_unregister(&s->mdev);
	v4l2_async_nf_unregister(&s->notifier);
	v4l2_async_nf_cleanup(&s->notifier);
	v4l2_device_unregister(&s->v4l2_dev);
	media_device_cleanup(&s->mdev);
	mutex_destroy(&s->lock);
}

/*
 * System sleep (s2idle) with a stream running: the sensor loses its
 * registers and the ISP its setup, so the stream is stopped before and
 * started again after, from the buffer that was being filled. The queue and
 * its buffers stay; the program only sees a gap in the frames. This runs in
 * prepare and complete, while every device is still awake: in suspend and
 * resume the sensor's own system sleep callbacks may already have powered
 * it down (its I2C writes failed with -110 there).
 */
static int seninf_prepare(struct device *dev)
{
	struct mt6589_seninf *s = dev_get_drvdata(dev);

	mutex_lock(&s->lock);
	if (vb2_is_streaming(&s->queue) && s->streaming) {
		seninf_hw_stop(s);
		s->resume_streaming = true;
	}
	mutex_unlock(&s->lock);
	return 0;
}

static void seninf_complete(struct device *dev)
{
	struct mt6589_seninf *s = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&s->lock);
	if (s->resume_streaming) {
		unsigned long flags;

		spin_lock_irqsave(&s->irqlock, flags);
		s->errors = 0;
		s->irq_status = s->dma_status = 0;
		spin_unlock_irqrestore(&s->irqlock, flags);
		ret = seninf_hw_start(s);
		if (ret) {
			/* The program gets an error, not a queue that never fills. */
			dev_err(dev, "stream not restarted after sleep: %d\n", ret);
			vb2_queue_error(&s->queue);
		} else {
			s->resume_streaming = false;
		}
	}
	mutex_unlock(&s->lock);
}

static const struct dev_pm_ops seninf_pm_ops = {
	.prepare = seninf_prepare,
	.complete = seninf_complete,
	SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	RUNTIME_PM_OPS(seninf_runtime_suspend, seninf_runtime_resume, NULL)
};

static const struct of_device_id mt6589_seninf_of_match[] = {
	{ .compatible = "mediatek,mt6589-seninf" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6589_seninf_of_match);

static struct platform_driver mt6589_seninf_driver = {
	.probe = mt6589_seninf_probe,
	.remove = mt6589_seninf_remove,
	.driver = {
		.name = "mt6589-seninf",
		.of_match_table = mt6589_seninf_of_match,
		.pm = pm_ptr(&seninf_pm_ops),
	},
};
module_platform_driver(mt6589_seninf_driver);

MODULE_DESCRIPTION("MediaTek MT6589 camera receive path (SENINF, MIPI-RX, raw IMGO)");
MODULE_LICENSE("GPL");
