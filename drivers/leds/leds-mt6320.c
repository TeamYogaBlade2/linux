// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LED driver for the MediaTek MT6320 PMIC
 *
 * Drives the three constant-current LED (ISINK / "NLED") sinks that the MT6320
 * exposes, plus the dedicated keypad LED.  The sinks are wired to LEDs on the
 * board rather than to the SoC GPIOs, so they cannot be handled by
 * drivers/leds/leds-gpio.c: each sink needs its own current step, its own
 * PWM-mode selection and a shared boost clock, all inside the PMIC.
 *
 * Register map and bit positions come from the downstream MT6589 sources:
 *   - aquaris-5/mediatek/platform/mt6589/kernel/core/include/mach/upmu_hw.h
 *       ISINKS_CON0..6, KPLED_CON0, TOP_CKPDN, TOP_CKCON1 and the matching
 *       PMIC_ISINK / PMIC_ISINKS / PMIC_KPLED mask-and-shift pairs
 *   - aquaris-5/mediatek/platform/mt6589/kernel/drivers/power/upmu_common.c
 *       the upmu_set_isinks_chN_en/mode/step(),
 *       upmu_set_isink_dimN_duty/fsel(), upmu_set_rg_bst_drv_1m_ck_pdn()
 *       and upmu_set_kpled_en/dim_duty() helpers
 *   - aquaris-5/mediatek/platform/mt6589/kernel/drivers/leds/leds.c
 *       the sequences in mt_set_led_brightness() this driver reproduces
 *       (steady state, blink table, keypad LED)
 *
 * All the register addresses used here already exist in the mainline header
 * (include/linux/mfd/mt6320/registers.h) and were cross-checked against the
 * vendor header above; none of them needed to be added.
 *
 * Datasheet cross-check is only possible for the addresses, not the bits: the
 * MT6320 is a companion PMIC and its register manual is not part of the
 * MT6589 SoC datasheet (grep -c ISINK on the extracted MT6589 datasheet text
 * returns 0).  Every bit position below is therefore taken from the vendor
 * header and marked VERIFIED-HEADER in NOTES.md.
 *
 * This deliberately does not extend drivers/leds/rgb/leds-mt6370-rgb.c or
 * drivers/leds/leds-mt6323.c.  The MT6370 driver drives an 8-bit-wide RGB
 * block at 0x182-0x194 with one register per attribute per channel, while the
 * MT6320 sinks are a 16-bit block at 0x056a-0x0580 where three channels share
 * one enable register; no address overlaps and the register widths differ, so
 * neither driver can simply be pointed at this part.  See NOTES.md.
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

/*
 * MT6320 drives three LED current sinks, plus a separate constant-current
 * driver dedicated to the keypad backlight.  Channel indices match the
 * "mediatek,mt6320-led" DT "reg" property and the ISINKS_CONx registers.
 */
#define MT6320_MAX_LEDS		3

