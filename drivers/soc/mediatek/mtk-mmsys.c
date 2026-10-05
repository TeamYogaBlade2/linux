// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: James Liao <jamesjj.liao@mediatek.com>
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/soc/mediatek/mtk-mmsys.h>
#include <linux/string.h>

#include "mtk-mmsys.h"
#include "mt6589-dispsys.h"
#include "mt8167-mmsys.h"
#include "mt8173-mmsys.h"
#include "mt8183-mmsys.h"
#include "mt8186-mmsys.h"
#include "mt8188-mmsys.h"
#include "mt8192-mmsys.h"
#include "mt8195-mmsys.h"
#include "mt8365-mmsys.h"

#define MMSYS_SW_RESET_PER_REG 32

static const struct mtk_mmsys_driver_data mt2701_mmsys_driver_data = {
	.clk_driver = "clk-mt2701-mm",
	.routes = mmsys_default_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_default_routing_table),
};

static const struct mtk_mmsys_driver_data mt2712_mmsys_driver_data = {
	.clk_driver = "clk-mt2712-mm",
	.routes = mmsys_default_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_default_routing_table),
};

static const struct mtk_mmsys_driver_data mt6589_dispsys_driver_data = {
	.clk_driver = "clk-mt6589-disp",
	.sw0_rst_offset = MT6589_DISP_SW_RST_B,
	.num_resets = MT6589_DISP_NUM_RESETS,
};

static const struct mtk_mmsys_driver_data mt6779_mmsys_driver_data = {
	.clk_driver = "clk-mt6779-mm",
};

static const struct mtk_mmsys_driver_data mt6795_mmsys_driver_data = {
	.clk_driver = "clk-mt6795-mm",
	.routes = mt8173_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8173_mmsys_routing_table),
	.sw0_rst_offset = MT8183_MMSYS_SW0_RST_B,
	.num_resets = 64,
};

static const struct mtk_mmsys_driver_data mt6797_mmsys_driver_data = {
	.clk_driver = "clk-mt6797-mm",
};

static const struct mtk_mmsys_driver_data mt8167_mmsys_driver_data = {
	.clk_driver = "clk-mt8167-mm",
	.routes = mt8167_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8167_mmsys_routing_table),
};

static const struct mtk_mmsys_driver_data mt8173_mmsys_driver_data = {
	.clk_driver = "clk-mt8173-mm",
	.routes = mt8173_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8173_mmsys_routing_table),
	.sw0_rst_offset = MT8183_MMSYS_SW0_RST_B,
	.num_resets = 64,
};

static const struct mtk_mmsys_driver_data mt8183_mmsys_driver_data = {
	.clk_driver = "clk-mt8183-mm",
	.routes = mmsys_mt8183_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8183_routing_table),
	.sw0_rst_offset = MT8183_MMSYS_SW0_RST_B,
	.num_resets = 32,
};

static const struct mtk_mmsys_driver_data mt8186_mmsys_driver_data = {
	.clk_driver = "clk-mt8186-mm",
	.routes = mmsys_mt8186_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8186_routing_table),
	.sw0_rst_offset = MT8186_MMSYS_SW0_RST_B,
	.num_resets = 32,
};

static const struct mtk_mmsys_driver_data mt8188_vdosys0_driver_data = {
	.clk_driver = "clk-mt8188-vdo0",
	.routes = mmsys_mt8188_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8188_routing_table),
	.sw0_rst_offset = MT8188_VDO0_SW0_RST_B,
	.rst_tb = mmsys_mt8188_vdo0_rst_tb,
	.num_resets = ARRAY_SIZE(mmsys_mt8188_vdo0_rst_tb),
};

static const struct mtk_mmsys_driver_data mt8188_vdosys1_driver_data = {
	.clk_driver = "clk-mt8188-vdo1",
	.routes = mmsys_mt8188_vdo1_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8188_vdo1_routing_table),
	.sw0_rst_offset = MT8188_VDO1_SW0_RST_B,
	.rst_tb = mmsys_mt8188_vdo1_rst_tb,
	.num_resets = ARRAY_SIZE(mmsys_mt8188_vdo1_rst_tb),
	.vsync_len = 1,
};

