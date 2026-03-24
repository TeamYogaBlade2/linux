// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/arm/mach-mediatek/platsmp.c
 *
 * Copyright (c) 2014 Mediatek Inc.
 * Author: Shunli Wang <shunli.wang@mediatek.com>
 *         Yingjoe Chen <yingjoe.chen@mediatek.com>
 */
#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/delay.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <linux/threads.h>
#include "../drivers/pmdomain/mediatek/mtk-pm-domains.h"

#define MTK_MAX_CPU		8
#define MTK_SMP_REG_SIZE	0x1000

#ifdef CONFIG_HOTPLUG_CPU
#define SPM_L1_PDN(n)		(0x25c + (8 * n))

#define SPM_SLEEP_TIMER_STA	0x0720
#define CPU_SLEEP(n)		BIT(15 + n)

#define L1RSTDISABLE1		BIT(1)

#define BOOT_SLAVE_CONFIG	0x0000
#define BOOT_SLAVE_ADDR		0x0004
#define BOOT_SLAVE_KEY		0x13800000

enum mtk_cpu_target_state {
	POWER_DOWN,
	POWER_UP,
};

struct mtk_hotplug_info {
	const char *const spm_compat;
	const char *const mcusys_compat;

	unsigned int spm_pwr_con[MTK_MAX_CPU - 1];
	unsigned int spm_pwr_status_bits[MTK_MAX_CPU - 1];
	unsigned int spm_l1_pdn_bits[MTK_MAX_CPU - 1];
	unsigned int spm_l1_pdn_ack_bits[MTK_MAX_CPU - 1];
};

static const struct mtk_hotplug_info mtk_mt6572_hotplug = {
	.spm_compat = "mediatek,mt6572-scpsys",
	.mcusys_compat = "mediatek,mt6572-mcusys",
	.spm_pwr_con = { 0x218 },
	.spm_pwr_status_bits = { BIT(11) },
	.spm_l1_pdn_bits = { BIT(0) },
	.spm_l1_pdn_ack_bits = { BIT(8) },
};
#endif

struct mtk_smp_boot_info {
	unsigned long smp_base;
	unsigned int jump_reg;
	unsigned int core_keys[MTK_MAX_CPU - 1];
	unsigned int core_regs[MTK_MAX_CPU - 1];
#ifdef CONFIG_HOTPLUG_CPU
	const struct mtk_hotplug_info *hotplug;
#endif
};

static const struct mtk_smp_boot_info mtk_mt8135_tz_boot = {
	0x80002000, 0x3fc,
	{ 0x534c4131, 0x4c415332, 0x41534c33 },
	{ 0x3f8, 0x3f8, 0x3f8 },
};

static const struct mtk_smp_boot_info mtk_mt6572_boot = {
	0x10001400, 0x08,
	{ 0x534c4131 },
	{ 0x0c },
#ifdef CONFIG_HOTPLUG_CPU
	&mtk_mt6572_hotplug,
#endif
};

static const struct mtk_smp_boot_info mtk_mt6589_boot = {
	0x10002000, 0x34,
	{ 0x534c4131, 0x4c415332, 0x41534c33 },
	{ 0x38, 0x3c, 0x40 },
};

static const struct mtk_smp_boot_info mtk_mt7623_boot = {
	0x10202000, 0x34,
	{ 0x534c4131, 0x4c415332, 0x41534c33 },
	{ 0x38, 0x3c, 0x40 },
};

static const struct of_device_id mtk_tz_smp_boot_infos[] __initconst = {
	{ .compatible   = "mediatek,mt8135", .data = &mtk_mt8135_tz_boot },
	{ .compatible   = "mediatek,mt8127", .data = &mtk_mt8135_tz_boot },
	{ .compatible   = "mediatek,mt2701", .data = &mtk_mt8135_tz_boot },
	{},
};

static const struct of_device_id mtk_smp_boot_infos[] __initconst = {
	{ .compatible   = "mediatek,mt6572", .data = &mtk_mt6572_boot },
	{ .compatible   = "mediatek,mt6582", .data = &mtk_mt7623_boot },
	{ .compatible   = "mediatek,mt6589", .data = &mtk_mt6589_boot },
	{ .compatible   = "mediatek,mt7623", .data = &mtk_mt7623_boot },
	{ .compatible   = "mediatek,mt7629", .data = &mtk_mt7623_boot },
	{},
};

static void __iomem *mtk_smp_base;
static const struct mtk_smp_boot_info *mtk_smp_info;

#ifdef CONFIG_HOTPLUG_CPU
static void __iomem *spm_base;
static void __iomem *mcusys_base;

static void spm_cpu_wait_busy(enum mtk_cpu_target_state state, unsigned int cpu)
{
	u32 val, mask = mtk_smp_info->hotplug->spm_pwr_status_bits[cpu - 1];
	do {
		val = readl(spm_base + SPM_PWR_STATUS) & mask;
		val |= readl(spm_base + SPM_PWR_STATUS_2ND) & mask;
	} while (state == POWER_DOWN ? !!val : !val);
}

