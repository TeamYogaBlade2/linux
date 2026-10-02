// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 Display BLS PWM Driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6589 BLS (Backlight Scaler) module provides a PWM generator
 * for LCD backlight control, integrated with gamma correction LUTs,
 * PWM output LUTs, dithering, and histogram-based auto-brightness.
 * This driver exposes only the PWM functionality; the other features
 * are set up once during probe and then left in their default states.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mt6589-bls-ddp.h"

/* Register offsets (relative to BLS base address) */
#define BLS_EN				0x0000
#define BLS_RST				0x0004
#define BLS_BLS_SETTING			0x0008
#define BLS_HIS_SETTING			0x000C
#define BLS_INTEN			0x0010
#define BLS_INTSTA			0x0014
#define BLS_SRC_SIZE			0x0018
#define BLS_HIS_PROT			0x001C
#define BLS_GAIN			0x0020
#define BLS_DISTORT_POINT		0x0024
#define BLS_GAIN_SETTING		0x0028
#define BLS_GAMMA_SETTING		0x0030
#define BLS_GAMMA_BOUNDARY		0x0034
#define BLS_LUT_UPDATE			0x0038
#define BLS_PWM_LUT_SEL			0x003C
#define BLS_MAXCLR_LIMIT		0x0040
#define BLS_PRE_DIST_THD		0x0044
#define BLS_DIST_THD_NORM		0x0048
#define BLS_DIST_THD_DARK		0x004C
#define BLS_DIST_THD_BRIGHT		0x0050
#define BLS_DIST_THD_TEXT		0x0054
#define BLS_DS_SETTING			0x0058
#define BLS_BS_SETTING			0x005C
#define BLS_TS_SETTING0			0x0060
#define BLS_TS_SETTING1			0x0064
#define BLS_SC_DIFF_THD			0x0068
#define BLS_SC_BIN_THD			0x006C
#define BLS_FAST_IIR_XCOEFF		0x0070
#define BLS_FAST_IIR_YCOEFF		0x0074
#define BLS_SLOW_IIR_XCOEFF		0x0078
#define BLS_SLOW_IIR_YCOEFF		0x007C
#define BLS_PWM_DUTY			0x0080
#define BLS_PWM_DUTY_GAIN		0x0084
#define BLS_PATTERN			0x0088
#define BLS_PWM_CON			0x0090
/*
 * PWM period is determined by PWM_H_DURATION, PWM_L_DURATION,
 * and PWM_G_DURATION registers. These are left at their reset values
 * for now, as downstream does not modify them. If the backlight
 * frequency needs to be adjusted, set these registers accordingly.
 */
#define PWM_H_DURATION			0x0094
#define PWM_L_DURATION			0x0098
#define PWM_G_DURATION			0x009C
#define PWM_SEND_DATA0			0x00A0
#define PWM_SEND_DATA1			0x00A4
#define PWM_WAVE_NUM			0x00A8
#define PWM_DATA_WIDTH			0x00AC
#define PWM_THRESH			0x00B0
#define PWM_SEND_WAVENUM		0x00B4

/* Gamma LUT / IGAMMA LUT / PWM LUT base offsets */
#define BLS_GAMMA_LUT(n)		(0x0400 + 4 * (n))	/* n=0..255 */
#define BLS_IGAMMA_LUT(n)		(0x0800 + 4 * (n))	/* n=0..255 */
#define BLS_PWM_LUT(n)			(0x0C00 + 4 * (n))	/* n=0..32  */

/* Dither registers */
#define BLS_DITHER(n)			(0x0E00 + 4 * (n))	/* n=0..17 */

/* BLS_EN bit definitions */
#define BLS_EN_PWM_ONLY			BIT(31)			/* Enable PWM, others off */

/*
 * BLS running as part of the display pipeline.
 *
 * This driver originally only ever wrote BLS_EN_PWM_ONLY, or zero, which
 * leaves the block as a pure backlight PWM generator: the BLS data path
 * stays disabled, so on the OVL -> COLOR -> BLS -> RDMA0 -> DSI0 path the
 * block RDMA0 fetches from is not running and no frame completes.
 *
 * The value is the one the stock driver writes whenever BLS is enabled for
 * display (ddp_bls.c disp_bls_set_aal(), and the resume path in ddp_drv.c).
 * Bit 0 enables the block; the high bits are latches the hardware sets once
 * it has caught up, so they are part of the target value rather than
 * separate writes.
 */