static const struct mtk_mmsys_driver_data mt8188_vppsys0_driver_data = {
	.clk_driver = "clk-mt8188-vpp0",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8188_vppsys1_driver_data = {
	.clk_driver = "clk-mt8188-vpp1",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8192_mmsys_driver_data = {
	.clk_driver = "clk-mt8192-mm",
	.routes = mmsys_mt8192_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8192_routing_table),
	.sw0_rst_offset = MT8186_MMSYS_SW0_RST_B,
	.num_resets = 32,
};

static const struct mtk_mmsys_driver_data mt8195_vdosys0_driver_data = {
	.clk_driver = "clk-mt8195-vdo0",
	.routes = mmsys_mt8195_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8195_routing_table),
};

static const struct mtk_mmsys_driver_data mt8195_vdosys1_driver_data = {
	.clk_driver = "clk-mt8195-vdo1",
	.routes = mmsys_mt8195_vdo1_routing_table,
	.num_routes = ARRAY_SIZE(mmsys_mt8195_vdo1_routing_table),
	.sw0_rst_offset = MT8195_VDO1_SW0_RST_B,
	.num_resets = 64,
};

static const struct mtk_mmsys_driver_data mt8195_vppsys0_driver_data = {
	.clk_driver = "clk-mt8195-vpp0",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8195_vppsys1_driver_data = {
	.clk_driver = "clk-mt8195-vpp1",
	.is_vppsys = true,
};

static const struct mtk_mmsys_driver_data mt8365_mmsys_driver_data = {
	.clk_driver = "clk-mt8365-mm",
	.routes = mt8365_mmsys_routing_table,
	.num_routes = ARRAY_SIZE(mt8365_mmsys_routing_table),
};

struct mtk_mmsys {
	void __iomem *regs;
	const struct mtk_mmsys_driver_data *data;
	struct platform_device *clks_pdev;
	struct platform_device *drm_pdev;
	spinlock_t lock; /* protects mmsys_sw_rst_b reg */
	struct reset_controller_dev rcdev;
	struct cmdq_client_reg cmdq_base;
	/*
	 * Routing table read from "mediatek,display-routes", used in place of
	 * ->data->routes when it is not NULL.  It is kept here rather than in
	 * ->data because that is a static const table shared by every device
	 * using this driver; an SoC whose routes come from device tree still
	 * shares it, and overwriting it would change all of them.
	 */
	struct mtk_mmsys_routes *dt_routes;
	unsigned int num_dt_routes;
};

/*
 * The routing table in force for this mmsys: the one from device tree when the
 * node has "mediatek,display-routes", otherwise the SoC's C table.
 */
static const struct mtk_mmsys_routes *mtk_mmsys_routes(struct mtk_mmsys *mmsys,
						       unsigned int *nroutes)
{
	if (mmsys->dt_routes) {
		*nroutes = mmsys->num_dt_routes;
		return mmsys->dt_routes;
	}

	*nroutes = mmsys->data->num_routes;
	return mmsys->data->routes;
}


/*
 * The name of every display component, indexed by enum mtk_ddp_comp_id, in the
 * spelling the "mediatek,display-path" and "mediatek,display-routes" device
 * tree properties use.  mtk_mmsys_comp_by_name() resolves a property to an id
 * and mtk_ddp_comp_name() does the reverse, so a message about a component
 * reads the way the device tree that configured it does.
 *
 * The order is positional and the size is asserted below, so an entry added
 * to enum mtk_ddp_comp_id without a name here fails the build rather than
 * making every name after it mean the wrong component.  A hole cannot happen
 * in C; a mistyped NAME can, and mtk_mmsys_comp_by_name() simply does not
 * match it, so the property fails at probe instead of matching some other
 * component.
 */
static const char * const mtk_ddp_comp_names[] = {
	"2DSHARP",
	"AAL0",
	"AAL1",
	"BLS",
	"CCORR",
	"COLOR0",
	"COLOR1",
	"DITHER0",
	"DITHER1",
	"DP_INTF0",
	"DP_INTF1",
	"DPI0",
	"DPI1",
	"DSC0",
	"DSC1",
	"DSI0",
	"DSI1",
	"DSI2",
	"DSI3",
	"ETHDR_MIXER",
	"GAMMA",
	"MDP_RDMA0",
	"MDP_RDMA1",
	"MDP_RDMA2",
	"MDP_RDMA3",
	"MDP_RDMA4",
	"MDP_RDMA5",
	"MDP_RDMA6",
	"MDP_RDMA7",
	"MERGE0",
	"MERGE1",
	"MERGE2",
	"MERGE3",
	"MERGE4",
	"MERGE5",
	"OD0",
	"OD1",
	"OVL0",
	"OVL_2L0",
	"OVL_2L1",
	"OVL_2L2",
	"OVL1",
	"PADDING0",
	"PADDING1",
	"PADDING2",
	"PADDING3",
	"PADDING4",
	"PADDING5",
	"PADDING6",
	"PADDING7",
	"POSTMASK0",
	"PWM0",
	"PWM1",
	"PWM2",
	"RDMA0",
	"RDMA1",
	"RDMA2",
	"RDMA4",
	"ROT",
	"SCL",
	"TDSHP",
	"UFOE",
	"WDMA0",
	"WDMA1",
	"DBI0",
};


static_assert(ARRAY_SIZE(mtk_ddp_comp_names) == DDP_COMPONENT_ID_MAX);

const char *mtk_ddp_comp_name(unsigned int comp_id)
{
	if (comp_id >= ARRAY_SIZE(mtk_ddp_comp_names))
		return "<unknown>";

	return mtk_ddp_comp_names[comp_id];
}
EXPORT_SYMBOL_GPL(mtk_ddp_comp_name);

static int mtk_mmsys_comp_by_name(const char *name, enum mtk_ddp_comp_id *comp)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_ddp_comp_names); i++) {
		if (!mtk_ddp_comp_names[i] ||
		    strcasecmp(name, mtk_ddp_comp_names[i]))
			continue;

		*comp = i;
		return 0;
	}

	return -EINVAL;
}

