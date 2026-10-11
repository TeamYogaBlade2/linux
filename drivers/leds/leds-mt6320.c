// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LED driver for the MediaTek MT6320 PMIC
 *
 * Drives the three constant-current LED (ISINK / "NLED") sinks of the MT6320,
 * plus the dedicated keypad LED.  They are wired to LEDs on the board, not to
 * SoC GPIOs, so drivers/leds/leds-gpio.c cannot do this: each sink needs its
 * own current step, its own PWM-mode selection and a shared boost clock, all
 * inside the PMIC.
 *
 * Register map and bit positions come from the downstream MT6589 sources (all
 * paths relative to aquaris-5/mediatek/platform/mt6589/kernel/):
 *   - core/include/mach/upmu_hw.h: ISINKS_CON0..6, KPLED_CON0, TOP_CKPDN and
 *     the matching PMIC_ISINK / PMIC_ISINKS / PMIC_KPLED mask-and-shift pairs
 *   - drivers/power/upmu_common.c: the upmu_set_isinks_chN_en/mode/step(),
 *     upmu_set_isink_dimN_duty/fsel(), upmu_set_rg_bst_drv_1m_ck_pdn() and
 *     upmu_set_kpled_en/dim_duty() helpers
 *   - drivers/leds/leds.c: the sequences in mt_brightness_set_pmic() this
 *     driver reproduces (steady state, blink table, keypad LED)
 *
 * The addresses all already exist in the mainline header
 * (include/linux/mfd/mt6320/registers.h) and were cross-checked against the
 * vendor header.  Datasheet cross-check is possible for the addresses only,
 * not for the bits: the MT6320 is a companion PMIC and its register manual is
 * not part of the MT6589 SoC datasheet (grep -c ISINK on the extracted
 * MT6589 datasheet text returns 0).  Every bit position below therefore comes
 * from the vendor header, not from a datasheet.
 *
 * This deliberately does not extend drivers/leds/rgb/leds-mt6370-rgb.c or
 * drivers/leds/leds-mt6323.c: the MT6370 driver drives an 8-bit-wide RGB block
 * at 0x182-0x194, the MT6320 sinks a 16-bit block at 0x056a-0x0580 where
 * three channels share one enable register.  No address overlaps and the
 * register widths differ, so neither driver can be pointed at this part.
 *
 * Copyright (C) 2026 Lenovo
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

#include <linux/mfd/mt6320/registers.h>
#include <linux/mfd/mt6397/core.h>

/* Channel indices match the DT "reg" property and the ISINKS_CONx registers. */
#define MT6320_MAX_LEDS		3

/*
 * TOP_CKPDN bit 7, RG_BST_DRV_1M_CK_PDN, is the 1 MHz boost-converter clock
 * that feeds the current sinks.  All three sinks share it, so it is reference
 * counted by mt6320_leds::bst_users rather than toggled per LED: gating it
 * while another sink is lit would tear down the boost rail under that LED.
 * The downstream driver never gates it at all - across leds.c the un-gate
 * calls are live (298, 315, 331, 546, 589, 632, 674) while every gate-back
 * call is commented out (556, 598, 641, 686) - so the rail is always running
 * there.  Counting the users is the mainline equivalent: the rail is parked as
 * soon as the last sink goes dark instead of being left on forever.
 *
 * upmu_hw.h:702  PMIC_RG_BST_DRV_1M_CK_PDN_MASK 0x1
 * upmu_hw.h:703  PMIC_RG_BST_DRV_1M_CK_PDN_SHIFT 7
 *
 * This bit is programmed in TOP_CKPDN itself, not through a companion shadow:
 * see mt6320_led_bst_clk() for why.
 */
#define MT6320_BST_DRV_1M_CK_PDN	BIT(7)

/*
 * ISINKS_CON0/1/2 hold one dimming counter per channel.
 * Duty is bits [12:8], fsel (the period divisor) is bits [4:0].
 *
 * upmu_hw.h:3632-3647
 *   PMIC_ISINK_DIM{0,1,2}_DUTY_MASK 0x1F / _SHIFT 8
 *   PMIC_ISINK_DIM{0,1,2}_FSEL_MASK 0x1F / _SHIFT 0
 */
#define MT6320_ISINK_DIM_DUTY_MASK	GENMASK(12, 8)
#define MT6320_ISINK_DIM_FSEL_MASK	GENMASK(4, 0)

/*
 * ISINKS_CON3 gates the three sinks.  One bit each: CH0 bit 8, CH1 bit 9,
 * CH2 bit 10.
 *
 * upmu_hw.h:3650-3655
 *   PMIC_ISINKS_CH0_EN_MASK 0x1 / _SHIFT 8
 *   PMIC_ISINKS_CH1_EN_MASK 0x1 / _SHIFT 9
 *   PMIC_ISINKS_CH2_EN_MASK 0x1 / _SHIFT 10
 */
#define MT6320_ISINK_CH_EN(i)		BIT(8 + (i))

/*
 * ISINKS_CON4/5/6 hold per-channel mode and current step.
 *
 * The mode field selects the dimming source.  The downstream driver always
 * writes PMIC_PWM_0 (leds_sw.h:22, enum { PMIC_PWM_0 = 0, ... }).  Because
 * this driver programs the counter itself, PMIC_PWM_0 is the only mode value
 * it needs; the other two encodings are left alone.
 *
 *   upmu_hw.h:3670-3687
 *     PMIC_ISINKS_CH{0,1,2}_MODE_MASK 0x3 / _SHIFT 8
 *     PMIC_ISINKS_CH{0,1,2}_STEP_MASK 0x7 / _SHIFT 12
 *
 * Step is the current code: 0 selects the smallest step, and the downstream
 * driver comments the extremes as 4 mA at code 0 (leds.c:533) up to 16 mA at
 * code 3 (leds.c:620).
 */