#define BLS_EN_DISPLAY			0x80010001

/* BLS_PWM_DUTY format */
#define PWM_DUTY_MIN_LEVEL		BIT(19)			/* Lower bound (fixed to 1) */
#define PWM_MAX_LEVEL			255			/* Maximum duty value */

/* Default PWM divider value (from downstream) */
#define PWM_DEFAULT_DIV			0x24

struct mt6589_bls_pwm {
	void __iomem *base;
	struct clk *clk_main;
	unsigned int max_level;
	struct reset_control *rstc;
	bool clk_enabled;
	bool bls_enabled;
};

static inline struct mt6589_bls_pwm *to_mt6589_bls_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

/*
 * Convert PWM duty cycle (in ns) to a brightness level (0..max_level).
 * The period is expected to be constant and provided by the consumer
 * (e.g., pwm-backlight).  We simply scale duty_cycle to the [0, max_level]
 * range.
 */
static unsigned int duty_to_level(const struct pwm_state *state,
				  unsigned int max_level)
{
	u64 level;

	if (!state->enabled || state->duty_cycle == 0)
		return 0;

	level = mul_u64_u64_div_u64(state->duty_cycle, max_level, state->period);
	if (level > max_level)
		level = max_level;

	return (unsigned int)level;
}

/* Initialize Gamma LUT with identity (1:1) mapping */
static void mt6589_bls_gamma_init(struct mt6589_bls_pwm *bls)
{
	unsigned int i;
	u32 val;

	/* Gamma table: 256 entries, 10-bit per component, packed as
	 * {R[9:0], G[9:0], B[9:0]} => bits 29..20, 19..10, 9..0.
	 * Identity mapping: entry[i] = (i << 2) so that 255 -> 1020.
	 */
	for (i = 0; i < 256; i++) {
		val = ((i << 2) & 0x3FF) << 20 |	/* Red */
		      ((i << 2) & 0x3FF) << 10 |	/* Green */
		      ((i << 2) & 0x3FF);		/* Blue */
		writel(val, bls->base + BLS_GAMMA_LUT(i));
	}

	/* Last point (index 256) boundary register */
	val = (0x3FF << 20) |
	      (0x3FF << 10) |
	      0x3FF;
	writel(val, bls->base + BLS_GAMMA_BOUNDARY);

	/* Enable gamma table (bit 0 of BLS_GAMMA_SETTING) */
	writel(0x00000001, bls->base + BLS_GAMMA_SETTING);
}

/* Configure dithering registers with downstream magic values */
static void mt6589_bls_dither_init(struct mt6589_bls_pwm *bls)
{
	/* Values taken from MT6589 downstream ddp_bls.c */
	writel(0x00000001, bls->base + BLS_DITHER(0));
	writel(0x00000000, bls->base + BLS_DITHER(6));
	writel(0x00000222, bls->base + BLS_DITHER(13));
	writel(0x00000000, bls->base + BLS_DITHER(14));
	writel(0x22220001, bls->base + BLS_DITHER(15));
	writel(0x22222222, bls->base + BLS_DITHER(16));
	writel(0x00000000, bls->base + BLS_DITHER(17));
}

