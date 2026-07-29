// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */
 
#ifndef __SOC_MEDIATEK_MT6582_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6582_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mediatek,mt6582-power.h>


#define MT6582_TOP_AXI_PROT_EN_MCI_M2		BIT(0)
#define MT6582_TOP_AXI_PROT_EN_MM_M0		BIT(1)
#define MT6582_TOP_AXI_PROT_EN_CONN_M		BIT(2)
#define MT6582_TOP_AXI_PROT_EN_MD1_M1		BIT(3)
#define MT6582_TOP_AXI_PROT_EN_MD1_M2		BIT(4)
#define MT6582_TOP_AXI_PROT_EN_MD1_M0		BIT(5)
#define MT6582_TOP_AXI_PROT_EN_MMAPB_S		BIT(6)
#define MT6582_TOP_AXI_PROT_EN_AP2MD_MD1	BIT(7)
#define MT6582_TOP_AXI_PROT_EN_CONN_S		BIT(8)
#define MT6582_TOP_AXI_PROT_EN_L2C_M2		BIT(9)
#define MT6582_TOP_AXI_PROT_EN_PERI_M0		BIT(10)
#define MT6582_TOP_AXI_PROT_EN_L2SS_SMI		BIT(11)
#define MT6582_TOP_AXI_PROT_EN_L2SS_AFF		BIT(12)

/*
 * MT6582 power domain support
 */

 static const struct scpsys_domain_data scpsys_domain_data_mt6582[] = {
	[MT6582_POWER_DOMAIN_MD1] = {
		.name = "md1",
		.sta_mask = PWR_STATUS_MD1,
		.ctl_offs = SPM_MD1_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = 0,
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6582_TOP_AXI_PROT_EN_MD1_M0 | 
                MT6582_TOP_AXI_PROT_EN_MD1_M1 | MT6582_TOP_AXI_PROT_EN_MD1_M2 |
                MT6582_TOP_AXI_PROT_EN_AP2MD_MD1),
		},
	},
	[MT6582_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(8, 8),
		.sram_pdn_ack_bits = 0,
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6582_TOP_AXI_PROT_EN_CONN_M |
            MT6582_TOP_AXI_PROT_EN_CONN_S),
		},
	},
	[MT6582_POWER_DOMAIN_DIS] = {
		.name = "dis",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6582_TOP_AXI_PROT_EN_MM_M0),
		},
	},
	[MT6582_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		
	},
	[MT6582_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
	},
	[MT6582_POWER_DOMAIN_VDE] = {
		.name = "vde",
		.sta_mask = PWR_STATUS_VDEC,
		.ctl_offs = SPM_VDE_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
	},

};

static const struct scpsys_soc_data mt6582_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6582,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6582),
};

#endif /* __SOC_MEDIATEK_MT6582_PM_DOMAINS_H */
