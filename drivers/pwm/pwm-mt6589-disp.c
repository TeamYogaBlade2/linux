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
#include <linux/spinlock.h>
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
/*
 * BLS_LUT_UPDATE strobes (datasheet p. 1661).  Bit 0 gates the gamma table,
 * bit 1 the inverse-gamma table and bit 2 the PWM table; each is asserted
 * before its table is written and released afterwards.  A table written
 * without its strobe is not latched by the engine.
 */
#define BLS_LUT_UPDATE_GAMMA		BIT(0)
#define BLS_LUT_UPDATE_PWM		BIT(2)
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
/*
 * MT6589 datasheet, BLS_EN:
 *   bit 31: PWM_En
 *   bit 16: HIS_En
 *   bit  0: BLS_En
 *
 * The normal Lenovo display path uses 0x80000000.  Enabling the PWM
 * must not implicitly enable histogram collection or BLS processing.
 */
#define BLS_EN_PWM			BIT(31)
#define BLS_EN_HISTOGRAM		BIT(16)
#define BLS_EN_BLS			BIT(0)

/* BLS_PWM_DUTY format */
/*
 * PWM_DUTY_LOW_LIMIT is left at 0.  Setting it would clamp the output to
 * LOW_LIMIT*8 duty units, and the bootloader's working configuration leaves it
 * at 0 while running PWM_DUTY_IN = 116.
 */
#define PWM_DUTY_LOW_LIMIT_MASK		GENMASK(23, 19)
#define PWM_MAX_LEVEL			255			/* Maximum duty value */

/*
 * PWM clock divider.  PWM_CLKDIV is bits 6:0 and the PWM period is
 * 256 * (CLKDIV + 1) BLS clocks, so this sets the PWM frequency, not the duty
 * ratio.
 *
 * 0x47 is what the bootloader runs this panel with (BLS_PWM_CON = 0x00050047 on
 * a board whose backlight dims correctly).  The vendor kernel's
 * PWM_DEFAULT_DIV_VALUE is 0x24, which produces the same ratio at roughly twice
 * the frequency - both within any backlight driver's range, so the value should
 * not matter for brightness, but the bootloader's is the one known to work on
 * this hardware and there is no reason to prefer the other.
 */
#define PWM_DEFAULT_DIV			0x47

/*
 * There is exactly one BLS block, and its state hangs off the pwm_chip -
 * never off device drvdata, unlike the OVL/RDMA/COLOR/DSI drivers which call
 * platform_set_drvdata() in their own probe.  dev_get_drvdata() on the DDP
 * component device is therefore always NULL, which is what silently turned
 * every ddp_bls hook into a no-op.
 *
 * bls_ddp_clk is the single clock handle shared by the PWM and the DDP hooks:
 * the BLS gate is one clock, and taking it twice would give two handles over
 * one clk whose prepare_count is shared, so the PWM disabling it would gate
 * the block out from under the display path.
 */
static struct mt6589_bls_pwm *bls_ddp;
static struct clk *bls_ddp_clk;

struct mt6589_bls_pwm {
	void __iomem *base;
	unsigned int max_level;
	/* Last applied state, so .get_state() can report something truthful. */
	struct pwm_state state;
	struct reset_control *rstc;
	spinlock_t en_lock;
	bool clk_enabled;
};

static inline struct mt6589_bls_pwm *to_mt6589_bls_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

/*
 * BLS_EN is shared by the PWM generator and the BLS/histogram engines.
 * Serialize read-modify-write operations so that changing the backlight
 * cannot overwrite bits managed by the display path.
 */