#define MT6320_ISINK_CH_MODE_MASK	GENMASK(9, 8)
#define MT6320_ISINK_CH_MODE_PWM0	0
#define MT6320_ISINK_CH_STEP_MASK	GENMASK(14, 12)

/*
 * KPLED_CON0: dedicated keypad backlight sink, independent of the three
 * ISINK channels.  Enable is bit 0, dimming duty is bits [12:8].
 *
 * upmu_hw.h:3612-3617
 *   PMIC_KPLED_DIM_DUTY_MASK 0x1F / _SHIFT 8
 *   PMIC_KPLED_EN_MASK 0x1 / _SHIFT 0
 */
#define MT6320_KPLED_EN			BIT(0)
#define MT6320_KPLED_DIM_DUTY_MASK	GENMASK(12, 8)

/*
 * Dimensioning limits and the vendor steady-state programming.
 *
 * Brightness model: the LED class brightness IS the current step.  brightness
 * N maps onto current step N-1, following drivers/leds/leds-mt6323.c
 * (ISINK_CH_STEP(brightness - 1), leds-mt6323.c:169).  brightness 0 switches
 * the sink off; 1..max_brightness select step 0..max_brightness-1.
 *
 * The step field is three bits wide, so codes 0..7 are representable and the
 * largest useful max_brightness is 8.  The default below exposes codes 0..3,
 * which covers what the BSP programs: it drives a fixed current per channel
 * rather than scaling it, picking step 0 for ch0 (leds.c:533, commented
 * 4 mA) and step 3 for ch1 and ch2 (leds.c:577, :620).
 *
 * Note the BSP's own comments disagree about what step 3 means: it is
 * labelled 16 mA at leds.c:620 for ch2 but 4 mA at leds.c:577 for ch1, and
 * leds.c:661 calls ch1 step 3 "4mA" as well.  The absolute current scale is
 * therefore NOT established, so only the step *codes* and the direction
 * brightness -> step come from here; the latter is this driver's own
 * convention, following leds-mt6323.c.
 *
 * Steady state: a solid LED is duty 15 with fsel 11 and the clock select
 * cleared, i.e. an always-on dimming counter rather than a PWM ratio.  These
 * are the BSP's own numbers for every ISINK channel it drives solid, each
 * commented "6320 0.25KHz":
 *
 *	leds.c:535-536  ch0  upmu_set_isink_dim0_duty(15), _fsel(11)
 *	leds.c:579-580  ch1  upmu_set_isink_dim1_duty(15), _fsel(11)
 *	leds.c:622-623  ch2  upmu_set_isink_dim2_duty(15), _fsel(11)
 *	leds.c:657-658  ch0  duty 1, fsel 1  (the slow, dim ISINK01 variant)
 *
 * fsel is NOT a brightness knob: it is the dimming-counter period divisor, and
 * the duty/fsel pair is the on-fraction of that period.  The LED level is
 * carried by the current step, so the steady-state duty/fsel below are a fixed
 * property of "on" rather than something scaled with brightness.  The steady
 * path in the BSP never touches breathN_trf_sel either; this driver parks it
 * at 0 anyway, as that is the only value the BSP ever programs there (every
 * other trf_sel write in leds.c is the un-GPIEd 0x04 at :231, inside the
 * disabled led_breath_pmic()), which keeps the field deterministic across a
 * blink -> steady transition.
 */
#define MT6320_MAX_BRIGHTNESS		4	/* step codes 0..3, as the BSP */
#define MT6320_MAX_STEP			7	/* step field is 3 bits wide */
#define MT6320_MAX_DUTY			31	/* duty field is 5 bits wide */

/* Vendor steady state, leds.c:535-536 and siblings - see the comment above. */
#define MT6320_STEADY_FSEL		11
#define MT6320_STEADY_DUTY		15
#define MT6320_STEADY_TRF_SEL		0

/*
 * Blink period selection.  The period is chosen by a clock-select nibble in
 * ISINKS_CON8/9/10 (bits [15:12], "trf_sel") plus an fsel correction
 * divider, NOT by fsel alone.  See mt6320_led_blink_timings() for the tables,
 * which are taken verbatim from the BSP.
 *
 * upmu_hw.h:3696-3709  PMIC_ISINKS_BREATH{0,1,2}_TRF_SEL_MASK 0xF / _SHIFT 12
 * upmu_common.c:21866  upmu_set_isinks_breath0_trf_sel() -> ISINKS_CON8
 *
 * The register is named BREATH because the same field also selects the clock
 * for the hardware breathing mode, whose on/off lengths come from two further
 * nibbles in the same register - TON_SEL at [11:8] and TOFF_SEL at [7:0]
 * (upmu_hw.h:3698-3701).  This driver programs only TRF_SEL: breathing is NOT
 * implemented and is not wired up to any LED API, so no userspace can request
 * it, and a "brightness 0..max" request drives the current step instead,
 * exactly as the vendor steady path does.
 *
 * That is a deliberate omission, not an oversight.  The only BSP code that
 * programs TON_SEL/TOFF_SEL is led_breath_pmic(), and it is itself disabled:
 * the whole function sits inside an "#if 0" in the vendor leds.c, and the
 * values it would write (ton 0x02, toff 0x03) are fixed magic numbers rather
 * than anything derived from a delay.  With no enabled reference for how the
 * nibbles are meant to be computed, guessing the encoding would be inventing
 * behaviour.
 */
#define MT6320_ISINK_TRF_SEL_MASK	GENMASK(15, 12)