/*
 * TOP_CKPDN bit 7, RG_BST_DRV_1M_CK_PDN, is the 1 MHz boost-converter clock
 * that feeds the current sinks.  It is shared by all three sinks, so it is
 * reference counted by mt6320_leds::bst_users rather than toggled per LED -
 * gating it while another sink is lit would tear down the boost rail under
 * that LED.  The downstream driver never gates it at all: across leds.c the
 * un-gate calls are live (lines 298, 315, 331, 546, 589, 632, 674) while every
 * gate-back call is commented out (556, 598, 641, 686), so the rail is always
 * running there.  Counting the users is the mainline equivalent: the rail is
 * parked as soon as the last sink goes dark instead of being left on forever.
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
 * writes PMIC_PWM_0 (leds_sw.h:12 enum { PMIC_PWM_0 = 0, ... }), i.e. the
 * channel follows the shared dimming counter selected by the PWM number.
 * Because this driver programs the counter itself, PMIC_PWM_0 is the only
 * mode value it needs; the other two encodings are left alone.
 *
 *   upmu_hw.h:3670-3687
 *     PMIC_ISINKS_CH{0,1,2}_MODE_MASK 0x3 / _SHIFT 8
 *     PMIC_ISINKS_CH{0,1,2}_STEP_MASK 0x7 / _SHIFT 12
 *
 * Step is the current code: 0 selects the smallest step, and the downstream
 * driver comments the extremes as 4 mA at code 0 (ch0, and ch1 in the
 * ISINK01 case) up to 16 mA at code 3 (leds.c:533, :577, :620).
 * The per-channel defaults below are the ones the BSP programs for this
 * board's LEDs.
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
 * Brightness model: the LED class brightness IS the current step.  A
 * brightness of N maps onto current step N-1, exactly as drivers/leds/
 * leds-mt6323.c does (ISINK_CH_STEP(brightness - 1), leds-mt6323.c:169),
 * which this driver follows deliberately.  brightness 0 switches the sink
 * off; 1..max_brightness select step 0..max_brightness-1.  There is exactly
 * one source of truth for the current level: mt6320_led_hw_on().  The
 * "mediatek,num-steps" DT property sizes that range, it does not compete
 * with it.
 *
 * The step field is three bits wide, so codes 0..7 are representable and the
 * largest useful max_brightness is 8.  The default below exposes codes 0..3,
 * which covers what the BSP programs: it drives a fixed current per channel
 * rather than scaling it, picking step 0 for ch0 (leds.c:533, commented
 * 4 mA) and step 3 for ch1 and ch2 (leds.c:577, :620).  Step 3 is therefore
 * the top code by default, so running such a channel at max_brightness
 * reproduces the BSP's level.
 *
 * Note the BSP's own comments disagree about what step 3 means: it is
 * labelled 16 mA at leds.c:620 for ch2 but 4 mA at leds.c:577 for ch1,
 * and leds.c:661 calls ch1 step 3 "4mA" as well.  The absolute current scale
 * is therefore NOT established (see NOTES.md), so only the step *codes* and
 * the direction brightness -> step come from here; the latter is this
 * driver's own convention, following leds-mt6323.c.
 *
 * Steady state: a solid LED is duty 15 with fsel 11 and the clock select
 * cleared, i.e. an always-on dimming counter rather than a PWM ratio.  These
 * are the BSP's own numbers for every ISINK channel it drives as a solid
 * backlight, each commented "6320 0.25KHz":
 *
 *	leds.c:535-536  ch0  upmu_set_isink_dim0_duty(15), _fsel(11)
 *	leds.c:579-580  ch1  upmu_set_isink_dim1_duty(15), _fsel(11)
 *	leds.c:622-623  ch2  upmu_set_isink_dim2_duty(15), _fsel(11)
 *	leds.c:657-658  ch0  duty 1, fsel 1  (the slow, dim ISINK01 variant)
 *
 * The steady path in the BSP writes dim duty and fsel but never touches
 * breathN_trf_sel; the trf_sel = 0 write in this area of the vendor driver
 * (leds.c:308, :320, :337) belongs to mt_led_blink_pmic().  This driver sets
 * trf_sel = 0 for the steady state anyway, because that is the only value
 * the BSP ever programs into the field (every trf_sel value in leds.c is 0,
 * including the un-GPIEd 0x04 at :231) and it keeps the field deterministic
 * across a blink -> steady transition.
 *
 * Note fsel is NOT a brightness knob here.  It is the dimming-counter period
 * divisor, and the duty/fsel pair is the on-fraction of that period; the LED
 * level is carried by the current step, not by this pair.  Brightness is
 * therefore represented purely by the current step, and the steady-state
 * duty/fsel below are a fixed, vendor-derived property of "on" rather than
 * something scaled with brightness.
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
 * divider, NOT by fsel alone.  See mt6320_led_set_blink() for the tables,
 * which are taken verbatim from the BSP.
 *
 * upmu_hw.h:3696-3709  PMIC_ISINKS_BREATH{0,1,2}_TRF_SEL_MASK 0x4 / _SHIFT 12
 * upmu_common.c:21866  upmu_set_isinks_breath0_trf_sel() -> ISINKS_CON8
 */
#define MT6320_ISINK_TRF_SEL_MASK	GENMASK(15, 12)

