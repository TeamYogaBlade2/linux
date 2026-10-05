// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 Display BLS PWM Driver
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6589 BLS (Backlight Scaler) module provides a PWM generator
 * for LCD backlight control, integrated with gamma correction LUTs,
 * PWM output LUTs, dithering, and histogram-based auto-brightness.
 * This driver exposes only the PWM functionality.  The gamma LUT and
 * the dither matrix are programmed once at probe and never revisited;
 * the BLS scaling, PWM LUT, histogram and auto-brightness features are
 * not driven at all.
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
 * The PWM period comes from PWM_{H,L,G}_DURATION.  They are left at their
 * reset values (1, 1, 0 - datasheet ch. 45.3, p. 1670), which the stock
 * driver also never writes.  Note the hardware's rule \"if duration = N,
 * program N-1\" and \"the duration of PWM must not be 0\": the reset values
 * are already the minimum legal ones, so anything else has to be programmed
 * deliberately.
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
 * 0x80010001 = BLS_En (bit 0) + HIS_En (bit 16) + PWM_En (bit 31) - the
 * datasheet's own three BLS_EN enables (ch. 45.3, p. 1655), none of them a
 * read-back latch.  This is the value the stock driver writes when BLS is on
 * for display, and it is what it polls for when resuming AAL
 * (aquaris-5 .../dispsys/ddp_bls.c:171 and ddp_drv.c:1722).
 *
 * BLS_En matters: writing only PWM_En (0x80000000, what this driver used to
 * do) leaves the scaling stage disabled, and on the OVL -> COLOR -> BLS ->
 * RDMA0 -> DSI0 path the block RDMA0 fetches from is then not running, so no
 * frame completes.
 */
#define BLS_EN_DISPLAY			0x80010001

/* BLS_PWM_DUTY format */
#define PWM_DUTY_MIN_LEVEL		BIT(19)			/* Lower bound = 1 */
#define PWM_MAX_LEVEL			255			/* Maximum duty value */

/* Default PWM divider value (ddp_bls.c:19, PWM_DEFAULT_DIV_VALUE). */
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

/* Scale the duty cycle to [0, max_level] against the consumer's period. */
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

/*
 * Identity gamma: 256 entries of 10-bit {R,G,B} packed as bits 29:20, 19:10
 * and 9:0 (datasheet ch. 45.3, p. 1674), so entry[i] = (i << 2) maps 255 to
 * 1020, plus the 257th entry in BLS_GAMMA_BOUNDARY (p. 1661).
 */
static void mt6589_bls_gamma_init(struct mt6589_bls_pwm *bls)
{
	unsigned int i;
	u32 val;

	for (i = 0; i < 256; i++) {
		val = ((i << 2) & 0x3FF) << 20 |	/* Red */
		      ((i << 2) & 0x3FF) << 10 |	/* Green */
		      ((i << 2) & 0x3FF);		/* Blue */
		writel(val, bls->base + BLS_GAMMA_LUT(i));
	}

	val = (0x3FF << 20) |
	      (0x3FF << 10) |
	      0x3FF;
	writel(val, bls->base + BLS_GAMMA_BOUNDARY);

	writel(0x00000001, bls->base + BLS_GAMMA_SETTING);
}

