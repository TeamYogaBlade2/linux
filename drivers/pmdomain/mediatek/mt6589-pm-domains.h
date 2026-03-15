/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_MEDIATEK_MT6589_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6589_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mt6589-power.h>

/*
 * MT6589 power domain support
 */

static const struct scpsys_domain_data scpsys_domain_data_mt6589[] = {
	[MT6589_POWER_DOMAIN_MD1] = {
		.name = "md1",
		.sta_mask = PWR_STATUS_MD1,
		.ctl_offs = SPM_MD1_PWR_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = 0, /* don't have */
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
		/*
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6589_TOP_AXI_PROT_EN_MD1),
		},
		*/
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_MD2] = {
		.name = "md2",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = 0, /* don't have */
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
		/*
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6589_TOP_AXI_PROT_EN_MD2),
		},
		*/
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_DPY] = {
		.name = "dpy",
		.sta_mask = PWR_STATUS_DDRPHY,
		.ctl_offs = 0x0240,
		.caps = MTK_SCPD_ALWAYS_ON | MTK_SCPD_NO_SRAM,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_DIS] = {
		.name = "dis",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		/*
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6589_TOP_AXI_PROT_EN_DIS),
		},
		*/
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
		/*
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(TODO),
		},
		*/
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_ISP] = {
		.name = "isp",
		.sta_mask = PWR_STATUS_ISP,
		.ctl_offs = SPM_ISP_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
		/*
		.bp_cfg = BUS_PROT_INFRA_UPDATE_TOPAXI(TODO), img_s_prot_en?,
		*/
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_IFR] = {
		.name = "ifr",
		.sta_mask = PWR_STATUS_INFRASYS,
		.ctl_offs = 0x0234,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.caps = MTK_SCPD_ALWAYS_ON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_VEN] = {
		.name = "ven",
		.sta_mask = BIT(7),
		.ctl_offs = SPM_VEN_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
	[MT6589_POWER_DOMAIN_VDE] = {
		.name = "vde",
		.sta_mask = BIT(8),
		.ctl_offs = SPM_VDE_PWR_CON,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.caps = MTK_SCPD_KEEP_DEFAULT_OFF,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
	},
};

static const struct scpsys_soc_data mt6589_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6589,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6589),
};

#endif /* __SOC_MEDIATEK_MT6589_PM_DOMAINS_H */