static void mt6589_bls_update_en(struct mt6589_bls_pwm *bls,
				 u32 mask, u32 value)
{
	unsigned long flags;
	u32 en;

	spin_lock_irqsave(&bls->en_lock, flags);
	en = readl(bls->base + BLS_EN);
	en = (en & ~mask) | (value & mask);
	writel(en, bls->base + BLS_EN);
	spin_unlock_irqrestore(&bls->en_lock, flags);
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
 *
 * The LUT writes only take effect when bracketed by the matching update
 * strobe: BLS_LUT_UPDATE bit 0 is "Gamma_Update", asserted before the table
 * is written and released afterwards (datasheet p. 1661; downstream
 * ddp_bls.c:275,292 wraps the same loop this way).  Without it the entries
 * land in a buffer the gamma engine never latches, which is silent because
 * the LUT registers still read back what was written.
 */
static void mt6589_bls_gamma_init(struct mt6589_bls_pwm *bls)
{
	unsigned int i;
	u32 val;

	writel(BLS_LUT_UPDATE_GAMMA, bls->base + BLS_LUT_UPDATE);

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

	writel(0, bls->base + BLS_LUT_UPDATE);

	writel(0x00000001, bls->base + BLS_GAMMA_SETTING);
}

/*
 * The PWM LUT is a 33x33 table: BLS_PWM_LUT_SEL picks one of 33 input rows,
 * and the 33 entries of that row are the output levels for the 33 duty steps.
 * The stock driver fills every row with the same linear ramp, value = n * 4
 * clamped at 255, which makes the table an identity map expressed in the
 * block's own units (disp_bls_update_pwm_lut(), shipped blade10 kernel at
 * c05410f0, reading the table at c09925c0).
 *
 * Like the gamma table this is only latched when bracketed by its update
 * strobe, and here the bit is BIT(2), "PWM_Update".  The vendor asserts it,
 * selects each row, writes the 33 entries and releases it.
 *
 * It is worth being explicit about why this matters for brightness: with the
 * gamma table present but the PWM LUT never written, the duty value in
 * BLS_PWM_DUTY still reads back whatever was written to it, so the control
 * looks like it works, but the engine has no mapping to apply and the panel's
 * average brightness does not follow.  That is exactly the "registers update,
 * backlight does not change" symptom reported on this board.
 */
static void mt6589_bls_pwm_lut_init(struct mt6589_bls_pwm *bls)
{
	unsigned int row, col;
	u32 val;

	writel(BLS_LUT_UPDATE_PWM, bls->base + BLS_LUT_UPDATE);

	for (row = 0; row < 33; row++) {
		writel(row, bls->base + BLS_PWM_LUT_SEL);

		for (col = 0; col < 33; col++) {
			val = min(col * 4U, 255U);
			writel(val, bls->base + BLS_PWM_LUT(col));
		}
	}

	writel(0, bls->base + BLS_LUT_UPDATE);
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

	if (!bls_ddp_clk)
		return -ENODEV;

	if (state->enabled) {
		if (!bls->clk_enabled) {
			ret = clk_prepare_enable(bls_ddp_clk);
			if (ret)
				return ret;

			bls->clk_enabled = true;
		}

		level = duty_to_level(state, bls->max_level);

		if (!level)
			reg = 0;
		else
			reg = level & 0x1FF;	/* PWM_DUTY_IN is bits 8:0 */

		writel(reg, bls->base + BLS_PWM_DUTY);
		mt6589_bls_update_en(bls, BLS_EN_PWM, BLS_EN_PWM);
		bls->state = *state;
	} else {
		/*
		 * Ensure the register is clocked before touching it, even if
		 * the PWM was already reported disabled.  Only PWM_En belongs
		 * to this driver; preserve BLS_En and HIS_En.
		 */
		if (!bls->clk_enabled) {
			ret = clk_prepare_enable(bls_ddp_clk);
			if (ret)
				return ret;

			bls->clk_enabled = true;
		}

		writel(0x0, bls->base + BLS_PWM_DUTY);
		mt6589_bls_update_en(bls, BLS_EN_PWM, 0);

		clk_disable_unprepare(bls_ddp_clk);
		bls->clk_enabled = false;
		bls->state = *state;
	}

	return 0;
}

/*
 * Report the last state applied.  Without this the PWM core cannot answer
 * pwm_get_state_hw() - which is what /sys/kernel/debug/pwm prints - and the
 * readout fails with -EOPNOTSUPP, leaving no way to see what the block was
 * actually programmed with.
 *
 * The state is cached rather than read back because the duty register holds
 * PWM_DUTY_IN (a 9-bit fraction of a period the block derives from
 * BLS_PWM_CON) and the divider lives elsewhere; reconstructing a period and
 * duty cycle from those would report a number this driver never accepted.
 */
static int mt6589_bls_pwm_get_state(struct pwm_chip *chip,
				    struct pwm_device *pwm,
				    struct pwm_state *state)
{
	struct mt6589_bls_pwm *bls = to_mt6589_bls_pwm(chip);

	*state = bls->state;

	return 0;
}

static const struct pwm_ops mt6589_bls_pwm_ops = {
	.apply = mt6589_bls_pwm_apply,
	.get_state = mt6589_bls_pwm_get_state,
};

static int mt6589_bls_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct mt6589_bls_pwm *bls;
	int ret;


	dev_info(dev, "bls pwm: probe\n");

	chip = devm_pwmchip_alloc(dev, 1, sizeof(*bls));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	bls = to_mt6589_bls_pwm(chip);
	spin_lock_init(&bls->en_lock);

	bls->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(bls->base))
		return dev_err_probe(dev, PTR_ERR(bls->base),
				     "bls pwm: ioremap failed\n");

	bls->max_level = PWM_MAX_LEVEL;			/* hardcoded, matches downstream default */

	/*
	 * One clock handle for both roles.  The BLS gate is a single clock:
	 * taking it twice - once here as "main" and once for the DDP hooks -
	 * gives two handles over one clk whose prepare_count is shared, so the
	 * PWM's disable would gate the block out from under the display path.
	 * The DDP-side helpers below use the same handle.
	 */
	bls_ddp_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(bls_ddp_clk))
		return dev_err_probe(dev, PTR_ERR(bls_ddp_clk),
				     "failed to get the BLS clock\n");

	/* Enable the clock for initialization */
	ret = clk_prepare_enable(bls_ddp_clk);
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

	mt6589_bls_pwm_lut_init(bls);

	mt6589_bls_dither_init(bls);

	/*
	 * Disable the four BLS interrupts (frame complete, frame underrun,
	 * scene change, PWM LUT miss, datasheet p. 1658).  The stock driver
	 * writes 0xF here (ddp_bls.c:351); its reset value is 0.
	 */
	writel(0x0, bls->base + BLS_INTEN);

	/*
	 * Match the Lenovo BSP's histogram register initialization even though
	 * the histogram engine remains disabled by PWM-only BLS_EN.
	 */
	writel(0x00000003, bls->base + BLS_HIS_SETTING);

	/* Match disp_bls_init(): enable PWM without BLS/HIS processing. */
	writel(BLS_EN_PWM, bls->base + BLS_EN);

	clk_disable_unprepare(bls_ddp_clk);

	chip->ops = &mt6589_bls_pwm_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "bls pwm: failed to add PWM chip\n");

	dev_info(dev, "bls pwm: chip registered\n");

	/* The DDP hooks reach the block through this pointer; see above. */
	bls_ddp = bls;

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