/*
 * struct mt6320_led - one current sink.  Channel indices match the
 * "mediatek,mt6320-led" DT "reg" property.
 * @channel:	sink index, 0..MT6320_MAX_LEDS-1
 * @parent:	the controller this sink belongs to
 * @cdev:	the LED class device
 * @current_brightness:	cached level, 0 when the sink is off
 * @blink_active:	a hardware blink pattern is programmed and running
 * @boost_ref:	this sink holds a reference on the shared boost clock
 * @dim_steady:	the dimming counter and its clock select are programmed in
 *		the steady (non-blink) configuration
 * @blink_on:	last programmed hardware blink on-time in ms
 * @blink_off:	last programmed hardware blink off-time in ms
 *
 * @blink_on and @blink_off are the timings currently in the hardware, since the
 * LED core asks for "the same blink as before" by passing both delays as zero.
 * They start out at the DT "delay-on"/"delay-off" values and are refreshed on
 * every successful non-zero program.
 *
 * Blinking is deliberately NOT tracked in @current_brightness, which stays at
 * the level the user last asked for so brightness_get() does not claim a level
 * nobody programmed.  @blink_active says the dimming counter is running a
 * pattern instead of the steady duty.
 *
 * @boost_ref is NOT the same thing as "this sink is lit": a boost release whose
 * regmap access fails leaves @boost_ref set, because bst_users was rolled back
 * with it, so a dark sink can still hold a reference.  @dim_steady exists
 * because of that: the dimming counter is programmed from exactly one of two
 * places - mt6320_led_set_steady(), or the blink pair mt6320_led_set_blink()
 * and mt6320_led_set_blink_clksel() - so what it holds cannot be inferred from
 * @blink_active (false for a sink that has simply never blinked) nor from
 * @boost_ref (true for a dark sink whose release failed).
 */
struct mt6320_kpled;

struct mt6320_led {
	unsigned int			channel;
	struct mt6320_leds		*parent;
	struct led_classdev		cdev;
	enum led_brightness		current_brightness;
	bool				blink_active;
	bool				boost_ref;
	bool				dim_steady;
	unsigned long			blink_on;
	unsigned long			blink_off;
};

/*
 * struct mt6320_leds - the whole controller
 * @dev:	the device doing the register writes
 * @regmap:	the MT6320 regmap, borrowed from the parent MFD device
 * @lock:	serialises brightness changes between LEDs
 * @led:	per-channel state, NULL where the board has no LED fitted
 * @bst_users: number of sinks currently holding the shared boost clock on
 *
 * @bst_users counts the sinks that want the 1 MHz boost clock, and is only ever
 * mutated with @lock held, which also serialises it against the register write
 * it guards.
 */
struct mt6320_leds {
	struct device			*dev;
	struct regmap			*regmap;
	struct mutex			lock;
	struct mt6320_led		*led[MT6320_MAX_LEDS];
#ifdef CONFIG_LEDS_MT6320_KPLED
	/* dedicated keypad sink, if the board has one */
	struct mt6320_kpled		*kpled;
#endif
	unsigned int			bst_users;
};

/* Dimming counter per channel (ISINKS_CON0/1/2) and mode+step per channel
 * (ISINKS_CON4/5/6).  One register serves every channel within each group,
 * hence a per-channel table rather than three separate sets.
 */
static const u16 mt6320_isink_dim_reg[] = {
	MT6320_ISINKS_CON0, MT6320_ISINKS_CON1, MT6320_ISINKS_CON2,
};

static const u16 mt6320_isink_cfg_reg[] = {
	MT6320_ISINKS_CON4, MT6320_ISINKS_CON5, MT6320_ISINKS_CON6,
};

/*
 * Per-channel clock-select registers used for blink timing: the "breathN_
 * trf_sel" fields the BSP uses (upmu_common.c:21866 for ch0).
 *
 * upmu_hw.h:405-410  ISINKS_CON7..CON10 = 0x0578, 0x057A, 0x057C, 0x057E
 */
static const u16 mt6320_isink_trf_reg[] = {
	MT6320_ISINKS_CON8, MT6320_ISINKS_CON9, MT6320_ISINKS_CON10,
};

#define MT6320_ISINKS_EN_REG	MT6320_ISINKS_CON3

/*
 * Un-gate or gate the shared 1 MHz boost-converter clock.
 *
 * Callers must hold leds->lock, which is what every path that races with a
 * concurrent brightness change does; the only exception is mt6320_led_remove(),
 * which is single-threaded teardown.  The counter is unsigned and clamped at
 * zero on the release path, so a stray release cannot wrap it round and leave
 * the clock permanently gated.
 *
 * The bit is programmed in TOP_CKPDN (0x0102, upmu_hw.h:38, matching
 * include/linux/mfd/mt6320/registers.h) by read-modify-write, deliberately not
 * through MT6320_TOP_CKPDN_SET (0x0104) or MT6320_TOP_CKPDN_CLR (0x0106).
 * Those are write-one-to-set and write-one-to-clear shadows: a bit written to
 * 0 in the _CLR shadow is simply not written, so it stays put and cannot
 * ungate anything.  The vendor never uses them for this bit;
 * upmu_set_rg_bst_drv_1m_ck_pdn() calls
 * pmic_config_interface(TOP_CKPDN, val, PMIC_RG_BST_DRV_1M_CK_PDN_MASK,
 * PMIC_RG_BST_DRV_1M_CK_PDN_SHIFT) (upmu_common.c:1379-1386), i.e. a
 * read-modify-write on the register itself, under pmic_lock().
 *
 * Only the first and last user touch the register: the count decides when the
 * rail is actually switched, so a call at a non-transition count costs nothing
 * but a decrement.
 */