/* Dither matrix, from ddp_bls.c:357-363 (disp_bls_init). */
static void mt6589_bls_dither_init(struct mt6589_bls_pwm *bls)
{
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

		/* The backlight PWM and the data path share BLS_EN, so
		 * PWM-only here would disable the stage the pipeline needs.
		 */
		writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
		bls->bls_enabled = true;
	} else {
		/*
		 * Brightness zero stops the PWM duty but must leave the data
		 * path alone: writing 0 to BLS_EN would take the block out of
		 * the display pipeline, so a merely dark panel would also stop
		 * passing frames.
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

/*
 * There is exactly one BLS block, and its state hangs off the pwm_chip -
 * never off device drvdata, unlike the OVL/RDMA/COLOR/DSI drivers which call
 * platform_set_drvdata() in their own probe.  dev_get_drvdata() on the DDP
 * component device is therefore always NULL, which is what silently turned
 * every ddp_bls hook into a no-op.  Keep the instance here instead.
 */
static struct mt6589_bls_pwm *bls_ddp;
static struct clk *bls_ddp_clk;

static int mt6589_bls_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct mt6589_bls_pwm *bls;
	int ret;

	/* The backlight node is a child of this one and fails silently if
	 * the chip never appears, so say where this gets to.
	 */
	dev_info(dev, "bls pwm: probe\n");

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*bls));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	bls = to_mt6589_bls_pwm(chip);

	bls->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bls->base))
		return dev_err_probe(dev, PTR_ERR(bls->base),
				     "bls pwm: ioremap failed\n");

	bls->clk_main = devm_clk_get(dev, "main");
	if (IS_ERR(bls->clk_main))
		return dev_err_probe(dev, PTR_ERR(bls->clk_main),
				     "bls pwm: no main clock\n");

	bls->max_level = PWM_MAX_LEVEL;			/* hardcoded, matches downstream default */

	/* Enable clocks for initialization */
	ret = clk_prepare_enable(bls->clk_main);
	if (ret)
		return ret;

	/*
	 * Release the block's reset properly.  BLS sits in the middle of the
	 * OVL -> COLOR -> BLS -> RDMA0 -> DSI0 path, so while its reset bit
	 * is asserted the block RDMA0 fetches from is held in reset.  The
	 * DISPSYS reset is active low (\"0: Reset, 1: Does not reset\",
	 * datasheet ch. 38, p. 1458), so clearing the bit releases it; the
	 * reset controller on the node performs the same assert-then-deassert.
	 *
	 * Look the reset up by index: the node carries a bare resets property
	 * with no reset-names, and a named lookup fails with -ENOENT.
	 */
	bls->rstc = devm_reset_control_get(dev, NULL);
	if (IS_ERR(bls->rstc))
		return dev_err_probe(dev, PTR_ERR(bls->rstc),
				     "failed to get reset control\n");

	ret = reset_control_reset(bls->rstc);
	if (ret)
		return dev_err_probe(dev, ret,
				     "bls pwm: failed to reset BLS\n");

	/* Clock divider and idle level high (ddp_bls.c:348). */
	writel(0x00050000 | PWM_DEFAULT_DIV, bls->base + BLS_PWM_CON);

	/* Duty gain = 1.0 (0x100 = 256/256, datasheet p. 1667). */
	writel(0x00000100, bls->base + BLS_PWM_DUTY_GAIN);

	/*
	 * Leave the scaling stage configured but not started; the register is
	 * not touched again afterwards.
	 */
	writel(0x0, bls->base + BLS_BLS_SETTING);

	/*
	 * BLS_SRC_SIZE is the width of the picture entering the block.  Zeroed
	 * here rather than left holding whatever the bootloader wrote;
	 * mt6589_bls_ddp_config() sets it from the mode.
	 */
	writel(0x0, bls->base + BLS_SRC_SIZE);

	mt6589_bls_gamma_init(bls);

	mt6589_bls_dither_init(bls);

	/*
	 * Disable the four BLS interrupts (frame complete, frame underrun,
	 * scene change, PWM LUT miss, datasheet p. 1658).  The stock driver
	 * writes 0xF here (ddp_bls.c:351); its reset value is 0.
	 */
	writel(0x0, bls->base + BLS_INTEN);

	/*
	 * BLS_HIS_SETTING at +0x0c is deliberately left alone.  It exists and
	 * is documented (datasheet ch. 45.3, p. 1653) but only holds
	 * Histogram_Mode and Histogram_Auto_Clear, and the histogram engine
	 * itself is off because BLS_EN.HIS_En is not part of
	 * BLS_EN_DISPLAY's data path use here.  The stock driver does write
	 * +0x0c, but with the value 0x3 (ddp_bls.c:366) whose \"w/o inverse
	 * gamma\" comment does not match this register's fields.
	 */

	/*
	 * Start the block as part of the display pipeline rather than leaving
	 * it off, or setting only the PWM-only value.
	 */
	writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
	bls->bls_enabled = true;

	clk_disable_unprepare(bls->clk_main);

	chip->ops = &mt6589_bls_pwm_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "bls pwm: failed to add PWM chip\n");

	dev_info(dev, "bls pwm: chip registered\n");

	/* The DDP hooks reach the block through this pointer; see above. */
	bls_ddp = bls;

	/* DDP-side clock handle; the component hooks cannot get one
	 * themselves, since dev_get_drvdata() there is NULL.
	 */
	bls_ddp_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(bls_ddp_clk))
		return dev_err_probe(dev, PTR_ERR(bls_ddp_clk),
				     "failed to get the BLS clock\n");

	return 0;
}

/*
 * DDP component side.
 *
 * On MT6589 the BLS block sits inside the display data path -
 * OVL -> COLOR -> BLS -> RDMA0 -> DSI0 - and also generates the backlight
 * PWM.  The stock driver enables MT_CG_DISP0_BLS in disp_bls_config()
 * (ddp_bls.c:393) and programs BLS_SRC_SIZE from the mode
 * (ddp_bls.c:346), so treating the block as PWM only leaves a stage of the
 * pipeline unsized.
 *
 * These are exported so mtk_ddp_comp.c can drive BLS as a real component.
 */
int mt6589_bls_ddp_clk_enable(struct device *dev)
{
	/* Taken once at probe from the node the generic mtk_ddp_clk_enable()
	 * uses: resolving it again per enable/disable would take a reference
	 * each time, and the disable side has no way to put it.
	 */
	if (!bls_ddp_clk)
		return -ENODEV;

	return clk_prepare_enable(bls_ddp_clk);
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_clk_enable);

void mt6589_bls_ddp_clk_disable(struct device *dev)
{
	if (bls_ddp_clk)
		clk_disable_unprepare(bls_ddp_clk);
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_clk_disable);

/* (srcHeight << 16) | srcWidth, exactly as ddp_bls.c:346 does it. */
void mt6589_bls_ddp_config(struct device *dev, unsigned int w,
			   unsigned int h, unsigned int vrefresh,
			   unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mt6589_bls_pwm *bls = bls_ddp;

	if (!bls || !w || !h)
		return;

	writel((h << 16) | w, bls->base + BLS_SRC_SIZE);

	/*
	 * BLS_SETTING stays 0 on this path, and that is correct: the 0x11d00
	 * bit mask is written by disp_onConfig_bls() (ddp_bls.c:120), which is
	 * only reached from the AAL colour-enhancement path (ddp_aal.c:132),
	 * not from the display path.  disp_bls_init(), which the main path
	 * calls, leaves it at 0 (ddp_bls.c:350).
	 */
	writel(0, bls->base + BLS_BLS_SETTING);
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_config);

/* See BLS_EN_DISPLAY for why the full enable value is used here. */
void mt6589_bls_ddp_start(struct device *dev)
{
	struct mt6589_bls_pwm *bls = bls_ddp;

	if (!bls)
		return;

	writel(BLS_EN_DISPLAY, bls->base + BLS_EN);
	bls->bls_enabled = true;
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_start);

void mt6589_bls_ddp_stop(struct device *dev)
{
	struct mt6589_bls_pwm *bls = bls_ddp;

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
