// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 MediaTek Inc.
 *
 * MT6589 cpuidle / DCM (Dynamic Clock Management) support.
 *
 * The MT6589 Cortex-A7 complex and the top AXI fabric cannot simply be
 * clock-gated while the rest of the system keeps running, so instead they use
 * DCM: the clock keeps running but is divided down while the consumer is
 * idle, and returns to full speed as soon as a bus transaction or a core
 * wake-up shows up. That makes DCM a *policy* decision rather than a plain
 * register write - which clocks may be slowed, in what order, and what has to
 * be put back on the way out.
 *
 * This driver supplies the MediaTek half of that policy for the generic ARM
 * cpuidle driver. It does not register a cpuidle driver of its own: it
 * registers a struct cpuidle_ops for the "mediatek,mt6589-smp" enable-method,
 * which is what drivers/cpuidle/cpuidle-arm.c reaches through
 * arm_cpuidle_suspend() for every DT-described idle state. The DT nodes own
 * the state names and the latency/residency numbers; this file owns the
 * sequence that has to happen around the WFI.
 *
 * Only the state that can be justified from the MT6589 datasheet is
 * implemented. See the comments on each register below for the section and
 * page it comes from.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpuidle.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <asm/barrier.h>
#include <asm/cpuidle.h>

#include "mtk-dcm.h"

static struct regmap *topckgen_regmap;
static struct regmap *infracfg_regmap;

/*
 * Top AXI fabric ("hf_faxi_ck") DCM, enabled around deep idle entries.
 *
 * The datasheet says this can be enabled from boot without a performance
 * concern, but it also warns that modules using AXI as their engine clock are
 * affected, and relies on those modules driving their idle signals into the
 * fabric so that BUS_DCM is not entered while any of them is busy. Which
 * modules do that cannot be established from the datasheet, and getting it
 * wrong shows up as random hangs rather than as a clean failure. So unlike
 * the Cortex-A7 DCM below, this stays a per-idle-entry enable that is undone
 * on the way out, and it is switchable at runtime.
 */
static bool bus_dcm_enabled = true;
module_param(bus_dcm_enabled, bool, 0644);
MODULE_PARM_DESC(bus_dcm_enabled,
		 "Enable top AXI fabric (hf_faxi_ck) DCM around idle entries");

/*
 * Cortex-A7 DCM (CPU_DCM). Datasheet section 16.2.3, page 810:
 *
 *   "CPU_DCM means if all the cores enter WFI, ARM_CK will be slowed down
 *    to 26MHz to save CA7's WFI power."
 *
 * The trigger is the condition that every core is in WFI, which is exactly
 * the state the cpuidle framework is about to put this core into, and which
 * the hardware re-evaluates on its own. Nothing needs to be undone on exit,
 * and leaving the bit set costs nothing when the cores are running, so it is
 * enabled once at probe and never touched from the idle path. That also
 * removes any cross-CPU race on a global register: no per-CPU teardown can
 * clear the bit while a sibling core is still in WFI.
 *
 * The three writes follow the order the vendor driver uses (mt_dcm.c, the
 * CPU_DCM branch of dcm_enable()): pick the frequency first, then the
 * de-bounce window, then the enable bit last, so the hardware never sees DCM
 * enabled against an unconfigured divider.
 */
static void mtk_dcm_cpu_enable(void)
{
	/*
	 * TOP_CA7DCMFSEL[26:24] dcm_arm_divsel. The datasheet names the field
	 * "Clock divider setting in ARM DCM mode" but gives no encoding table
	 * for it, and it resets to 0. The vendor driver writes 7 on this SoC,
	 * so the reset value is evidently not what shipping hardware wants;
	 * 7 is copied from there rather than invented.
	 */
	regmap_update_bits(infracfg_regmap, INFRA_TOP_CA7DCMFSEL,
			   TOP_CA7DCMFSEL_DIVSEL_MASK, DCM_ARM_DIVSEL_7);

	/*
	 * TOP_DCMDBC[6:0] topckgen_dcm_dbc_cnt, a de-bounce counter for the
	 * core clock. The vendor driver writes 1 here, annotated "force to
	 * 26M" - i.e. take the core clock down as soon as the last core is
	 * idle rather than waiting out a long debounce window.
	 */
	regmap_write(infracfg_regmap, INFRA_TOP_DCMDBC, TOP_DCMDBC_CNT_1);

	/*
	 * TOP_DCMCTL[1] arm_dcm_wfi_enable is the only bit set here. The
	 * vendor driver also sets bit 2, arm_dcm_wfe_enable, but the ARM
	 * idle path uses WFI only, so bit 2 is deliberately left clear: the
	 * datasheet gives no way to confirm that dividing ARM_CK while cores
	 * are parked in WFE is safe for every WFE user in the tree.
	 */
	regmap_set_bits(infracfg_regmap, INFRA_TOP_DCMCTL,
			TOP_DCMCTL_ARM_DCM_WFI_ENABLE);
}