static int mt6320_led_bst_clk(struct mt6320_leds *leds, bool enable)
{
	unsigned int val;
	int ret;

	if (enable) {
		if (leds->bst_users++)
			return 0;
	} else {
		if (!leds->bst_users)
			return 0;
		if (--leds->bst_users)
			return 0;
	}

	ret = regmap_read(leds->regmap, MT6320_TOP_CKPDN, &val);
	if (ret)
		goto err_count;

	if (enable)
		val &= ~MT6320_BST_DRV_1M_CK_PDN;
	else
		val |= MT6320_BST_DRV_1M_CK_PDN;

	ret = regmap_write(leds->regmap, MT6320_TOP_CKPDN, val);

err_count:
	if (ret) {
		if (enable)
			leds->bst_users--;
		else
			leds->bst_users++;
	}

	return ret;
}

/*
 * Take and drop this sink's reference on the shared boost clock.
 *
 * These two wrappers are the only places that mutate led->boost_ref, and both
 * do so only on the real edge, so the flag cannot drift out of step with the
 * counter.
 *
 * The release (mt6320_led_drop_boost() below) is transactional in exactly the
 * same way and for the same reason: mt6320_led_bst_clk() rolls bst_users back
 * when the regmap access fails, so it leaves the counter on its pre-call value
 * and reports the error.  Clearing boost_ref regardless of that error would be
 * a half-applied release - the flag says this sink holds nothing while the
 * controller still counts it - and the next take would then add a second
 * reference for one lit sink, running bst_users permanently one high and
 * parking the rail forever.
 *
 * Taking is idempotent, so the caller cannot tell whether it just acquired the
 * reference or found one already held - which is exactly what an error path
 * needs to know.  The take therefore reports that through @acquired, and only
 * a reference this call really acquired may be released again: dropping one
 * held before the call leaves a sink whose enable bit is still set - a lit LED
 * - with the boost rail gated off underneath it, and nobody left to take that
 * reference back.
 *
 * Callers must hold leds->lock (mt6320_led_remove() being the documented
 * exception), which is also what mt6320_led_bst_clk() requires.
 */
static int mt6320_led_take_boost(struct mt6320_led *led, bool *acquired)
{
	struct mt6320_leds *leds = led->parent;
	int ret;

	*acquired = false;

	if (led->boost_ref)
		return 0;

	ret = mt6320_led_bst_clk(leds, true);
	if (!ret) {
		led->boost_ref = true;
		*acquired = true;
	}

	return ret;
}

/*
 * Release this sink's reference on the shared boost rail.
 *
 * Returns 0 when there is nothing to release or the release succeeded, and
 * a negative errno when the release failed and boost_ref was left set to stay
 * consistent with the bst_users that mt6320_led_bst_clk() rolled back.  The
 * caller decides whether that is worth reporting: the LED has already been
 * switched off by then, so it must not turn a successful brightness change
 * into an error.  Keeping boost_ref set on failure is what makes the retry
 * possible - the state stays consistent and the next hw_off() drains it.
 */
static int mt6320_led_drop_boost(struct mt6320_led *led)
{
	struct mt6320_leds *leds = led->parent;
	int ret;

	if (!led->boost_ref)
		return 0;

	ret = mt6320_led_bst_clk(leds, false);
	if (ret)
		return ret;

	led->boost_ref = false;

	return 0;
}

/*
 * The mode field travels with the step because the BSP writes both through the
 * same ISINKS_CON4/5/6 word, always with PMIC_PWM_0.
 */
static int mt6320_led_set_step(struct mt6320_led *led, u8 step)
{
	struct mt6320_leds *leds = led->parent;

	return regmap_update_bits(leds->regmap,
				  mt6320_isink_cfg_reg[led->channel],
				  MT6320_ISINK_CH_STEP_MASK |
				  MT6320_ISINK_CH_MODE_MASK,
				  FIELD_PREP(MT6320_ISINK_CH_STEP_MASK, step) |
				      FIELD_PREP(MT6320_ISINK_CH_MODE_MASK,
					     MT6320_ISINK_CH_MODE_PWM0));
}

/* The clamp keeps a step inside the three-bit field whatever it is handed. */
static u8 mt6320_led_brightness_step(struct mt6320_led *led,
				     enum led_brightness brightness)
{
	return min_t(unsigned int, brightness, led->cdev.max_brightness) - 1;
}

/*
 * The brightness a blink is run at: the one the user last left the sink at, or
 * the bottom step code if it was dark when the blink started.  The vendor
 * starts a blink from dark at the smallest step its sink uses,
 * upmu_set_isinks_ch0_step(0x0) (leds.c:300), commented 4 mA.  Keeping that
 * rule here, rather than in the blink path, is what stops
 * mt6320_led_set_step() and brightness_get() from ever disagreeing about the
 * current in the step field.
 */
static enum led_brightness
mt6320_led_blink_brightness(struct mt6320_led *led)
{
	return led->current_brightness ? led->current_brightness : LED_ON;
}

/*
 * Put the sink into a steady level rather than a blink pattern.
 *
 * The level itself is NOT expressed here: it lives in the current step, which
 * mt6320_led_hw_on() programs before calling this.  This only puts the dimming
 * counter into its always-on configuration, so the sink is not mid-blink when
 * the step is changed.
 */
static int mt6320_led_set_steady(struct mt6320_led *led)
{
	struct mt6320_leds *leds = led->parent;
	u32 dim;
	int ret;

	/* Clock select first, then the duty/fsel pair, so the counter is never
	 * reconfigured while running at a stale divider.
	 */
	dim = FIELD_PREP(MT6320_ISINK_DIM_DUTY_MASK, MT6320_STEADY_DUTY) |
	      FIELD_PREP(MT6320_ISINK_DIM_FSEL_MASK, MT6320_STEADY_FSEL);

	ret = regmap_update_bits(leds->regmap,
				 mt6320_isink_trf_reg[led->channel],
				 MT6320_ISINK_TRF_SEL_MASK,
				 FIELD_PREP(MT6320_ISINK_TRF_SEL_MASK,
					    MT6320_STEADY_TRF_SEL));
	if (ret)
		goto err_steady;

	ret = regmap_update_bits(leds->regmap,
				 mt6320_isink_dim_reg[led->channel],
				 MT6320_ISINK_DIM_DUTY_MASK |
				 MT6320_ISINK_DIM_FSEL_MASK, dim);

	if (!ret)
		led->dim_steady = true;

	return ret;

err_steady:
	/* A partially applied steady configuration is not the steady
	 * configuration, so the next mt6320_led_hw_on() programs it again.
	 */
	led->dim_steady = false;

	return ret;
}

