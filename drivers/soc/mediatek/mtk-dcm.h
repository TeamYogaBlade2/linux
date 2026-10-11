/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2025 MediaTek Inc.
 *
 * Register definitions for the MediaTek DCM (Dynamic Clock Management)
 * registers found on the MT6589 "top clock generator" (TOPCKGEN) and
 * "infrastructure configuration" (INFRACFG) blocks.
 *
 * These are offsets inside the two existing syscon windows already
 * described by the "mediatek,mt6589-topckgen" and "mediatek,mt6589-infracfg"
 * nodes, so no new DT node and no new ioremap() is needed.
 */
#ifndef _MTK_DCM_H
#define _MTK_DCM_H

/* TOPCKGEN, relative to the topckgen syscon node base (0x1000_0100). */
#define TOPCK_DCM_CFG			0x004

/*
 * INFRACFG, relative to the infracfg syscon node base (0x1000_1000).
 *
 * Note that despite the "TOPCKGEN ..." / "TOPCKGEN Cortex-A7 ..." prefixes
 * the datasheet gives for these three registers, they are mapped in the
 * INFRACFG window, not the TOPCKGEN one. The datasheet (section 8.2,
 * "Digital Clock Manager", page 433) introduces them as controlling the
 * INFRASYS clock tree, and they sit in the infracfg register map.
 */
#define INFRA_TOP_DCMCTL		0x010
#define INFRA_TOP_DCMDBC		0x014
#define INFRA_TOP_CA7DCMFSEL		0x018

#define INFRA_DCMCTL			0x050
#define INFRA_DCMDBC			0x054
#define INFRA_DCMFSEL			0x058

/* TOP_DCMCTL bits. */
#define TOP_DCMCTL_INFRA_DCM_ENABLE	BIT(0)
#define TOP_DCMCTL_ARM_DCM_WFI_ENABLE	BIT(1)
#define TOP_DCMCTL_ARM_DCM_WFE_ENABLE	BIT(2)

/* TOP_CA7DCMFSEL[26:24], the Cortex-A7 clock divider used in ARM DCM mode. */
#define TOP_CA7DCMFSEL_DIVSEL_MASK	0x07000000
#define DCM_ARM_DIVSEL_7		0x07000000

/* TOP_DCMDBC[6:0], the Cortex-A7 clock de-bounce counter. */
#define TOP_DCMDBC_CNT_1		0x01

/* INFRA_DCMCTL bits. */
#define INFRA_DCMCTL_FAXI_DCM_ENABLE	BIT(0)
#define INFRA_DCMCTL_FMEM_DCM_ENABLE	BIT(1)
#define INFRA_DCMCTL_AXI_CLOCK_GATED	BIT(8)

/*
 * Bus (hf_faxi_ck) DCM, in DCM_CFG. This is the top AXI fabric clock, not the
 * Cortex-A7 complex clock.
 */
#define DCM_CFG_FAXI_ENABLE		BIT(7)
#define DCM_CFG_DBC_ENABLE		BIT(15)
#define DCM_CFG_FAXI_FSEL_MASK		0x1f
#define DCM_CFG_FAXI_FSEL_DIV2		0x0f

#endif /* _MTK_DCM_H */