static void spm_l1_wait_busy(enum mtk_cpu_target_state state, unsigned int cpu)
{
	u32 val;
	do {
		val = readl(spm_base + SPM_L1_PDN(cpu)) &
		      mtk_smp_info->hotplug->spm_l1_pdn_ack_bits[cpu - 1];
	} while (state == POWER_DOWN ? !val : !!val);
}

static void spm_ctrl_cpu(enum mtk_cpu_target_state state, unsigned int cpu)
{
	u32 val, pwr_con = mtk_smp_info->hotplug->spm_pwr_con[cpu - 1],
		 l1_pdn_bits = mtk_smp_info->hotplug->spm_l1_pdn_bits[cpu - 1];

	switch (state) {
	case POWER_DOWN:
		while (!(readl(spm_base + SPM_SLEEP_TIMER_STA) &
			 CPU_SLEEP(cpu)))
			;

		val = readl(spm_base + pwr_con);
		val |= PWR_CLK_DIS_BIT | PWR_SRAM_CLKISO_BIT | PWR_ISO_BIT;
		writel(val, spm_base + pwr_con);
		val = readl(spm_base + pwr_con);
		val &= ~(PWR_SRAM_ISOINT_B_BIT | PWR_RST_B_BIT);
		writel(val, spm_base + pwr_con);

		val = readl(spm_base + SPM_L1_PDN(cpu));
		val |= l1_pdn_bits;
		writel(val, spm_base + SPM_L1_PDN(cpu));
		spm_l1_wait_busy(state, cpu);

		val = readl(spm_base + pwr_con);
		val &= ~PWR_ON_BIT;
		writel(val, spm_base + pwr_con);
		udelay(1);

		val = readl(spm_base + pwr_con);
		val &= ~PWR_ON_2ND_BIT;
		writel(val, spm_base + pwr_con);
		udelay(1);

		spm_cpu_wait_busy(state, cpu);

		break;
	case POWER_UP:
		val = readl(spm_base + pwr_con);
		val |= PWR_ON_BIT;
		writel(val, spm_base + pwr_con);
		udelay(1);

		val = readl(spm_base + pwr_con);
		val |= PWR_ON_2ND_BIT;
		writel(val, spm_base + pwr_con);
		udelay(3);

		spm_cpu_wait_busy(state, cpu);

		val = readl(spm_base + SPM_L1_PDN(cpu));
		val &= ~l1_pdn_bits;
		writel(val, spm_base + SPM_L1_PDN(cpu));
		udelay(1);
		spm_l1_wait_busy(state, cpu);

		val = readl(spm_base + pwr_con);
		val |= PWR_SRAM_ISOINT_B_BIT;
		writel(val, spm_base + pwr_con);

		val = readl(spm_base + pwr_con);
		val &= ~(PWR_CLK_DIS_BIT | PWR_ISO_BIT);
		writel(val, spm_base + pwr_con);

		val = readl(spm_base + pwr_con);
		val |= PWR_CLK_DIS_BIT;
		writel(val, spm_base + pwr_con);

		val = readl(spm_base + pwr_con);
		val &= ~PWR_SRAM_CLKISO_BIT;
		writel(val, spm_base + pwr_con);

		val = readl(mcusys_base);
		writel(val | L1RSTDISABLE1, mcusys_base);
		dsb();

		val = readl(spm_base + pwr_con);
		val &= ~PWR_CLK_DIS_BIT;
		writel(val, spm_base + pwr_con);
		dsb();

		val = readl(spm_base + pwr_con);
		val |= PWR_RST_B_BIT;
		writel(val, spm_base + pwr_con);

		break;
	}
}

static bool mtk_hotplug_is_available(void)
{
	return spm_base && mcusys_base;
}

static bool mtk_hotplug_is_off(unsigned int cpu)
{
	u32 val = readl_relaxed(spm_base + SPM_PWR_STATUS);
	return !(val & mtk_smp_info->hotplug->spm_pwr_status_bits[cpu - 1]);
}
#endif

static int mtk_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	u32 val;

	if (!mtk_smp_base)
		return -EINVAL;

	if (!mtk_smp_info->core_keys[cpu - 1])
		return -EINVAL;

	if (mtk_hotplug_is_available() && mtk_hotplug_is_off(cpu)) {
		val = readl(mcusys_base);
		writel(val & ~L1RSTDISABLE1, mcusys_base);

		writel(BOOT_SLAVE_KEY | 1, mtk_smp_base + BOOT_SLAVE_CONFIG);
		writel(__pa_symbol(secondary_startup_arm),
		       mtk_smp_base + BOOT_SLAVE_ADDR);

		wmb();
		spm_ctrl_cpu(POWER_UP, cpu);
		dsb_sev();
	} else
		writel(mtk_smp_info->core_keys[cpu - 1],
		       mtk_smp_base + mtk_smp_info->core_regs[cpu - 1]);

	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	return 0;
}