/*
 * Program a hardware blink pattern.
 *
 * fsel is NOT a period divider.  The period is chosen by a clock-select
 * field (the "trf_sel" nibble in ISINKS_CON8/9/10, bits [15:12]) and fsel is
 * only a small correction divider within that clock.  The BSP drives it as a
 * lookup: a table of supported periods, a parallel table of clock selects,
 * and a parallel table of fsel values (leds.c:264-268)
 *
 *	pmic_period_array[] = {250,500,1000,1250,1666,2000,2500,3333,4000,5000,6666,8000,10000};
 *	pmic_clksel_array[]  = {  0,  0,   0,   0,   0,   0,    1,    1,    1,    2,    2,    2,     3};
 *	pmic_freqsel_array[] = { 21, 22,  23,  24,  24,  24,   25,   25,   26,   26,   28,   28,    28};
 *
 * and picks the first entry whose period covers the requested one
 * (find_time_index_pmic(), leds.c:271-281).  The duty field is a plain
 * percentage of the period, computed as 32*on/period (leds.c:295).
 *
 * Those tables are reproduced verbatim below rather than re-derived: the
 * clock-select encoding is not documented anywhere in this tree.  The exact
 * resulting frequency is not stated by the BSP, so this is "the vendor's rate
 * for this period", not a verified Hz figure.  Every entry is >= 250 ms, so a
 * faster request rounds up to 250 ms.
 *
 * One deliberate divergence: find_time_index_pmic() clamps an over-long
 * request to the last entry (10000 ms).  This driver returns -EINVAL instead,
 * so the LED core falls back to its software timer and blinks at the rate
 * actually requested rather than at a capped rate the caller did not ask for.
 */
static int mt6320_led_blink_timings(struct mt6320_led *led,
				    unsigned int on_ms, unsigned int period_ms,
				    unsigned int *duty, unsigned int *fsel,
				    unsigned int *clksel)
{
	static const unsigned int tbl_period[] = {
		250, 500, 1000, 1250, 1666, 2000, 2500, 3333, 4000, 5000,
		6666, 8000, 10000,
	};
	static const u8 tbl_clksel[] = {
		0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3,
	};
	static const u8 tbl_fsel[] = {
		21, 22, 23, 24, 24, 24, 25, 25, 26, 26, 28, 28, 28,
	};
	unsigned int i;

	if (!period_ms || on_ms >= period_ms)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(tbl_period); i++)
		if (period_ms <= tbl_period[i])
			break;
	if (i == ARRAY_SIZE(tbl_period))
		return -EINVAL;

	*duty = min_t(unsigned int, 32u * on_ms / period_ms, MT6320_MAX_DUTY);
	if (on_ms && !*duty)
		*duty = 1;

	*fsel = tbl_fsel[i];
	*clksel = tbl_clksel[i];

	return 0;
}

/*
 * Program the dimming counter for a hardware blink.
 *
 * Only the dimming counter is touched here - never the current step, the
 * boost clock or the enable bit.  The caller owns the current level and the
 * enable, and needs to program the step before this one and the clock select
 * after it, exactly the way the BSP orders them (see
 * mt6320_led_hw_blink_set()).
 *
 * This also clears @dim_steady, since programming a pattern and programming the
 * steady configuration are mutually exclusive claims on the same register.
 */
static int mt6320_led_set_blink(struct mt6320_led *led, unsigned int duty,
				unsigned int fsel)
{
	struct mt6320_leds *leds = led->parent;
	int ret;

	ret = regmap_update_bits(leds->regmap,
				 mt6320_isink_dim_reg[led->channel],
				 MT6320_ISINK_DIM_DUTY_MASK | MT6320_ISINK_DIM_FSEL_MASK,
				 FIELD_PREP(MT6320_ISINK_DIM_DUTY_MASK, duty) |
				 FIELD_PREP(MT6320_ISINK_DIM_FSEL_MASK, fsel));

	/* Dropped whether the write succeeded or not: on failure nothing is
	 * known about the counter, which is what "not steady" describes.
	 */
	led->dim_steady = false;

	return ret;
}

/*
 * Program the clock select that runs the blink pattern.  This travels in the
 * trf_sel nibble of ISINKS_CON8/9/10, the same field the steady path parks at
 * MT6320_STEADY_TRF_SEL.
 */
static int mt6320_led_set_blink_clksel(struct mt6320_led *led,
				       unsigned int clksel)
{
	struct mt6320_leds *leds = led->parent;
	int ret;

	ret = regmap_update_bits(leds->regmap,
				 mt6320_isink_trf_reg[led->channel],
				 MT6320_ISINK_TRF_SEL_MASK,
				 FIELD_PREP(MT6320_ISINK_TRF_SEL_MASK, clksel));

	led->dim_steady = false;

	return ret;
}

/*
 * This deliberately does NOT touch the shared boost clock.  Both callers take
 * their own boost reference with mt6320_led_take_boost() before calling this
 * and unwind it themselves if this returns an error, so the ownership of a
 * reference stays with the function that took it and there is exactly one
 * release per acquisition.
 */