/*
 * Bus DCM, around one idle entry.
 *
 * Datasheet section 16.2.3, pages 810-812, describes BUS_DCM as the top AXI
 * clock DCM, which slows the clock when there is no bus transaction, and notes
 * that the SoC ANDs the idle signals of the modules that use AXI as their
 * engine clock into the fabric so that BUS_DCM is not entered while any of
 * them is busy.
 *
 * The register is global but this is only a power optimisation, so the race
 * between two CPUs going idle at once is resolved conservatively: whichever
 * CPU finds the bit already set does not clear it on the way out, and the
 * worst case is that the last CPU out of idle leaves DCM off. That costs
 * power, never correctness.
 */
static bool mtk_dcm_bus_enable(void)
{
	u32 val;
	int ret;

	/*
	 * arm_idle_init() and mtk_dcm_probe() are both device_initcall() and
	 * may run in either order, so the maps may still be unset here. That
	 * only costs us the DCM sequence, never correctness: state 0 is a
	 * plain WFI regardless, and this function simply does nothing.
	 */
	if (!bus_dcm_enabled || IS_ERR_OR_NULL(topckgen_regmap))
		return false;

	ret = regmap_read(topckgen_regmap, TOPCK_DCM_CFG, &val);
	if (ret)
		return false;

	/* Another CPU is already holding the fabric in DCM; leave it to them. */
	if (val & DCM_CFG_FAXI_ENABLE)
		return false;

	/*
	 * TOPCK_DCM_CFG[4:0] dcm_full_fsel, where 01xxx selects hf_faxi_ck/2.
	 * The vendor driver writes 0xf here (bus_dcm_enable()), i.e. half
	 * speed, which is where the saving actually comes from - the enable
	 * bit on its own would just enable DCM at full speed.
	 */
	ret = regmap_update_bits(topckgen_regmap, TOPCK_DCM_CFG,
				 DCM_CFG_FAXI_ENABLE | DCM_CFG_FAXI_FSEL_MASK,
				 DCM_CFG_FAXI_ENABLE | DCM_CFG_FAXI_FSEL_DIV2);
	if (ret)
		return false;

	return true;
}

static void mtk_dcm_bus_disable(bool owned)
{
	if (owned)
		regmap_clear_bits(topckgen_regmap, TOPCK_DCM_CFG, DCM_CFG_FAXI_ENABLE);
}

/*
 * The deep idle body.
 *
 * Interrupts are already disabled here (cpuidle enters its state with IRQs
 * off), so there is no window in which an IRQ handler could run with the
 * fabric clock divided down. dsb() before the WFI makes the DCM register
 * writes visible before the core can go to sleep, exactly as the vendor
 * sequence in mt_idle.c does around its WFI.
 */
static int mtk_dcm_enter(void)
{
	bool bus_owned;

	bus_owned = mtk_dcm_bus_enable();

	dsb();
	wfi();

	mtk_dcm_bus_disable(bus_owned);

	return 0;
}

/*
 * struct cpuidle_ops::suspend, reached from arm_cpuidle_suspend() with the
 * index of whichever DT-described state is being entered.
 *
 * Index 0 is the plain WFI state that cpuidle-arm.c registers itself, and
 * __CPU_PM_CPU_IDLE_ENTER() short-circuits it to a bare cpu_do_idle() before
 * ever calling this, so every index that does arrive here is a state the
 * device tree asked for. There is only one such state today and it gets the
 * full DCM treatment; the index is deliberately unused rather than switched on,
 * so that adding a deeper state later forces a decision about what that state
 * may assume instead of silently inheriting today's sequence.
 */
static int mtk_dcm_suspend(unsigned long index)
{
	return mtk_dcm_enter();
}

static int mtk_dcm_init(struct device_node *cpu_node, int cpu)
{
	return 0;
}

static const struct cpuidle_ops mtk_dcm_ops = {
	.init = mtk_dcm_init,
	.suspend = mtk_dcm_suspend,
};

CPUIDLE_METHOD_OF_DECLARE(mtk_dcm, "mediatek,mt6589-smp", &mtk_dcm_ops);

static int __init mtk_dcm_probe(void)
{
	topckgen_regmap = syscon_regmap_lookup_by_compatible("mediatek,mt6589-topckgen");
	if (IS_ERR(topckgen_regmap))
		return PTR_ERR(topckgen_regmap);

	infracfg_regmap = syscon_regmap_lookup_by_compatible("mediatek,mt6589-infracfg");
	if (IS_ERR(infracfg_regmap))
		return PTR_ERR(infracfg_regmap);

	mtk_dcm_cpu_enable();

	pr_info("Cortex-A7 WFI DCM enabled\n");

	return 0;
}
device_initcall(mtk_dcm_probe);