static void mtk_mmsys_update_bits(struct mtk_mmsys *mmsys, u32 offset, u32 mask, u32 val,
				  struct cmdq_pkt *cmdq_pkt)
{
	int ret;
	u32 tmp;

	if (mmsys->cmdq_base.size && cmdq_pkt) {
		ret = cmdq_pkt_write_mask(cmdq_pkt, mmsys->cmdq_base.subsys,
					  mmsys->cmdq_base.offset + offset, val,
					  mask);
		if (ret)
			pr_debug("CMDQ unavailable: using CPU write\n");
		else
			return;
	}
	tmp = readl_relaxed(mmsys->regs + offset);
	tmp = (tmp & ~mask) | (val & mask);
	writel_relaxed(tmp, mmsys->regs + offset);
}

void mtk_mmsys_ddp_connect(struct device *dev,
			   enum mtk_ddp_comp_id cur,
			   enum mtk_ddp_comp_id next)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_routes *routes;
	unsigned int nroutes;
	int i;

	routes = mtk_mmsys_routes(mmsys, &nroutes);

	for (i = 0; i < nroutes; i++)
		if (cur == routes[i].from_comp && next == routes[i].to_comp)
			mtk_mmsys_update_bits(mmsys, routes[i].addr, routes[i].mask,
					      routes[i].val, NULL);

	if (mmsys->data->vsync_len)
		mtk_mmsys_update_bits(mmsys, MT8188_VDO1_MIXER_VSYNC_LEN, GENMASK(31, 0),
				      mmsys->data->vsync_len, NULL);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_connect);

void mtk_mmsys_ddp_disconnect(struct device *dev,
			      enum mtk_ddp_comp_id cur,
			      enum mtk_ddp_comp_id next)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_routes *routes;
	unsigned int nroutes;
	int i;

	routes = mtk_mmsys_routes(mmsys, &nroutes);

	for (i = 0; i < nroutes; i++)
		if (cur == routes[i].from_comp && next == routes[i].to_comp)
			mtk_mmsys_update_bits(mmsys, routes[i].addr, routes[i].mask, 0, NULL);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_disconnect);