static int mt6320_led_hw_enable(struct mt6320_led *led)
{
	struct mt6320_leds *leds = led->parent;

	return regmap_update_bits(leds->regmap, MT6320_ISINKS_EN_REG,
				 MT6320_ISINK_CH_EN(led->channel),
				 MT6320_ISINK_CH_EN(led->channel));
}

/*
 * @dim_steady, not @boost_ref, decides whether the dimming counter has to be
 * programmed again: a sink coming up from dark, one whose steady program
 * failed, and one left holding a boost reference by a failed release are all
 * "boost_ref may or may not be set, dim_steady is false".
 */
static int mt6320_led_hw_on(struct mt6320_led *led,
			    enum led_brightness brightness)
{
	bool took_boost = false;
	int ret;

	/* Leaving a blink, or coming up from dark, needs the counter running
	 * steadily before the step changes, otherwise the sink would switch
	 * current mid-pattern.  This is what stops a hardware blink when the
	 * user writes a brightness; the LED core relies on the brightness
	 * callback doing it - see the "Deactivate blinking again when the
	 * brightness is set to LED_OFF" contract on struct led_classdev
	 * (include/linux/leds.h).
	 */
	if (!led->dim_steady) {
		ret = mt6320_led_set_steady(led);
		if (ret)
			return ret;
	}

	ret = mt6320_led_take_boost(led, &took_boost);
	if (ret)
		return ret;

	ret = mt6320_led_set_step(led, mt6320_led_brightness_step(led, brightness));
	if (ret)
		goto err_unref;

	/* Dropped after the enable, so a failed write leaves the old state. */
	ret = mt6320_led_hw_enable(led);
	if (ret)
		goto err_unref;

	led->blink_active = false;

	return 0;

err_unref:
	/* Only a reference this call took: one held before the call belongs to a
	 * sink that was already enabled, and releasing it would gate the boost
	 * rail out from under a lit LED.
	 */
	if (took_boost)
		mt6320_led_drop_boost(led);

	return ret;
}

/*
 * Clearing the enable bit is what physically darkens the sink, and tearing
 * down the blink state is what the LED core means by brightness 0 stopping
 * hardware blinking.  Leaving a blink armed here would mean a later
 * brightness write re-enabled the sink into the old pattern, since the enable
 * bit is all the hardware needs to resume it.
 *
 * The boost reference is dropped last, and only once the enable bit has really
 * been cleared: until then the sink may still be lit and drawing from the
 * rail.  A failed enable write therefore returns early and keeps both the
 * blink state and the reference, which is consistent rather than a leak - the
 * next write to this LED drains it.  The drop is unconditional rather than on
 * a brightness edge because it is the release path that has to be able to
 * retry a release that failed last time; a sink that never held a reference
 * returns immediately.
 */
static int mt6320_led_hw_off(struct mt6320_led *led)
{
	int ret;

	ret = regmap_clear_bits(led->parent->regmap, MT6320_ISINKS_EN_REG,
				MT6320_ISINK_CH_EN(led->channel));
	if (ret)
		return ret;

	/* dim_steady goes with the blink state: the enable bit is clear, so
	 * whatever pattern was armed cannot run, and a dark sink's dimming
	 * registers are whatever the last attempt left there.
	 */
	led->blink_active = false;
	led->dim_steady = false;

	/* A failed release leaves the rail counted as held and on for a dark
	 * sink: a power cost, not a correctness problem, since the state is
	 * coherent and the next brightness 0 here - or mt6320_led_remove() -
	 * drains it.  Report it, but only after the sink is already dark: the
	 * brightness change itself succeeded, so the LED core must not see an
	 * error for it.
	 */
	ret = mt6320_led_drop_boost(led);
	if (ret)
		dev_warn(led->parent->dev,
			 "failed to release boost clock for channel %u; rail left on\n",
			 led->channel);

	return 0;
}

static int mt6320_led_set_brightness(struct led_classdev *cdev,
				     enum led_brightness brightness)
{
	struct mt6320_led *led = container_of(cdev, struct mt6320_led, cdev);
	int ret;

	mutex_lock(&led->parent->lock);

	if (!brightness)
		ret = mt6320_led_hw_off(led);
	else
		ret = mt6320_led_hw_on(led, brightness);

	if (!ret)
		led->current_brightness = brightness;

	mutex_unlock(&led->parent->lock);

	return ret;
}

static enum led_brightness
mt6320_led_get_brightness(struct led_classdev *cdev)
{
	struct mt6320_led *led = container_of(cdev, struct mt6320_led, cdev);
	enum led_brightness brightness;
	unsigned int status;
	int ret;

	ret = regmap_read(led->parent->regmap, MT6320_ISINKS_EN_REG, &status);
	if (ret)
		return led->current_brightness;

	if (!(status & MT6320_ISINK_CH_EN(led->channel)))
		return LED_OFF;

	/*
	 * While blinking, report the level the pattern is programmed at rather
	 * than led->current_brightness, which is 0 for a blink started from
	 * dark.  Reporting 0 there would tell the LED core the sink is off
	 * while its enable bit is plainly set, and led_set_software_blink()
	 * would cache LED_OFF as the level to return to if it ever falls back
	 * to a software blink.  mt6320_led_blink_brightness() is the same value
	 * mt6320_led_hw_blink_set() put in the step field, so this cannot
	 * disagree with the hardware.
	 */
	brightness = led->blink_active ?
		      mt6320_led_blink_brightness(led) : led->current_brightness;

	return brightness;
}