/*
 * struct mt6320_led - one current sink
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
 * @blink_on and @blink_off are the timings currently programmed in the
 * hardware.  The LED core may ask for "the same blink as before" by passing
 * both delays as zero, so the driver needs to remember what it programmed in
 * order to answer that without guessing.  They start out at the DT
 * "delay-on"/"delay-off" values, and are refreshed on every successful
 * non-zero program, so they never go stale.
 *
 * Blinking is deliberately NOT tracked in @current_brightness.  The two are
 * separate things: @current_brightness is the level the user last asked for
 * through brightness_set(), and it keeps that value for the whole of a blink
 * so brightness_get() does not claim a level nobody programmed.  @blink_active
 * says the dimming counter is running a pattern instead of the steady duty.
 *
 * @boost_ref is the sink's share of the shared 1 MHz boost clock.  It is
 * mutated only in the two helpers that take and drop that reference, so
 * mt6320_leds::bst_users always equals the number of sinks whose enable bit
 * is set and the rail cannot be left running for a dark sink, or gated under a
 * lit one.  Using a flag rather than "is the brightness non-zero" is what keeps
 * a blink, which deliberately leaves @current_brightness alone, from losing
 * track of the reference it took.
 *
 * @boost_ref is NOT the same thing as "this sink is lit": a boost release whose
 * regmap access fails leaves @boost_ref set, because bst_users was rolled back
 * with it, so a dark sink can still hold a reference.  @dim_steady exists
 * because of that.  The dimming counter is programmed from exactly one of two
 * places - mt6320_led_set_steady(), or the blink pair mt6320_led_set_blink()
 * and mt6320_led_set_blink_clksel() - so what it holds cannot be inferred
 * from @blink_active (false for a sink that has simply never blinked) nor
 * from @boost_ref (true for a dark sink whose release failed).  This flag
 * records which of the two is actually in the register, and the first dimming
 * program to fail clears it, so mt6320_led_hw_on() re-establishes the steady
 * configuration after any error rather than skipping it.
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
 * @bst_users counts the sinks that want the 1 MHz boost clock.  It is only
 * ever mutated with @lock held, which also serialises it against the
 * register write it guards.
 */
struct mt6320_leds {
	struct device			*dev;
	struct regmap			*regmap;
	struct mutex			lock;
	struct mt6320_led		*led[MT6320_MAX_LEDS];
	/* dedicated keypad sink, if the board has one */
	struct mt6320_kpled		*kpled;
	unsigned int			bst_users;
};

/*
 * Per-channel register offsets.
 *
 * The three dimming counters are in ISINKS_CON0/1/2, the three enable bits
 * are all in ISINKS_CON3, and the mode+step pairs are in ISINKS_CON4/5/6.
 * The register is the same for every channel within each group, so these
 * are indexed by channel rather than being separate tables.
 */
static const u16 mt6320_isink_dim_reg[] = {
	MT6320_ISINKS_CON0, MT6320_ISINKS_CON1, MT6320_ISINKS_CON2,
};

static const u16 mt6320_isink_cfg_reg[] = {
	MT6320_ISINKS_CON4, MT6320_ISINKS_CON5, MT6320_ISINKS_CON6,
};

/*
 * Per-channel clock-select registers used for blink timing.  These are the
 * "breathN_trf_sel" fields the BSP uses (upmu_common.c:21866 for ch0), one per
 * channel.
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
 * The bit is programmed in TOP_CKPDN (0x0102) by read-modify-write, which is
 * what the vendor does and deliberately not through MT6320_TOP_CKPDN_SET
 * (0x0104) or MT6320_TOP_CKPDN_CLR (0x0106).  Those are write-one-to-set and
 * write-one-to-clear shadows: a bit written to 0 in the _CLR shadow is simply
 * not written, so it stays put and cannot ungate anything.  The vendor never
 * uses them for this bit; upmu_set_rg_bst_drv_1m_ck_pdn() calls
 * pmic_config_interface(TOP_CKPDN, val, PMIC_RG_BST_DRV_1M_CK_PDN_MASK,
 * PMIC_RG_BST_DRV_1M_CK_PDN_SHIFT) (upmu_common.c:1379-1386), i.e. a
 * read-modify-write on the register itself, under pmic_lock().  Its callers
 * all pass 0x0 to ungate (leds.c:298, 315, 331, 546, 589, 632, 674) and the
 * matching 0x1 gate-backs are commented out (556, 598, 641, 686).  TOP_CKPDN
 * is 0x0102 in upmu_hw.h:38, matching include/linux/mfd/mt6320/registers.h.
 *
 * Only the first and last user touch the register: the count decides when the
 * rail is actually switched, and only then is the bit changed, so calls at a
 * non-transition count cost nothing but a decrement.
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

	/* Clear the power-down bit to ungate, set it to gate. */
	ret = regmap_read(leds->regmap, MT6320_TOP_CKPDN, &val);
	if (ret)
		goto err_count;

	if (enable)
		val &= ~MT6320_BST_DRV_1M_CK_PDN;
	else
		val |= MT6320_BST_DRV_1M_CK_PDN;

	ret = regmap_write(leds->regmap, MT6320_TOP_CKPDN, val);