void mtk_mmsys_merge_async_config(struct device *dev, int idx, int width, int height,
				  struct cmdq_pkt *cmdq_pkt)
{
	mtk_mmsys_update_bits(dev_get_drvdata(dev), MT8195_VDO1_MERGE0_ASYNC_CFG_WD + 0x10 * idx,
			      ~0, height << 16 | width, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_merge_async_config);

void mtk_mmsys_hdr_config(struct device *dev, int be_width, int be_height,
			  struct cmdq_pkt *cmdq_pkt)
{
	mtk_mmsys_update_bits(dev_get_drvdata(dev), MT8195_VDO1_HDRBE_ASYNC_CFG_WD, ~0,
			      be_height << 16 | be_width, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_hdr_config);

void mtk_mmsys_mixer_in_config(struct device *dev, int idx, bool alpha_sel, u16 alpha,
			       u8 mode, u32 biwidth, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_MIXER_IN1_ALPHA + (idx - 1) * 4, ~0,
			      alpha << 16 | alpha, cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_HDR_TOP_CFG, BIT(15 + idx), 0, cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_HDR_TOP_CFG, BIT(19 + idx),
			      alpha_sel << (19 + idx), cmdq_pkt);
	mtk_mmsys_update_bits(mmsys, MT8195_VDO1_MIXER_IN1_PAD + (idx - 1) * 4,
			      GENMASK(31, 16) | GENMASK(1, 0), biwidth << 16 | mode, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_mixer_in_config);

void mtk_mmsys_mixer_in_channel_swap(struct device *dev, int idx, bool channel_swap,
				     struct cmdq_pkt *cmdq_pkt)
{
	mtk_mmsys_update_bits(dev_get_drvdata(dev), MT8195_VDO1_MIXER_IN1_PAD + (idx - 1) * 4,
			      BIT(4), channel_swap << 4, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_mixer_in_channel_swap);

void mtk_mmsys_ddp_dpi_fmt_config(struct device *dev, u32 val)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	switch (val) {
	case MTK_DPI_RGB888_SDR_CON:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB888_SDR_CON, NULL);
		break;
	case MTK_DPI_RGB565_SDR_CON:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB565_SDR_CON, NULL);
		break;
	case MTK_DPI_RGB565_DDR_CON:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB565_DDR_CON, NULL);
		break;
	case MTK_DPI_RGB888_DDR_CON:
	default:
		mtk_mmsys_update_bits(mmsys, MT8186_MMSYS_DPI_OUTPUT_FORMAT,
				      MT8186_DPI_FORMAT_MASK, MT8186_DPI_RGB888_DDR_CON, NULL);
		break;
	}
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_dpi_fmt_config);

void mtk_mmsys_vpp_rsz_merge_config(struct device *dev, u32 id, bool enable,
				    struct cmdq_pkt *cmdq_pkt)
{
	u32 reg;

	switch (id) {
	case 2:
		reg = MT8195_SVPP2_BUF_BF_RSZ_SWITCH;
		break;
	case 3:
		reg = MT8195_SVPP3_BUF_BF_RSZ_SWITCH;
		break;
	default:
		dev_err(dev, "Invalid id %d\n", id);
		return;
	}

	mtk_mmsys_update_bits(dev_get_drvdata(dev), reg, ~0, enable, cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_vpp_rsz_merge_config);

void mtk_mmsys_vpp_rsz_dcm_config(struct device *dev, bool enable,
				  struct cmdq_pkt *cmdq_pkt)
{
	u32 client;

	client = MT8195_SVPP1_MDP_RSZ;
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_1ST_DIS0, client,
			      ((enable) ? client : 0), cmdq_pkt);
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_2ND_DIS0, client,
			      ((enable) ? client : 0), cmdq_pkt);

	client = MT8195_SVPP2_MDP_RSZ | MT8195_SVPP3_MDP_RSZ;
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_1ST_DIS1, client,
			      ((enable) ? client : 0), cmdq_pkt);
	mtk_mmsys_update_bits(dev_get_drvdata(dev),
			      MT8195_VPP1_HW_DCM_2ND_DIS1, client,
			      ((enable) ? client : 0), cmdq_pkt);
}
EXPORT_SYMBOL_GPL(mtk_mmsys_vpp_rsz_dcm_config);

static int mtk_mmsys_reset_update(struct reset_controller_dev *rcdev, unsigned long id,
				  bool assert)
{
	struct mtk_mmsys *mmsys = container_of(rcdev, struct mtk_mmsys, rcdev);
	unsigned long flags;
	u32 offset;
	u32 reg;

	if (mmsys->data->rst_tb) {
		if (id >= mmsys->data->num_resets) {
			dev_err(rcdev->dev, "Invalid reset ID: %lu (>=%u)\n",
				id, mmsys->data->num_resets);
			return -EINVAL;
		}
		id = mmsys->data->rst_tb[id];
	}

	offset = (id / MMSYS_SW_RESET_PER_REG) * sizeof(u32);
	id = id % MMSYS_SW_RESET_PER_REG;
	reg = mmsys->data->sw0_rst_offset + offset;

	spin_lock_irqsave(&mmsys->lock, flags);

	if (assert)
		mtk_mmsys_update_bits(mmsys, reg, BIT(id), 0, NULL);
	else
		mtk_mmsys_update_bits(mmsys, reg, BIT(id), BIT(id), NULL);

	spin_unlock_irqrestore(&mmsys->lock, flags);

	return 0;
}