static int mt6589_bls_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
				const struct pwm_state *state)
{
	struct mt6589_bls_pwm *bls = to_mt6589_bls_pwm(chip);
	unsigned int level;
	u32 reg;
	int ret;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (state->enabled) {
		if (!bls->clk_enabled) {
			ret = clk_prepare_enable(bls->clk_main);
			if (ret)
				return ret;

			bls->clk_enabled = true;
		}

		level = duty_to_level(state, bls->max_level);

		if (!level)
			reg = 0;
		else
			reg = PWM_DUTY_MIN_LEVEL | (level & 0x3FF);

		writel(reg, bls->base + BLS_PWM_DUTY);

		/*
		 * Run the block as a whole.  The backlight PWM and the BLS
		 * data path share BLS_EN, so selecting PWM-only here would
		 * disable the scaling stage the display pipeline needs.
		 */
		writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
		bls->bls_enabled = true;
	} else {
		/*
		 * Brightness zero: stop driving the backlight PWM duty, but
		 * leave the BLS data path alone.  Writing 0 here would take
		 * the block out of the display pipeline entirely, so a panel
		 * that was merely dark would also stop passing frames.
		 */
		writel(0x0, bls->base + BLS_PWM_DUTY);

		if (bls->bls_enabled) {
			writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
		} else if (bls->clk_enabled) {
			writel(0x0, bls->base + BLS_EN);
			clk_disable_unprepare(bls->clk_main);
			bls->clk_enabled = false;
		}
	}

	return 0;
}

static const struct pwm_ops mt6589_bls_pwm_ops = {
	.apply = mt6589_bls_pwm_apply,
};

static int mt6589_bls_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct mt6589_bls_pwm *bls;
	int ret;

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*bls));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	bls = to_mt6589_bls_pwm(chip);

	bls->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bls->base))
		return PTR_ERR(bls->base);

	bls->clk_main = devm_clk_get(dev, "main");
	if (IS_ERR(bls->clk_main))
		return dev_err_probe(dev, PTR_ERR(bls->clk_main),
				     "failed to get clock\n");

	bls->max_level = PWM_MAX_LEVEL;			/* hardcoded, matches downstream default */

	/* Enable clocks for initialization */
	ret = clk_prepare_enable(bls->clk_main);
	if (ret)
		return ret;

	/*
	 * Release the block's reset properly.  BLS sits in the middle of the
	 * OVL -> COLOR -> BLS -> RDMA0 -> DSI0 path, so while its reset bit
	 * is asserted the block RDMA0 fetches from is held in reset.  The
	 * DISPSYS reset is active low, so clearing the bit releases it, which
	 * is what writing 0 does here; the reset controller added to the node
	 * performs the same toggle and leaves the block out of reset too.
	 *
	 * Look the reset up by index: the node carries a bare resets property
	 * with no reset-names, and a named lookup fails with -ENOENT before any
	 * reset controller is consulted.
	 */
	bls->rstc = devm_reset_control_get(dev, NULL);
	if (IS_ERR(bls->rstc))
		return dev_err_probe(dev, PTR_ERR(bls->rstc),
				     "failed to get reset control\n");

	ret = reset_control_reset(bls->rstc);
	if (ret)
		return dev_err_probe(dev, ret, "failed to reset BLS\n");

	/* Initialize PWM control: clock divider, idle level high */
	writel(0x00050000 | PWM_DEFAULT_DIV, bls->base + BLS_PWM_CON);

	/* Duty gain = 1.0 (0x100 = 256/256) */
	writel(0x00000100, bls->base + BLS_PWM_DUTY_GAIN);

	/*
	 * Leave the scaling stage configured but not yet started: BLS_SETTING
	 * stays 0 here and the block is enabled below, once the pipeline is
	 * ready for it.  The register is not touched again afterwards.
	 */
	writel(0x0, bls->base + BLS_BLS_SETTING);

	/*
	 * BLS_SRC_SIZE is the width of the picture entering the block.  The
	 * stock driver programs it from the display mode; this driver leaves
	 * it at 0 because it only ever modelled the backlight PWM.  Set it to
	 * 0 here explicitly so the register is not left holding whatever the
	 * bootloader left, and note that the frame size itself still comes
	 * from the DRM path.
	 */
	writel(0x0, bls->base + BLS_SRC_SIZE);

	/* Initialize gamma LUT (identity) */
	mt6589_bls_gamma_init(bls);

	/* Initialize dithering */
	mt6589_bls_dither_init(bls);

	/* Disable interrupts (they are enabled by default?) */
	writel(0x0, bls->base + BLS_INTEN);

	/*
	 * BLS_HIS_SETTING:
	 * bit1: Histogram_Mode = 1 (histogram of input data, w/o inverse gamma)
	 * bit0: Histogram_Auto_Clear = 1 (auto-clear histogram at frame start)
	 * We do not use the histogram engine, but keep downstream default.
	 */
	writel(0x00000003, bls->base + BLS_HIS_SETTING);

	/*
	 * Start the block as part of the display pipeline rather than leaving
	 * it off.  It used to be zeroed here and only ever set to the
	 * PWM-only value, which means the BLS data path never ran at all and
	 * the main LCD path had nothing to fetch from.
	 */
	writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
	bls->bls_enabled = true;

	clk_disable_unprepare(bls->clk_main);

	chip->ops = &mt6589_bls_pwm_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to add PWM chip\n");

	return 0;
}