err_count:
	/* Undo the accounting so the rail is retried on the next transition. */
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
 * Both wrappers are the only places that mutate led->boost_ref, and both do
 * so only on the real edge, so the flag cannot drift out of step with the
 * counter.  That matters because the counter no longer follows
 * led->current_brightness: a hardware blink deliberately leaves the
 * brightness alone, so "is the brightness non-zero" would no longer answer
 * "does this sink hold a reference".
 *
 * The release is transactional in exactly the same way the take is, and for
 * the same reason: mt6320_led_bst_clk() rolls bst_users back when the regmap
 * access fails, so it leaves the counter on its pre-call value and reports the
 * error.  Clearing boost_ref regardless of that error would be a half-applied
 * release - the flag says this sink holds nothing while the controller still
 * counts it - and the next take would then add a second reference for one lit
 * sink, running bst_users permanently one high and parking the rail forever.
 * So a failed release keeps boost_ref set: the two pieces of state still agree
 * that a reference is held, and the release is retried on the next transition
 * to this sink (see mt6320_led_hw_off(), which calls this unconditionally on
 * the enable-bit path) rather than being silently forgotten.
 *
 * Taking is idempotent, so after a successful mt6320_led_take_boost() the
 * caller cannot tell whether it just acquired the reference or found one that
 * was already held - which is exactly what an error path needs to know.  The
 * take therefore reports that through @acquired, and only a reference this
 * call really acquired may be released again: dropping one that was already
 * held before the call leaves a sink whose enable bit is still set - a lit
 * LED - with the boost rail gated off underneath it, and nobody left to take
 * that reference back.
 *
 * Callers must hold leds->lock (mt6320_led_remove() being the documented
 * exception), which is also what mt6320_led_bst_clk() requires.
 */