static int mtk_mmsys_reset_assert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return mtk_mmsys_reset_update(rcdev, id, true);
}

static int mtk_mmsys_reset_deassert(struct reset_controller_dev *rcdev, unsigned long id)
{
	return mtk_mmsys_reset_update(rcdev, id, false);
}

static int mtk_mmsys_reset(struct reset_controller_dev *rcdev, unsigned long id)
{
	int ret;

	ret = mtk_mmsys_reset_assert(rcdev, id);
	if (ret)
		return ret;

	usleep_range(1000, 1100);

	return mtk_mmsys_reset_deassert(rcdev, id);
}

static const struct reset_control_ops mtk_mmsys_reset_ops = {
	.assert = mtk_mmsys_reset_assert,
	.deassert = mtk_mmsys_reset_deassert,
	.reset = mtk_mmsys_reset,
};

/*
 * "mediatek,display-path" and "mediatek,display-routes" are string lists, one
 * entry per line of the C tables they replace, in the same order:
 *
 *	mediatek,display-path = "ovl0", "bls", "rdma0", "dsi0";
 *	mediatek,display-routes =
 *		"ovl0 bls 0x24 0x2 0x2",	 - DISP_OVL_MOUT_EN_BLS
 *		"ovl0 bls 0x54 0x3 0x0",	 - DISP_BLS_SEL_OVL
 *		"bls rdma0 0x0 0x0 0x0",	 - fixed link
 *		"rdma0 dsi0 0x34 0x3 0x0";	 - DISP_RDMA0_OUT_SEL_DSI
 *
 * A route is "<from> <to> <reg offset> <mask> <value>"; an offset of 0 marks a
 * fixed link, which is hardware wiring that no register selects.  Strings
 * rather than cells because both lists name components, and a cell property
 * cannot be read as a list of names; sscanf() parses the numbers.
 */
#define MTK_MMSYS_ROUTE_STR_FMT	"%15s %15s %x %x %x"

static int mtk_mmsys_ddp_route_parse(struct device *dev, const char *str,
				     struct mtk_mmsys_routes *route)
{
	enum mtk_ddp_comp_id from, to;
	char from_name[16], to_name[16];
	int ret;

	ret = sscanf(str, MTK_MMSYS_ROUTE_STR_FMT, from_name, to_name,
		     &route->addr, &route->mask, &route->val);
	if (ret != 5)
		return -EINVAL;

	ret = mtk_mmsys_comp_by_name(from_name, &from);
	if (ret)
		return ret;
	ret = mtk_mmsys_comp_by_name(to_name, &to);
	if (ret)
		return ret;

	route->from_comp = from;
	route->to_comp = to;

	/*
	 * The C tables get this from MMSYS_ROUTE()'s build-time check, so a
	 * route whose value does not fit its mask cannot reach hardware.  There
	 * is no equivalent for text parsed at probe, so check it here.
	 */
	if (route->mask && (route->val & ~route->mask))
		dev_warn(dev, "route \"%s\": value %#x does not fit mask %#x\n",
			 str, route->val, route->mask);

	return 0;
}