/*
 * DDP component side.
 *
 * On MT6589 the BLS block sits inside the display data path -
 * OVL -> COLOR -> BLS -> RDMA0 -> DSI0 - and also generates the backlight
 * PWM.  The stock driver enables MT_CG_DISP0_BLS in disp_bls_config() and
 * programs BLS_SRC_SIZE from the mode, so treating the block as PWM only
 * leaves a stage of the pipeline unclocked and unsized, and OVL then reports
 * "RDMA0 didn't complete frame" forever.
 *
 * These are exported so mtk_ddp_comp.c can drive BLS as a real component.
 */
int mt6589_bls_ddp_clk_enable(struct device *dev)
{
	struct mt6589_bls_pwm *bls = dev_get_drvdata(dev);

	if (!bls)
		return -ENODEV;

	return clk_prepare_enable(bls->clk_main);
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_clk_enable);

void mt6589_bls_ddp_clk_disable(struct device *dev)
{
	struct mt6589_bls_pwm *bls = dev_get_drvdata(dev);

	if (!bls || !bls->clk_enabled)
		return;

	clk_disable_unprepare(bls->clk_main);
	bls->clk_enabled = false;
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_clk_disable);

/*
 * BLS_SRC_SIZE carries the size of the picture entering the block.  Without
 * it the block has no idea how wide the frame is, which is what the stock
 * driver programs from the mode (ddp_bls.c: (srcHeight << 16) | srcWidth).
 */
void mt6589_bls_ddp_config(struct device *dev, unsigned int w,
			   unsigned int h, unsigned int vrefresh,
			   unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mt6589_bls_pwm *bls = dev_get_drvdata(dev);

	if (!bls || !w || !h)
		return;

	writel((h << 16) | w, bls->base + BLS_SRC_SIZE);
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_config);

/*
 * Run the block as part of the display pipeline rather than leaving it as a
 * backlight generator.  0x80010001 is the value the stock driver waits for
 * when enabling BLS for display (ddp_drv.c, ddp_bls.c); 0x80000000, which
 * this driver used to write, is "only enable PWM" per its own comment in
 * ddp_bls.c.
 */
void mt6589_bls_ddp_start(struct device *dev)
{
	struct mt6589_bls_pwm *bls = dev_get_drvdata(dev);

	if (!bls)
		return;

	writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
	bls->bls_enabled = true;
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_start);

void mt6589_bls_ddp_stop(struct device *dev)
{
	struct mt6589_bls_pwm *bls = dev_get_drvdata(dev);

	if (!bls)
		return;

	writel(0x0, bls->base + BLS_EN);
	bls->bls_enabled = false;
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_stop);

static const struct of_device_id mt6589_bls_pwm_of_match[] = {
	{ .compatible = "mediatek,mt6589-disp-pwm" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6589_bls_pwm_of_match);

static struct platform_driver mt6589_bls_pwm_driver = {
	.probe = mt6589_bls_pwm_probe,
	.driver = {
		.name = "mt6589-disp-pwm",
		.of_match_table = mt6589_bls_pwm_of_match,
	},
};
module_platform_driver(mt6589_bls_pwm_driver);

MODULE_AUTHOR("Akari Tsuyukusa");
MODULE_DESCRIPTION("MediaTek MT6589 Display BLS PWM Driver");
MODULE_LICENSE("GPL");