static int mt6320_led_take_boost(struct mt6320_led *led, bool *acquired)
{
	struct mt6320_leds *leds = led->parent;
	int ret;

	/* Defined for the caller even when the take itself fails. */
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
 * Release this sink's reference on the shared boost clock.
 *
 * Mirrors mt6320_led_take_boost(): the flag moves only if the release really
 * happened.  Returning is deliberately void - every caller already has an
 * error to report or an outcome already decided, and the LED core cannot act
 * on a boost-clock release that failed (it only sees brightness_set()'s return
 * value, and the sink is dark either way at this point).  Keeping boost_ref
 * set on failure is what makes the retry possible: the state stays consistent,
 * the rail stays counted as held, and the next hw_off() drains it.
 */
/*
 * Release this sink's reference on the shared boost rail.
 *
 * Returns 0 when there is nothing to release or the release succeeded, and
 * a negative errno when the release failed and boost_ref was left set to stay
 * consistent with the bst_users that mt6320_led_bst_clk() rolled back.  The
 * caller decides whether that is worth reporting: the LED has already been
 * switched off by then, so it must not turn a successful brightness change
 * into an error.
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
 * Set the sink's current step.  This is the ONE place the current level is
 * decided; brightness is mapped onto the step exactly once, in
 * mt6320_led_hw_on(), and everything else just programs what it is given.
 *
 * The mode field travels with the step because the BSP writes both through
 * the same ISINKS_CON4/5/6 word, always with PMIC_PWM_0.
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

/*
 * Map an LED class brightness onto a current step code.  This is the ONE
 * place in the driver that does that conversion: brightness N selects step
 * N-1, following leds-mt6323.c (ISINK_CH_STEP(brightness - 1),
 * leds-mt6323.c:169).
 *
 * The clamp keeps a step inside the three-bit field no matter what it is
 * handed, so it can never land on another channel's current range.
 */
static u8 mt6320_led_brightness_step(struct mt6320_led *led,
				     enum led_brightness brightness)
{
	return min_t(unsigned int, brightness, led->cdev.max_brightness) - 1;
}

/*
 * The brightness a hardware blink is being run at, which is the brightness
 * the user last left the sink at, or LED_FULL_1 if it was dark when the blink
 * started.
 *
 * The dark case takes the bottom step code: the vendor starts a blink from
 * dark at the smallest step its sink uses, upmu_set_isinks_ch0_step(0x0)
 * (leds.c:300), commented 4 mA.  Reporting LED_ON for that is therefore the
 * level that really is programmed, and it is consistent with the steady path,
 * which uses LED_ON for step 0 too.
 *
 * Keeping the "dark means the bottom step" rule here, rather than in the blink
 * path, is what stops mt6320_led_set_step() and brightness_get() from ever
 * disagreeing about the current in the step field.
 */
static enum led_brightness
mt6320_led_blink_brightness(struct mt6320_led *led)
{
	return led->current_brightness ? led->current_brightness : LED_ON;
}

/*
 * Put the sink into a steady level rather than a blink pattern.
 *
 * This deliberately reproduces the vendor steady-state configuration rather
 * than inventing one: fsel = 11, trf_sel = 0, duty = 15, exactly as the BSP
 * programs it for every ISINK channel it drives solid (leds.c:535-536 for
 * ch0, :579-580 for ch1, :622-623 for ch2).  See MT6320_STEADY_* above for
 * why those particular values and where each comes from.
 *
 * The level itself is NOT expressed here: it lives in the current step, which
 * mt6320_led_hw_on() programs before calling this.  This function only puts
 * the dimming counter into its always-on configuration, so that the sink is
 * not mid-blink when the step is changed.
 */
static int mt6320_led_set_steady(struct mt6320_led *led)
{
	struct mt6320_leds *leds = led->parent;
	u32 dim;
	int ret;

	/*
	 * Clock select first, then the duty/fsel pair, so the counter is never
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
	/*
	 * A partially applied steady configuration is not the steady
	 * configuration, so the flag is cleared rather than left set: the next
	 * mt6320_led_hw_on() has to program the counter again from scratch.
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
 * clock-select encoding is not documented anywhere in this tree, and
 * inverting it would be guesswork.  The exact resulting frequency is not
 * stated by the BSP, so this is "the vendor's rate for this period", not a
 * verified Hz figure.  Note also that every table entry is >= 250 ms, so a
 * faster request rounds up to 250 ms - matching find_time_index_pmic(),
 * which returns the first entry that covers the request (leds.c:271-281).
 *
 * One deliberate divergence: find_time_index_pmic() clamps an over-long
 * request to the last entry (10000 ms).  This driver returns -EINVAL for
 * anything past 10 s instead, so the LED core falls back to its software
 * timer and blinks at the rate actually requested, rather than blinking at a
 * capped rate the caller did not ask for.
 *
 * Returns -EINVAL when no entry covers the requested period, which is the
 * documented way to ask the LED core to fall back to software blinking.
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

	/* First entry that covers the requested period, else give up. */
	for (i = 0; i < ARRAY_SIZE(tbl_period); i++)
		if (period_ms <= tbl_period[i])
			break;
	if (i == ARRAY_SIZE(tbl_period))
		return -EINVAL;

	/* Duty is the on-fraction of the period, scaled the way the BSP does. */
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

	/*
	 * Once a blink pattern is in the counter the steady configuration is
	 * gone, so the flag is dropped whether the write succeeded or not.  On
	 * failure nothing is known about the counter, which is exactly the
	 * state "not steady" describes.
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

	/* Same mutual exclusion with the steady configuration; see above. */
	led->dim_steady = false;

	return ret;
}

/*
 * Enable the sink's hardware enable bit.
 *
 * Split out from mt6320_led_hw_on() because the blink path shares it: both
 * paths need the sink lit, but only the brightness path may reprogram the
 * dimming counter into its steady configuration.  Calling that from here
 * would overwrite the blink pattern the caller has just programmed.
 *
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
 * Turn the sink on at the current step selected by @brightness.
 *
 * The level itself comes from mt6320_led_brightness_step(), the single place
 * in the driver that converts an LED class brightness into a current step.
 *
 * Leaving a blink: the dimming counter is put back into the steady
 * configuration here.  This is what stops a hardware blink when the user
 * writes a brightness, and it has to happen before the step is programmed and
 * before the sink is re-enabled, or the LED would resume the stale pattern.
 * The LED core relies on the brightness callback doing this - see the
 * "Deactivate blinking again when the brightness is set to LED_OFF" contract
 * on struct led_classdev (include/linux/leds.h).
 *
 * The boost clock is referenced here and released in mt6320_led_hw_off(), so
 * the two are symmetric for both the brightness and the blink path.  The
 * reference is taken on the off -> on edge only, detected with led->boost_ref
 * rather than the brightness: a blink leaves the brightness alone, so the
 * brightness no longer says whether this sink is lit.  mt6320_led_take_boost()
 * is a no-op when the reference is already held, so 10 -> 12 -> 20 -> 0 takes
 * one reference and releases it once; without the edge test the count would
 * grow with every level change and the rail would stay up forever after the
 * sink goes dark.
 *
 * @dim_steady, not @boost_ref, is what decides whether the dimming counter
 * has to be programmed again here.  A sink coming up from dark has no steady
 * configuration in the register, but neither does one whose steady program
 * failed, and neither does one that has been left holding a boost reference
 * by a failed release - all three are "boost_ref may or may not be set,
 * dim_steady is false".
 */
static int mt6320_led_hw_on(struct mt6320_led *led,
			    enum led_brightness brightness)
{
	bool took_boost = false;
	int ret;

	/*
	 * Leaving a blink, or coming up from dark, needs the dimming counter
	 * running steadily before the step changes, otherwise the sink would
	 * switch current mid-pattern.  When the sink is already lit at a
	 * steady level the counter is already in that configuration and is
	 * left alone.
	 */
	if (!led->dim_steady) {
		ret = mt6320_led_set_steady(led);
		if (ret)
			return ret;
	}

	/* The boost rail must be running before the sink is enabled. */
	ret = mt6320_led_take_boost(led, &took_boost);
	if (ret)
		return ret;

	ret = mt6320_led_set_step(led, mt6320_led_brightness_step(led, brightness));
	if (ret)
		goto err_unref;

	/*
	 * Only now, with the counter steady and the step programmed, may the
	 * sink be lit.  Blink state is dropped after the enable succeeded so a
	 * failed write leaves the previous state intact.
	 */
	ret = mt6320_led_hw_enable(led);
	if (ret)
		goto err_unref;

	led->blink_active = false;

	return 0;

err_unref:
	/*
	 * The sink was not lit by this call, so a reference it just took has to
	 * go back or the rail would stay up for a light that is still dark.
	 * A reference that was already held before this call is deliberately
	 * left alone: that sink was lit when this started, so releasing here
	 * would gate the boost rail out from under a still-enabled sink.
	 */
	if (took_boost)
		mt6320_led_drop_boost(led);

	return ret;
}

/*
 * Turn the sink off.
 *
 * Both parts of "off" are done here: the enable bit is cleared, which is what
 * physically darkens the sink, and the blink state is torn down, which is what
 * the LED core means by brightness 0 stopping hardware blinking.  Leaving a
 * blink armed here would mean a later brightness write re-enabled the sink
 * into the old pattern, since the enable bit is all the hardware needs to
 * resume it.  mt6320_led_hw_on() also re-programs the steady configuration
 * whenever it sees dim_steady clear, so neither of the two paths can leave a
 * stale pattern behind.
 *
 * The boost reference is dropped last, and only if one is actually held, so a
 * blink that was started while the sink was dark gets its reference back and
 * the shared rail is parked once the last sink goes dark.  The ordering is
 * deliberate: the reference is only given up once the enable bit has really
 * been cleared, because until then the sink may still be lit and drawing from
 * the rail.  A failed enable write therefore returns early and keeps both the
 * blink state and the reference, which is consistent rather than a leak - the
 * next write to this LED drains it.
 *
 * mt6320_led_drop_boost() is called unconditionally here rather than on a
 * brightness edge, because it is the release path that has to be able to
 * retry: if the boost release itself failed last time, boost_ref is still set
 * and this is the call that finishes the job.  A sink that never held a
 * reference returns immediately, so calling this for an already-dark LED
 * still cannot release anything it never took.
 */
static int mt6320_led_hw_off(struct mt6320_led *led)
{
	int ret;

	ret = regmap_clear_bits(led->parent->regmap, MT6320_ISINKS_EN_REG,
				MT6320_ISINK_CH_EN(led->channel));
	if (ret)
		return ret;

	/*
	 * The enable bit is clear, so whatever pattern was armed cannot run:
	 * drop the blink state here, before the sink is ever enabled again, so
	 * no later brightness write can resume it.  dim_steady goes with it: a
	 * dark sink's dimming registers are whatever the last attempt left
	 * there, so the next mt6320_led_hw_on() programs them again.
	 */
	led->blink_active = false;
	led->dim_steady = false;

	/*
	 * Drop the shared boost rail once the last sink is dark.  This is the
	 * mirror of the reference taken in mt6320_led_hw_on(), so it only
	 * happens on an on -> off transition: dropping for an already-dark
	 * sink would release a reference it never took.
	 * mt6320_led_drop_boost() checks led->boost_ref, which
	 * mt6320_led_bst_clk() also clamps at zero, so a duplicate release
	 * cannot wrap the unsigned counter.
	 *
	 * If this release fails its regmap access, mt6320_led_drop_boost()
	 * keeps boost_ref set to match the bst_users it rolled back, so the
	 * rail stays counted as held rather than the two pieces of state
	 * disagreeing.  The rail is then left on for a dark sink, which is a
	 * power cost and not a correctness problem: the state is coherent and
	 * the next brightness 0 on this sink - or mt6320_led_remove() - drains
	 * it.
	 *
	 * Report it, but only after the sink is already dark.  Failing this
	 * write did not stop the LED from turning off, so the brightness
	 * change itself succeeded and the LED core must not see an error for
	 * it; what failed is releasing a shared rail, which is worth logging
	 * rather than swallowing because otherwise it is invisible until
	 * somebody notices the boost clock never parked.
	 */
	if (!mt6320_led_drop_boost(led) && led->boost_ref)
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
 * Both delays being zero is the LED core asking for "the blink that is
 * already programmed" (include/linux/leds.h:145-147), not for permission to
 * impose a timing of the driver's choosing.  The last programmed timings are
 * cached in the LED and reprogrammed as they are, so the LED keeps blinking
 * exactly as the user asked it to.  The cache starts out holding the DT
 * "delay-on"/"delay-off" values, which is also what an LED with no default
 * of its own falls back to.
 *
 * The programming order reproduces the BSP's mt_led_blink_pmic() for ISINK0
 * (leds.c:296-313), which is the whole point of starting a blink in this
 * driver rather than just enabling a sink:
 *
 *	leds.c:298  upmu_set_rg_bst_drv_1m_ck_pdn(0x0)   boost clock ungated
 *	leds.c:299  upmu_set_isinks_ch0_mode(PMIC_PWM_0) dimming source
 *	leds.c:300  upmu_set_isinks_ch0_step(0x0)        current step
 *	leds.c:301  upmu_set_isink_dim0_duty(duty)        on-fraction
 *	leds.c:302  upmu_set_isink_dim0_fsel(fsel)        frequency trim
 *	leds.c:308  upmu_set_isinks_breath0_trf_sel(0x0) blink clock select
 *	leds.c:312  upmu_set_isinks_ch0_en(0x01)         sink enabled
 *
 * ISINK1 (leds.c:314-329) and ISINK2 (leds.c:330-344) are the same sequence
 * on their own channel, so the three collapse into the per-channel index this
 * driver already has.  Note that the step is programmed in the middle of it:
 * entering a blink from dark without writing the step is exactly what leaves
 * the sink running at whatever reset value the field happened to hold.  The
 * boost rail goes up first and the sink is enabled last, so the pattern is
 * never visible at a stale step, on a gated rail, or with a stale divider.
 *
 * led->current_brightness is deliberately left alone.  Blinking is not a
 * brightness the user asked for, so claiming one would make brightness_get()
 * report a level that is not programmed; led->blink_active records the blink
 * instead, and mt6320_led_hw_on() unwinds it on the next brightness write.
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

	/* Nothing cached and nothing to fall back on: let the core decide. */
	if (!*delay_on || !*delay_off)
		return -EINVAL;

	on_ms = *delay_on;
	period_ms = *delay_on + *delay_off;

	/*
	 * Everything below touches the same per-channel registers that
	 * brightness_set does, so the whole sequence is done under the lock
	 * rather than just the enable - otherwise a concurrent brightness
	 * change can interleave with the dimming setup.
	 */
	mutex_lock(&led->parent->lock);

	/*
	 * Resolve the timings before touching hardware, so a request the counter
	 * cannot express is rejected without having un-gated the boost rail or
	 * enabled anything.
	 */
	ret = mt6320_led_blink_timings(led, on_ms, period_ms, &duty, &fsel,
					&clksel);
	if (ret)
		goto out_unlock;

	/* leds.c:298 - the boost rail must be up before the sink is lit. */
	ret = mt6320_led_take_boost(led, &took_boost);
	if (ret)
		goto out_unlock;

	/*
	 * leds.c:299-300 - dimming source and current step, together in the
	 * ISINKS_CON4/5/6 word as the BSP writes them.
	 * mt6320_led_set_step() writes both the step and the PMIC_PWM_0 mode,
	 * so this single call covers both of the BSP's lines.  A sink that was
	 * already lit keeps the current its brightness selected, which is what
	 * blinking from an existing level should do; a dark sink takes the
	 * bottom step code, as the BSP does.  Writing it before the pattern is
	 * armed also means the very first flash is at a configured current
	 * rather than at whatever the reset value of the field was.
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

	/*
	 * leds.c:312 - enable the sink last, deliberately not through
	 * mt6320_led_hw_on(): that would put the dimming counter back into the
	 * steady configuration and wipe out the pattern programmed above.
	 */
	ret = mt6320_led_hw_enable(led);
	if (ret)
		goto err_unref;

	/*
	 * Only refresh the cache once the pattern really is programmed.  The
	 * blink is recorded in its own flag, not in current_brightness.
	 */
	led->blink_on = on_ms;
	led->blink_off = *delay_off;
	led->blink_active = true;

	mutex_unlock(&led->parent->lock);

	return 0;

err_unref:
	/*
	 * The sink was never lit by this call, so a reference taken a moment
	 * ago has to go back or the rail would stay up for a dark LED.  One
	 * held before this call is deliberately left alone: that sink was
	 * already enabled when this started, and the pattern above never
	 * cleared its enable bit, so releasing here would gate the boost rail
	 * out from under a still-lit LED.
	 */
	if (took_boost)
		mt6320_led_drop_boost(led);

out_unlock:
	mutex_unlock(&led->parent->lock);

	return ret;
}

/*
 * Keypad backlight.
 *
 * This is a fourth, independent sink on KPLED_CON0 rather than one of the
 * three ISINK channels, so it gets its own class device and a pair of simple
 * on/off helpers rather than sharing the channel machinery above.
 */
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
		/*
		 * Duty 9 is what the BSP programs for this sink
		 * (upmu_set_kpled_dim_duty(0x9), leds.c:506-507).
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
		struct mt6320_kpled *kpled;
		u32 reg, num_steps, delay_on = 0, delay_off = 0;

		/*
		 * A "keypad" child is the dedicated KPLED sink and does not
		 * consume an ISINK channel.
		 */
		if (of_property_read_bool(child, "mediatek,is-kpled")) {
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

		/*
		 * "mediatek,num-steps" says how many current step codes this
		 * sink should expose, which is also its max_brightness.
		 * It sizes the brightness range - brightness N selects step
		 * N-1 - and is the only knob for the current level; there is
		 * no separate per-LED current property competing with it.
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

		/*
		 * Seed the blink cache with the timings this LED should use
		 * when it has never been blinked explicitly.  These are only
		 * defaults: a later blink_set() with real timings replaces
		 * them, and a blink_set(0, 0) re-applies whatever is cached at
		 * that moment.  A sink that asks for neither keeps an empty
		 * cache, and mt6320_led_hw_blink_set() then declines to guess,
		 * leaving the LED core to pick its own fallback.
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

		/*
		 * Honour the DT default state before the class device starts
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

	if (leds->kpled)
		mt6320_kpled_set_brightness(&leds->kpled->cdev, LED_OFF);

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