static int mtk_mmsys_routes_of(struct mtk_mmsys *mmsys, struct device *dev)
{
	struct device_node *node = dev->of_node;
	struct mtk_mmsys_routes *routes;
	const char *str;
	unsigned int i, nroutes = 0;
	int ret;

	/*
	 * Count the entries first so the array can be allocated in one go.
	 * of_property_read_string_index() is what decides the count here
	 * because the loop has to walk to the end of the property anyway.
	 */
	while (of_property_read_string_index(node, "mediatek,display-routes",
					     nroutes, &str) == 0)
		nroutes++;

	routes = devm_kcalloc(dev, nroutes, sizeof(*routes), GFP_KERNEL);
	if (!routes)
		return -ENOMEM;

	for (i = 0; i < nroutes; i++) {
		of_property_read_string_index(node, "mediatek,display-routes",
					      i, &str);
		ret = mtk_mmsys_ddp_route_parse(dev, str, &routes[i]);
		if (ret) {
			dev_warn(dev, "bad \"mediatek,display-routes\" entry \"%s\"\n",
				 str);
			return -EINVAL;
		}
	}

	/*
	 * A route whose "from" component is not on the pipeline is unused.
	 * That is normal here: most of these blocks have no driver, and the
	 * routes are kept because the crossbar is documented once rather than
	 * partly.  Worth saying out loud, though, because a mistyped component
	 * name looks exactly the same.
	 */
	if (of_property_present(node, "mediatek,display-path")) {
		unsigned int path[DDP_COMPONENT_ID_MAX], len;

		if (!mtk_mmsys_ddp_display_path(dev, path, ARRAY_SIZE(path), &len)) {
			for (i = 0; i < nroutes; i++) {
				unsigned int j;

				for (j = 0; j < len; j++)
					if (path[j] == routes[i].from_comp)
						break;
				if (j < len)
					continue;

				dev_dbg(dev, "route from %s to %s is unused: %s is not on the display path\n",
					mtk_ddp_comp_name(routes[i].from_comp),
					mtk_ddp_comp_name(routes[i].to_comp),
					mtk_ddp_comp_name(routes[i].from_comp));
			}
		}
	}

	mmsys->dt_routes = routes;
	mmsys->num_dt_routes = nroutes;

	return 0;
}

bool mtk_mmsys_ddp_route_available(struct device *dev, enum mtk_ddp_comp_id from,
				   enum mtk_ddp_comp_id to)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	const struct mtk_mmsys_routes *routes;
	unsigned int i;
	unsigned int nroutes;

	routes = mtk_mmsys_routes(mmsys, &nroutes);

	for (i = 0; i < nroutes; i++)
		if (from == routes[i].from_comp && to == routes[i].to_comp)
			return true;

	return false;
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_route_available);

bool mtk_mmsys_ddp_routes_from_dt(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	return mmsys->dt_routes != NULL;
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_routes_from_dt);

int mtk_mmsys_ddp_display_path(struct device *dev, unsigned int *path,
			       unsigned int max, unsigned int *len)
{
	struct device_node *node = dev->of_node;
	const char *str;
	unsigned int i;
	int ret;

	if (!of_property_present(node, "mediatek,display-path"))
		return -ENOENT;

	for (i = 0; ; i++) {
		enum mtk_ddp_comp_id comp;

		ret = of_property_read_string_index(node, "mediatek,display-path",
						    i, &str);
		if (ret)
			break;

		if (i >= max) {
			dev_err(dev, "mediatek,display-path has more than %u components\n",
				max);
			return -EINVAL;
		}

		ret = mtk_mmsys_comp_by_name(str, &comp);
		if (ret) {
			dev_err(dev, "mediatek,display-path: unknown component \"%s\"\n",
				str);
			return -EINVAL;
		}

		path[i] = comp;
	}

	*len = i;

	return 0;
}
EXPORT_SYMBOL_GPL(mtk_mmsys_ddp_display_path);

static int mtk_mmsys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *clks;
	struct platform_device *drm;
	struct mtk_mmsys *mmsys;
	int ret;

	mmsys = devm_kzalloc(dev, sizeof(*mmsys), GFP_KERNEL);
	if (!mmsys)
		return -ENOMEM;

	mmsys->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mmsys->regs)) {
		ret = PTR_ERR(mmsys->regs);
		dev_err(dev, "Failed to ioremap mmsys registers: %d\n", ret);
		return ret;
	}

	mmsys->data = of_device_get_match_data(&pdev->dev);

	/*
	 * SoCs whose routing table lives in the device tree read it here.  Every
	 * other SoC keeps the C table in its per-SoC header, and is left alone:
	 * its route list and its display path still have to be kept in step by
	 * hand, but reworking every SoC is a different job.
	 */
	if (of_property_present(dev->of_node, "mediatek,display-routes")) {
		ret = mtk_mmsys_routes_of(mmsys, dev);
		if (ret) {
			dev_err(dev, "Failed to read mediatek,display-routes: %d\n",
				ret);
			return ret;
		}
	}

	if (mmsys->data->num_resets > 0) {
		spin_lock_init(&mmsys->lock);

		mmsys->rcdev.owner = THIS_MODULE;
		mmsys->rcdev.nr_resets = mmsys->data->num_resets;
		mmsys->rcdev.ops = &mtk_mmsys_reset_ops;
		mmsys->rcdev.of_node = pdev->dev.of_node;
		ret = devm_reset_controller_register(&pdev->dev, &mmsys->rcdev);
		if (ret) {
			dev_err(&pdev->dev, "Couldn't register mmsys reset controller: %d\n", ret);
			return ret;
		}
	}

	/* CMDQ is optional */
	ret = cmdq_dev_get_client_reg(dev, &mmsys->cmdq_base, 0);
	if (ret)
		dev_dbg(dev, "No mediatek,gce-client-reg!\n");

	platform_set_drvdata(pdev, mmsys);

	clks = platform_device_register_data(&pdev->dev, mmsys->data->clk_driver,
					     PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(clks))
		return PTR_ERR(clks);
	mmsys->clks_pdev = clks;

	if (mmsys->data->is_vppsys)
		goto out_probe_done;

	drm = platform_device_register_data(&pdev->dev, "mediatek-drm",
					    PLATFORM_DEVID_AUTO, NULL, 0);
	if (IS_ERR(drm)) {
		platform_device_unregister(clks);
		return PTR_ERR(drm);
	}
	mmsys->drm_pdev = drm;