/*
 * Hardware blink.  Anything the counter cannot express is handed back to the
 * LED core, which falls back to its own timer when this returns -EINVAL.
 *
 * The programming order reproduces the BSP's mt_led_blink_pmic() for ISINK0
 * (leds.c:296-313):
 *
 *	leds.c:298  upmu_set_rg_bst_drv_1m_ck_pdn(0x0)   boost clock ungated
 *	leds.c:299  upmu_set_isinks_ch0_mode(PMIC_PWM_0) dimming source
 *	leds.c:300  upmu_set_isinks_ch0_step(0x0)        current step
 *	leds.c:301  upmu_set_isink_dim0_duty(duty)        on-fraction
 *	leds.c:302  upmu_set_isink_dim0_fsel(fsel)        frequency trim
 *	leds.c:308  upmu_set_isinks_breath0_trf_sel(0x0) blink clock select
 *	leds.c:312  upmu_set_isinks_ch0_en(0x01)         sink enabled
 *
 * ISINK1 (leds.c:314-329) and ISINK2 (leds.c:330-344) are the same sequence on
 * their own channel.  Note that the step is programmed in the middle of it:
 * entering a blink from dark without writing the step leaves the sink running
 * at whatever reset value the field happened to hold.  The boost rail goes up
 * first and the sink is enabled last, so the pattern is never visible at a
 * stale step, on a gated rail, or with a stale divider.
 */
static int mt6320_led_hw_blink_set(struct led_classdev *cdev,
				   unsigned long *delay_on,
				   unsigned long *delay_off)
{
	struct mt6320_led *led = container_of(cdev, struct mt6320_led, cdev);
	unsigned int on_ms, period_ms, duty, fsel, clksel;
	bool took_boost = false;
	int ret;

	if (!*delay_on && !*delay_off) {
		*delay_on = led->blink_on;
		*delay_off = led->blink_off;
	}

	if (!*delay_on || !*delay_off)
		return -EINVAL;

	on_ms = *delay_on;
	period_ms = *delay_on + *delay_off;

	/* Everything below touches the same per-channel registers that
	 * brightness_set does, so the whole sequence is done under the lock -
	 * otherwise a concurrent brightness change can interleave with the
	 * dimming setup.
	 */
	mutex_lock(&led->parent->lock);

	/* Resolve the timings before touching hardware, so a request the counter
	 * cannot express is rejected without having un-gated the boost rail.
	 */
	ret = mt6320_led_blink_timings(led, on_ms, period_ms, &duty, &fsel,
					&clksel);
	if (ret)
		goto out_unlock;

	/* leds.c:298 - the boost rail must be up before the sink is lit. */
	ret = mt6320_led_take_boost(led, &took_boost);
	if (ret)
		goto out_unlock;

	/* leds.c:299-300 - dimming source and current step, which
	 * mt6320_led_set_step() writes together in the ISINKS_CON4/5/6 word.
	 */
	ret = mt6320_led_set_step(led,
				  mt6320_led_brightness_step(led, mt6320_led_blink_brightness(led)));
	if (ret)
		goto err_unref;

	/* leds.c:301-302 - on-fraction and frequency trim. */
	ret = mt6320_led_set_blink(led, duty, fsel);
	if (ret)
		goto err_unref;

	/* leds.c:308 - the clock select that runs the pattern. */
	ret = mt6320_led_set_blink_clksel(led, clksel);
	if (ret)
		goto err_unref;

	/* leds.c:312 - enable the sink last, deliberately not through
	 * mt6320_led_hw_on(): that would put the dimming counter back into the
	 * steady configuration and wipe out the pattern programmed above.
	 */
	ret = mt6320_led_hw_enable(led);
	if (ret)
		goto err_unref;

	led->blink_on = on_ms;
	led->blink_off = *delay_off;
	led->blink_active = true;

	mutex_unlock(&led->parent->lock);

	return 0;

err_unref:
	/* As in mt6320_led_hw_on(): only a reference this call took. */
	if (took_boost)
		mt6320_led_drop_boost(led);

out_unlock:
	mutex_unlock(&led->parent->lock);

	return ret;
}

/*
 * Keypad backlight: a fourth, independent sink on KPLED_CON0 rather than one
 * of the three ISINK channels, so it gets its own class device and a pair of
 * simple on/off helpers rather than sharing the channel machinery above.
 *
 * Both CONFIG_LEDS_MT6320_KPLED and a DT node with "mediatek,is-kpled" are
 * required for a sink to exist: the symbol is the build-wide statement "this
 * kernel drives the MT6320 keypad sink", DT says whether a particular board
 * has such a sink wired up.  A keypad node on a build without the symbol is
 * skipped rather than treated as an error.
 */
#ifdef CONFIG_LEDS_MT6320_KPLED

struct mt6320_kpled {
	struct mt6320_leds		*parent;
	struct led_classdev		cdev;
	enum led_brightness		current_brightness;
};

static enum led_brightness
mt6320_kpled_get_brightness(struct led_classdev *cdev)
{
	struct mt6320_kpled *kpled = container_of(cdev, struct mt6320_kpled, cdev);
	unsigned int status;

	if (regmap_read(kpled->parent->regmap, MT6320_KPLED_CON0, &status))
		return kpled->current_brightness;

	return status & MT6320_KPLED_EN ? LED_ON : LED_OFF;
}

static int mt6320_kpled_set_brightness(struct led_classdev *cdev,
				       enum led_brightness brightness)
{
	struct mt6320_kpled *kpled = container_of(cdev, struct mt6320_kpled, cdev);
	int ret;

	mutex_lock(&kpled->parent->lock);

	if (!brightness) {
		ret = regmap_clear_bits(kpled->parent->regmap,
					 MT6320_KPLED_CON0, MT6320_KPLED_EN);
	} else {
		/* Duty 9 is what the BSP programs for this sink,
		 * upmu_set_kpled_dim_duty(0x9) (leds.c:506-507).
		 */
		ret = regmap_update_bits(kpled->parent->regmap,
					 MT6320_KPLED_CON0,
					 MT6320_KPLED_EN | MT6320_KPLED_DIM_DUTY_MASK,
					 MT6320_KPLED_EN |
					 FIELD_PREP(MT6320_KPLED_DIM_DUTY_MASK, 9));
	}

