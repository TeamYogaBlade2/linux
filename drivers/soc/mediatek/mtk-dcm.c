// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * MT6589 Dynamic Clock Management.
 *
 * CPU_DCM is triggered by hardware when all Cortex-A7 cores enter WFI.
 * BUS_DCM is qualified by the bus and engine idle conditions.
 *
 * The MT6589 datasheet (section 16.2.3) explicitly permits enabling
 * BUS_DCM at boot and leaving it enabled. Configure both DCM modes
 * once during initialization instead of manipulating shared clock
 * registers from a per-CPU idle callback.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/string.h>

#include "mtk-dcm.h"

#define MTK_SMP_ENABLE_METHOD	"mediatek,mt6589-smp"

/*
 * Cortex-A7 CPU_DCM.
 *
 * The vendor driver selects divider 7 and debounce count 1.
 * Enable WFI-triggered DCM only: the datasheet documents the all-core
 * WFI condition, while enabling WFE-triggered DCM is not needed here.
 *
 * Keep the configuration order: divider, debounce, enable.
 */
static int mtk_dcm_enable_cpu(struct regmap *infracfg)
{
	int ret;

	ret = regmap_update_bits(infracfg, INFRA_TOP_CA7DCMFSEL,
				 TOP_CA7DCMFSEL_DIVSEL_MASK,
				 DCM_ARM_DIVSEL_7);
	if (ret)
		return ret;

	ret = regmap_update_bits(infracfg, INFRA_TOP_DCMDBC,
				 GENMASK(6, 0), TOP_DCMDBC_CNT_1);
	if (ret)
		return ret;

	/*
	 * Explicitly clear WFE enable as well as setting WFI enable.
	 * This avoids inheriting an unexpected WFE configuration.
	 */
	return regmap_update_bits(infracfg, INFRA_TOP_DCMCTL,
				  TOP_DCMCTL_ARM_DCM_WFI_ENABLE |
				  TOP_DCMCTL_ARM_DCM_WFE_ENABLE,
				  TOP_DCMCTL_ARM_DCM_WFI_ENABLE);
}

/*
 * Top AXI fabric BUS_DCM.
 *
 * Select hf_faxi_ck / 2 (the value used by the vendor driver) and enable
 * DCM in one update. The hardware decides when the fabric is idle enough
 * to apply the reduced clock.
 *
 * TOPCK_DCM_CFG is offset 0x004 in the TOPCKGEN syscon window.
 */
static int mtk_dcm_enable_bus(struct regmap *topckgen)
{
	return regmap_update_bits(topckgen, TOPCK_DCM_CFG,
				  DCM_CFG_FAXI_ENABLE |
				  DCM_CFG_FAXI_FSEL_MASK,
				  DCM_CFG_FAXI_ENABLE |
				  DCM_CFG_FAXI_FSEL_DIV2);
}

static int __init mtk_dcm_probe(void)
{
	struct device_node *cpus;
	struct regmap *infracfg;
	struct regmap *topckgen;
	const char *enable_method = NULL;
	int ret;

	/*
	 * MT6589 declares its SMP enable method on /cpus. Do not depend on
	 * individual CPU nodes carrying a duplicate enable-method property.
	 */
	cpus = of_find_node_by_path("/cpus");
	if (!cpus)
		return 0;

	ret = of_property_read_string(cpus, "enable-method",
				      &enable_method);
	of_node_put(cpus);

	if (ret || strcmp(enable_method, MTK_SMP_ENABLE_METHOD))
		return 0;

	/*
	 * These are optional power optimizations. Failure to configure one
	 * DCM mode must not prevent the other mode from being configured
	 * or affect system boot.
	 */
	infracfg = syscon_regmap_lookup_by_compatible(
			"mediatek,mt6589-infracfg");
	if (IS_ERR(infracfg)) {
		pr_warn("cannot find INFRACFG syscon: %ld\n",
			PTR_ERR(infracfg));
	} else {
		ret = mtk_dcm_enable_cpu(infracfg);
		if (ret)
			pr_warn("failed to enable CPU_DCM: %d\n", ret);
		else
			pr_info("Cortex-A7 WFI DCM enabled\n");
	}

	topckgen = syscon_regmap_lookup_by_compatible(
			"mediatek,mt6589-topckgen");
	if (IS_ERR(topckgen)) {
		pr_warn("cannot find TOPCKGEN syscon: %ld\n",
			PTR_ERR(topckgen));
	} else {
		ret = mtk_dcm_enable_bus(topckgen);
		if (ret)
			pr_warn("failed to enable BUS_DCM: %d\n", ret);
		else
			pr_info("AXI BUS DCM enabled (hf_faxi_ck / 2 when idle)\n");
	}

	return 0;
}
device_initcall(mtk_dcm_probe);

MODULE_DESCRIPTION("MediaTek MT6589 Dynamic Clock Management");
MODULE_LICENSE("GPL");
