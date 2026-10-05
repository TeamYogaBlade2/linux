/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#ifndef __MTK_MMSYS_H
#define __MTK_MMSYS_H

#include <linux/mailbox_controller.h>
#include <linux/mailbox/mtk-cmdq-mailbox.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

enum mtk_ddp_comp_id;
struct device;

enum mtk_dpi_out_format_con {
	MTK_DPI_RGB888_SDR_CON,
	MTK_DPI_RGB888_DDR_CON,
	MTK_DPI_RGB565_SDR_CON,
	MTK_DPI_RGB565_DDR_CON
};

enum mtk_ddp_comp_id {
	DDP_COMPONENT_2DSHARP,
	DDP_COMPONENT_AAL0,
	DDP_COMPONENT_AAL1,
	DDP_COMPONENT_BLS,
	DDP_COMPONENT_CCORR,
	DDP_COMPONENT_COLOR0,
	DDP_COMPONENT_COLOR1,
	DDP_COMPONENT_DITHER0,
	DDP_COMPONENT_DITHER1,
	DDP_COMPONENT_DP_INTF0,
	DDP_COMPONENT_DP_INTF1,
	DDP_COMPONENT_DPI0,
	DDP_COMPONENT_DPI1,
	DDP_COMPONENT_DSC0,
	DDP_COMPONENT_DSC1,
	DDP_COMPONENT_DSI0,
	DDP_COMPONENT_DSI1,
	DDP_COMPONENT_DSI2,
	DDP_COMPONENT_DSI3,
	DDP_COMPONENT_ETHDR_MIXER,
	DDP_COMPONENT_GAMMA,
	DDP_COMPONENT_MDP_RDMA0,
	DDP_COMPONENT_MDP_RDMA1,
	DDP_COMPONENT_MDP_RDMA2,
	DDP_COMPONENT_MDP_RDMA3,
	DDP_COMPONENT_MDP_RDMA4,
	DDP_COMPONENT_MDP_RDMA5,
	DDP_COMPONENT_MDP_RDMA6,
	DDP_COMPONENT_MDP_RDMA7,
	DDP_COMPONENT_MERGE0,
	DDP_COMPONENT_MERGE1,
	DDP_COMPONENT_MERGE2,
	DDP_COMPONENT_MERGE3,
	DDP_COMPONENT_MERGE4,
	DDP_COMPONENT_MERGE5,
	DDP_COMPONENT_OD0,
	DDP_COMPONENT_OD1,
	DDP_COMPONENT_OVL0,
	DDP_COMPONENT_OVL_2L0,
	DDP_COMPONENT_OVL_2L1,
	DDP_COMPONENT_OVL_2L2,
	DDP_COMPONENT_OVL1,
	DDP_COMPONENT_PADDING0,
	DDP_COMPONENT_PADDING1,
	DDP_COMPONENT_PADDING2,
	DDP_COMPONENT_PADDING3,
	DDP_COMPONENT_PADDING4,
	DDP_COMPONENT_PADDING5,
	DDP_COMPONENT_PADDING6,
	DDP_COMPONENT_PADDING7,
	DDP_COMPONENT_POSTMASK0,
	DDP_COMPONENT_PWM0,
	DDP_COMPONENT_PWM1,
	DDP_COMPONENT_PWM2,
	DDP_COMPONENT_RDMA0,
	DDP_COMPONENT_RDMA1,
	DDP_COMPONENT_RDMA2,
	DDP_COMPONENT_RDMA4,
	DDP_COMPONENT_ROT,
	DDP_COMPONENT_SCL,
	DDP_COMPONENT_TDSHP,
	DDP_COMPONENT_UFOE,
	DDP_COMPONENT_WDMA0,
	DDP_COMPONENT_WDMA1,
	DDP_COMPONENT_DBI0,
	DDP_COMPONENT_ID_MAX,
};

/**
 * mtk_ddp_comp_name - the device tree name of a display component
 * @comp_id: a DDP_COMPONENT_* id
 *
 * The same strings that "mediatek,display-path" and "mediatek,display-routes"
 * use, so a message about a component reads the way the device tree that
 * configured it does.
 */
const char *mtk_ddp_comp_name(unsigned int comp_id);

void mtk_mmsys_ddp_connect(struct device *dev,
			   enum mtk_ddp_comp_id cur,
			   enum mtk_ddp_comp_id next);

void mtk_mmsys_ddp_disconnect(struct device *dev,
			      enum mtk_ddp_comp_id cur,
			      enum mtk_ddp_comp_id next);

/**
 * mtk_mmsys_ddp_route_available - is @from -> @to wired up at all?
 * @dev: the mmsys device
 * @from: the output component
 * @to: the input component it should feed
 *
 * mtk_mmsys_ddp_connect() only writes registers for pairs that its routing
 * table lists, and it returns nothing, so a pair it does not know is a silent
 * no-op that leaves the crossbar as the bootloader left it.  That is how a
 * path can be wired up correctly in the device tree, compile, probe, and still
 * display nothing.  Callers that cannot tolerate that ask this first.
 *
 * Return: true when the mmsys has at least one route entry for the pair,
 * counting a fixed link as a route.  False when the mmsys has no routing
 * table at all, which is a false "unavailable" and not an error - only SoCs
 * whose routes come from device tree require every connected pair to match.
 */
bool mtk_mmsys_ddp_route_available(struct device *dev, enum mtk_ddp_comp_id from,
				   enum mtk_ddp_comp_id to);

/**
 * mtk_mmsys_ddp_routes_from_dt - are this mmsys's routes read from DT?
 * @dev: the mmsys device
 *
 * True only for a node with a "mediatek,display-routes" property, i.e. one
 * whose routing table this driver parsed at probe and could therefore
 * complain about.  Every other SoC's routes come from a C table that the
 * driver cannot check, so nothing about them is asserted.
 */
bool mtk_mmsys_ddp_routes_from_dt(struct device *dev);

/**
 * mtk_mmsys_ddp_display_path - read "mediatek,display-path"
 * @dev: the mmsys device
 * @path: output array of component ids
 * @max: size of @path
 * @len: receives the number of components
 *
 * Resolve the component names in "mediatek,display-path" to DDP_COMPONENT_*
 * ids, in the order the pipeline runs.
 *
 * Return: 0 on success, -ENOENT if the node has no such property, -EINVAL if
 * it names something this driver does not know or does not fit in @path.
 */
int mtk_mmsys_ddp_display_path(struct device *dev, unsigned int *path,
			       unsigned int max, unsigned int *len);

void mtk_mmsys_ddp_dpi_fmt_config(struct device *dev, u32 val);

void mtk_mmsys_merge_async_config(struct device *dev, int idx, int width,
				  int height, struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_hdr_config(struct device *dev, int be_width, int be_height,
			  struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_mixer_in_config(struct device *dev, int idx, bool alpha_sel, u16 alpha,
			       u8 mode, u32 biwidth, struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_mixer_in_channel_swap(struct device *dev, int idx, bool channel_swap,
				     struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_vpp_rsz_merge_config(struct device *dev, u32 id, bool enable,
				    struct cmdq_pkt *cmdq_pkt);

void mtk_mmsys_vpp_rsz_dcm_config(struct device *dev, bool enable,
				  struct cmdq_pkt *cmdq_pkt);

#endif /* __MTK_MMSYS_H */