out_probe_done:
	return 0;
}

static void mtk_mmsys_remove(struct platform_device *pdev)
{
	struct mtk_mmsys *mmsys = platform_get_drvdata(pdev);

	platform_device_unregister(mmsys->drm_pdev);
	platform_device_unregister(mmsys->clks_pdev);
}

static const struct of_device_id of_match_mtk_mmsys[] = {
	{ .compatible = "mediatek,mt2701-mmsys", .data = &mt2701_mmsys_driver_data },
	{ .compatible = "mediatek,mt2712-mmsys", .data = &mt2712_mmsys_driver_data },
	{ .compatible = "mediatek,mt6589-dispsys", .data = &mt6589_dispsys_driver_data },
	{ .compatible = "mediatek,mt6779-mmsys", .data = &mt6779_mmsys_driver_data },
	{ .compatible = "mediatek,mt6795-mmsys", .data = &mt6795_mmsys_driver_data },
	{ .compatible = "mediatek,mt6797-mmsys", .data = &mt6797_mmsys_driver_data },
	{ .compatible = "mediatek,mt8167-mmsys", .data = &mt8167_mmsys_driver_data },
	{ .compatible = "mediatek,mt8173-mmsys", .data = &mt8173_mmsys_driver_data },
	{ .compatible = "mediatek,mt8183-mmsys", .data = &mt8183_mmsys_driver_data },
	{ .compatible = "mediatek,mt8186-mmsys", .data = &mt8186_mmsys_driver_data },
	{ .compatible = "mediatek,mt8188-vdosys0", .data = &mt8188_vdosys0_driver_data },
	{ .compatible = "mediatek,mt8188-vdosys1", .data = &mt8188_vdosys1_driver_data },
	{ .compatible = "mediatek,mt8188-vppsys0", .data = &mt8188_vppsys0_driver_data },
	{ .compatible = "mediatek,mt8188-vppsys1", .data = &mt8188_vppsys1_driver_data },
	{ .compatible = "mediatek,mt8192-mmsys", .data = &mt8192_mmsys_driver_data },
	/* "mediatek,mt8195-mmsys" compatible is deprecated */
	{ .compatible = "mediatek,mt8195-mmsys", .data = &mt8195_vdosys0_driver_data },
	{ .compatible = "mediatek,mt8195-vdosys0", .data = &mt8195_vdosys0_driver_data },
	{ .compatible = "mediatek,mt8195-vdosys1", .data = &mt8195_vdosys1_driver_data },
	{ .compatible = "mediatek,mt8195-vppsys0", .data = &mt8195_vppsys0_driver_data },
	{ .compatible = "mediatek,mt8195-vppsys1", .data = &mt8195_vppsys1_driver_data },
	{ .compatible = "mediatek,mt8365-mmsys", .data = &mt8365_mmsys_driver_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_match_mtk_mmsys);

static struct platform_driver mtk_mmsys_drv = {
	.driver = {
		.name = "mtk-mmsys",
		.of_match_table = of_match_mtk_mmsys,
	},
	.probe = mtk_mmsys_probe,
	.remove = mtk_mmsys_remove,
};
module_platform_driver(mtk_mmsys_drv);

MODULE_AUTHOR("Yongqiang Niu <yongqiang.niu@mediatek.com>");
MODULE_DESCRIPTION("MediaTek SoC MMSYS driver");
MODULE_LICENSE("GPL");