static void __init __mtk_smp_prepare_cpus(unsigned int max_cpus, int trustzone)
{
	int i, num;
	const struct of_device_id *infos;

	if (trustzone) {
		num = ARRAY_SIZE(mtk_tz_smp_boot_infos);
		infos = mtk_tz_smp_boot_infos;
	} else {
		num = ARRAY_SIZE(mtk_smp_boot_infos);
		infos = mtk_smp_boot_infos;
	}

	/* Find smp boot info for this SoC */
	for (i = 0; i < num; i++) {
		if (of_machine_is_compatible(infos[i].compatible)) {
			mtk_smp_info = infos[i].data;
			break;
		}
	}

	if (!mtk_smp_info) {
		pr_err("%s: Device is not supported\n", __func__);
		return;
	}

	if (trustzone) {
		/* smp_base(trustzone-bootinfo) is reserved by device tree */
		mtk_smp_base = phys_to_virt(mtk_smp_info->smp_base);
	} else {
		mtk_smp_base = ioremap(mtk_smp_info->smp_base, MTK_SMP_REG_SIZE);
		if (!mtk_smp_base) {
			pr_err("%s: Can't remap %lx\n", __func__,
				mtk_smp_info->smp_base);
			return;
		}
	}

	/*
	 * write the address of slave startup address into the system-wide
	 * jump register
	 */
	writel_relaxed(__pa_symbol(secondary_startup_arm),
			mtk_smp_base + mtk_smp_info->jump_reg);
}

#ifdef CONFIG_HOTPLUG_CPU
static void __mtk_hotplug_prepare(void)
{
	struct device_node *np;

	if (!mtk_smp_info->hotplug || !mtk_smp_info->hotplug->spm_compat ||
	    !mtk_smp_info->hotplug->mcusys_compat) {
		pr_err("%s: hotplug data is incomplete, disabling hotplug\n",
		       __func__);
		return;
	}

	np = of_find_compatible_node(NULL, NULL,
				     mtk_smp_info->hotplug->spm_compat);
	if (!np) {
		pr_err("%s: SPM node not found, hotplug won't be available\n",
		       __func__);
		return;
	}

	spm_base = of_iomap(np, 0);
	of_node_put(np);

	np = of_find_compatible_node(NULL, NULL,
				     mtk_smp_info->hotplug->mcusys_compat);
	if (!np) {
		pr_err("%s: MCUSYS node not found, hotplug won't be available\n",
		       __func__);
		iounmap(spm_base);
		return;
	}

	mcusys_base = of_iomap(np, 0);
	of_node_put(np);
}
#endif

static void __init mtk_tz_smp_prepare_cpus(unsigned int max_cpus)
{
	__mtk_smp_prepare_cpus(max_cpus, 1);
}

static void __init mtk_smp_prepare_cpus(unsigned int max_cpus)
{
	__mtk_smp_prepare_cpus(max_cpus, 0);
#ifdef CONFIG_HOTPLUG_CPU
	__mtk_hotplug_prepare();
#endif
}

#ifdef CONFIG_HOTPLUG_CPU
static bool mtk_cpu_can_disable(unsigned int cpu)
{
	return mtk_hotplug_is_available() && cpu != 0;
}

static int mtk_cpu_disable(unsigned int cpu)
{
	if (mtk_cpu_can_disable(cpu))
		return 0;

	return -EPERM;
}

static void mtk_cpu_die(unsigned int cpu)
{
	v7_exit_coherency_flush(louis);

	while (1)
		wfi();
}
static int mtk_cpu_kill(unsigned int cpu)
{
	spm_ctrl_cpu(POWER_DOWN, cpu);
	return 1;
}
#endif

static const struct smp_operations mt81xx_tz_smp_ops __initconst = {
	.smp_prepare_cpus = mtk_tz_smp_prepare_cpus,
	.smp_boot_secondary = mtk_boot_secondary,
};
CPU_METHOD_OF_DECLARE(mt81xx_tz_smp, "mediatek,mt81xx-tz-smp", &mt81xx_tz_smp_ops);

static const struct smp_operations mt6589_smp_ops __initconst = {
	.smp_prepare_cpus = mtk_smp_prepare_cpus,
	.smp_boot_secondary = mtk_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_can_disable = mtk_cpu_can_disable,
	.cpu_disable	= mtk_cpu_disable,
	.cpu_die	= mtk_cpu_die,
	.cpu_kill	= mtk_cpu_kill,
#endif
};
CPU_METHOD_OF_DECLARE(mt6589_smp, "mediatek,mt6589-smp", &mt6589_smp_ops);
