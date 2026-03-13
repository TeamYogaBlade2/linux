/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __SOC_MEDIATEK_MT6572_PM_DOMAINS_H
#define __SOC_MEDIATEK_MT6572_PM_DOMAINS_H

#include "mtk-pm-domains.h"
#include <dt-bindings/power/mediatek,mt6572-power-controller.h>

/*
 * MT6572 power domain support
 */

static enum scpsys_bus_prot_block scpsys_bus_prot_blocks_mt6572[] = {
	BUS_PROT_BLOCK_INFRA
};

static const struct scpsys_domain_data scpsys_domain_data_mt6572[] = {
	[MT6572_POWER_DOMAIN_MD1] = {
		.name = "md1",
		.sta_mask = PWR_STATUS_MD1,
		.ctl_offs = SPM_MD1_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = 0,
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6572_TOP_AXI_PROT_EN_MD1),
		},
	},
	[MT6572_POWER_DOMAIN_CONN] = {
		.name = "conn",
		.sta_mask = PWR_STATUS_CONN,
		.ctl_offs = SPM_CONN_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = 0,
		.sram_pdn_ack_bits = 0,
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6572_TOP_AXI_PROT_EN_CONN),
		},
	},
	[MT6572_POWER_DOMAIN_DIS] = {
		.name = "dis",
		.sta_mask = PWR_STATUS_DISP,
		.ctl_offs = SPM_DIS_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = GENMASK(11, 8),
		.sram_pdn_ack_bits = GENMASK(15, 12),
		.bp_cfg = {
			BUS_PROT_INFRA_UPDATE_TOPAXI(MT6572_TOP_AXI_PROT_EN_DIS),
		},
	},
	[MT6572_POWER_DOMAIN_MFG] = {
		.name = "mfg",
		.sta_mask = PWR_STATUS_MFG,
		.ctl_offs = SPM_MFG_PWR_CON,
		.pwr_sta_offs = SPM_PWR_STATUS,
		.pwr_sta2nd_offs = SPM_PWR_STATUS_2ND,
		.sram_pdn_bits = BIT(8),
		.sram_pdn_ack_bits = BIT(12),
	},
};

static const struct scpsys_soc_data mt6572_scpsys_data = {
	.domains_data = scpsys_domain_data_mt6572,
	.num_domains = ARRAY_SIZE(scpsys_domain_data_mt6572),
	.bus_prot_blocks = scpsys_bus_prot_blocks_mt6572,
	.num_bus_prot_blocks = ARRAY_SIZE(scpsys_bus_prot_blocks_mt6572),
};

#endif /* __SOC_MEDIATEK_MT6572_PM_DOMAINS_H */