/*
 * The downstream MT6589 main display path writes BLS_EN = 0x80000000
 * in both disp_bls_init() and disp_bls_config(): PWM is enabled while
 * BLS processing and histogram collection remain disabled.
 *
 * BLS is still part of the routed hardware path. Do not infer that the
 * crossbar connection is disabled merely because BLS_EN_BLS is clear.
 */
void mt6589_bls_ddp_start(struct device *dev)
{
	struct mt6589_bls_pwm *bls = bls_ddp;

	if (!bls)
		return;

	/*
	 * Enable the PWM output only: 0x80000000, PWM_En with BLS_En clear.
	 *
	 * BLS_EN bit 31 is PWM_En ("Enables PWM waveform") and bit 0 is
	 * BLS_En ("Enables BLS"), per the data sheet field table.  BLS_En
	 * turns on the BLS *scaling engine*, and with it enabled the duty
	 * register stops behaving as a plain brightness control: writing
	 * PWM_DUTY_IN is accepted (the register reads back the new value) but
	 * the panel's average brightness no longer follows it, because the
	 * scaling engine is driving the output instead of the raw duty cycle.
	 *
	 * This is not a theory: the bootloader, whose display and backlight
	 * both work on this board, leaves BLS_EN at exactly 0x80000000
	 * (barebox register dump of 0x14008000).  The vendor kernel's
	 * aal_debug_flag == 1 branch writes the same value, described as
	 * "only enable PWM" (ddp_bls.c:173).
	 *
	 * An earlier revision of this function set BLS_En too, on the theory
	 * that BLS is a stage of the pixel path (OVL -> COLOR -> BLS ->
	 * RDMA0) and so had to be enabled for pixels to flow.  It does not:
	 * the display came up when the D-PHY timing was fixed, with BLS_En
	 * clear throughout, and setting it only broke backlight control.
	 */
	mt6589_bls_update_en(bls,
			     BLS_EN_PWM | BLS_EN_BLS | BLS_EN_HISTOGRAM,
			     BLS_EN_PWM);
	dev_info(dev,
		 "MT6589 BLS start: en=%#x src_size=%#x setting=%#x\n",
		 readl(bls->base + BLS_EN), readl(bls->base + BLS_SRC_SIZE),
		 readl(bls->base + BLS_BLS_SETTING));
}
EXPORT_SYMBOL_GPL(mt6589_bls_ddp_start);

void mt6589_bls_ddp_stop(struct device *dev)
{
	struct mt6589_bls_pwm *bls = bls_ddp;

	if (!bls)
		return;

	/* The DDP path does not own the backlight PWM enable bit. */
	mt6589_bls_update_en(bls, BLS_EN_BLS | BLS_EN_HISTOGRAM, 0);
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