	if (!ret)
		kpled->current_brightness = brightness;

	mutex_unlock(&kpled->parent->lock);

	return ret;
}

#endif /* CONFIG_LEDS_MT6320_KPLED */

static int mt6320_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev_of_node(dev);
	struct mt6397_chip *chip = dev_get_drvdata(dev->parent);
	struct mt6320_leds *leds;
	struct mt6320_led *led;
	int ret;

	if (!chip || !chip->regmap)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "no MT6320 regmap from parent\n");

	leds = devm_kzalloc(dev, sizeof(*leds), GFP_KERNEL);
	if (!leds)
		return -ENOMEM;

	leds->dev = dev;
	leds->regmap = chip->regmap;
	mutex_init(&leds->lock);
	platform_set_drvdata(pdev, leds);

	for_each_available_child_of_node_scoped(np, child) {
		struct led_init_data init_data = {};
#ifdef CONFIG_LEDS_MT6320_KPLED
		struct mt6320_kpled *kpled;
#endif
		u32 reg, num_steps, delay_on = 0, delay_off = 0;

		/* A keypad child never consumes an ISINK channel, compiled in or
		 * not, so skip the rest of the body either way.
		 */
		if (of_property_read_bool(child, "mediatek,is-kpled")) {
#ifdef CONFIG_LEDS_MT6320_KPLED
			kpled = devm_kzalloc(dev, sizeof(*kpled), GFP_KERNEL);
			if (!kpled)
				return -ENOMEM;

			leds->kpled = kpled;
			kpled->parent = leds;
			kpled->cdev.max_brightness = 1;
			kpled->cdev.brightness_set_blocking =
						mt6320_kpled_set_brightness;
			kpled->cdev.brightness_get =
				mt6320_kpled_get_brightness;

			if (of_property_read_string(child, "label",
						   &kpled->cdev.name) ||
			    !kpled->cdev.name)
				kpled->cdev.name = dev_name(dev);

			init_data.fwnode = of_fwnode_handle(child);
			ret = devm_led_classdev_register_ext(dev, &kpled->cdev,
							    &init_data);
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to register keypad LED\n");
#endif
			continue;
		}

		ret = of_property_read_u32(child, "reg", &reg);
		if (ret)
			return dev_err_probe(dev, ret, "missing led 'reg'\n");

		if (reg >= MT6320_MAX_LEDS || leds->led[reg])
			return dev_err_probe(dev, -EINVAL,
					     "invalid led reg %u\n", reg);

		led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->channel = reg;
		led->parent = leds;
		leds->led[reg] = led;

		/* "mediatek,num-steps" sizes the brightness range and is the
		 * only knob for the current level; there is no separate
		 * per-LED current property competing with it.
		 */
		reg = MT6320_MAX_BRIGHTNESS;
		ret = of_property_read_u32(child, "mediatek,num-steps", &num_steps);
		if (!ret)
			reg = num_steps;
		else if (ret != -EINVAL)
			return dev_err_probe(dev, ret, "bad num-steps\n");

		/* brightness must be at least 1, and must fit the step field */
		if (!reg || reg > MT6320_MAX_STEP + 1)
			return dev_err_probe(dev, -EINVAL,
					     "num-steps %u out of range\n",
					     reg);

		led->cdev.max_brightness = reg;
		led->cdev.brightness_set_blocking = mt6320_led_set_brightness;
		led->cdev.brightness_get = mt6320_led_get_brightness;
		led->cdev.blink_set = mt6320_led_hw_blink_set;

		/* Seed the blink cache with the defaults this LED should use when
		 * it has never been blinked explicitly.  A sink that asks for
		 * neither keeps an empty cache, and mt6320_led_hw_blink_set()
		 * then declines to guess.
		 */
		of_property_read_u32(child, "delay-on", &delay_on);
		of_property_read_u32(child, "delay-off", &delay_off);
		if (delay_on && delay_off) {
			led->blink_on = delay_on;
			led->blink_off = delay_off;
		}

		init_data.fwnode = of_fwnode_handle(child);
		ret = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to register LED\n");

		/* Honour the DT default state before the class device starts
		 * driving the sink.
		 */
		if (led_init_default_state_get(of_fwnode_handle(child)) ==
							 LEDS_DEFSTATE_ON) {
			ret = mt6320_led_set_brightness(&led->cdev,
							led->cdev.max_brightness);
			if (ret)
				return dev_err_probe(dev, ret,
						     "failed to set default state\n");
		}
	}

	return 0;
}

static void mt6320_led_remove(struct platform_device *pdev)
{
	struct mt6320_leds *leds = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < MT6320_MAX_LEDS; i++)
		if (leds->led[i])
			mt6320_led_hw_off(leds->led[i]);

#ifdef CONFIG_LEDS_MT6320_KPLED
	if (leds->kpled)
		mt6320_kpled_set_brightness(&leds->kpled->cdev, LED_OFF);
#endif

	mutex_destroy(&leds->lock);
}

static const struct of_device_id mt6320_led_of_match[] = {
	{ .compatible = "mediatek,mt6320-led" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6320_led_of_match);

static struct platform_driver mt6320_led_driver = {
	.probe		= mt6320_led_probe,
	.remove		= mt6320_led_remove,
	.driver		= {
		.name		= "mt6320-led",
		.of_match_table	= mt6320_led_of_match,
	},
};

module_platform_driver(mt6320_led_driver);

MODULE_DESCRIPTION("LED driver for MediaTek MT6320 PMIC");
MODULE_LICENSE("GPL");
