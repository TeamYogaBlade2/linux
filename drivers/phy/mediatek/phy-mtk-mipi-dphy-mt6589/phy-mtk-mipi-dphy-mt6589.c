// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 MIPI CSI-2 D-PHY
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * THIS DRIVER IS A DOCUMENTED STUB.  It provides the generic-PHY provider,
 * the phy_ops and the DT-facing plumbing so that the media graph can be
 * wired up end to end, but mtk_mipi_csi_dphy_power_on() does not program
 * the analogue block, because the register map is not available.
 *
 * Why it is a stub
 * ----------------
 * The MT6589 sensor front end has two separate D-PHY receivers:
 *
 *   DVDD18_MIPIRX  powers CSI0, a full 4-lane D-PHY (RD0..RD3 plus RCK)
 *   DVDD18_MIPIIO  powers CSI1, a 1-lane interface
 *
 * Neither PHY's registers are documented in the MT6589 data sheet.  The
 * imgsys address map is documented exhaustively, from SMI_ISPSYS_COMMON at
 * 0x15003000 through seninf/csi2/SCAM at 0x15008000..0x15008500 to jpgdec at
 * 0x15009000 and jpgenc at 0x1500A310, and it contains no PHY registers at
 * all.  The PHY appears in the document only as the word "mipi_rx" inside
 * the description of img_rst ("resets imgsys and its related pad macro
 * (cam, mipi_rx)"), which places it in the imgsys reset domain but gives no
 * register offsets.
 *
 * The in-tree drivers/phy/mediatek/phy-mtk-mipi-csi-0-5.c is NOT a usable
 * starting point, despite the similar name.  It targets a different
 * register generation: it programs CSIXA/CSIXB ANA00..WRAPPER80 with a 0x1000
 * stride between the two banks, supports 4 data + 1 clock only, and its
 * only compatible is "mediatek,mt8365-csi-rx".  There is no evidence that
 * the MT6589 MIPI RX PHY uses that layout.  Writing this driver against
 * those offsets would produce something that compiles, looks plausible, and
 * silently fails to bring up a camera.
 *
 * What is needed to finish this
 * -----------------------------
 *   1. The MIPI RX PHY base address and register map, from the vendor
 *      "MIPI RX Configuration Module" functional specification (referenced
 *      but not included in the MT6589 data sheet) or from a later MediaTek
 *      tree.
 *   2. Whether MT6589 uses the CSIXA/CSIXB layout of
 *      phy-mtk-mipi-csi-0-5.c.  If it does, this file can be replaced by
 *      adding a mt6589 entry to that driver's of_match table and dropping
 *      this one entirely, which is the preferred outcome.
 *   3. Confirmation that CSI0 (MIPIRX, 4-lane) is the one carrying the main
 *      camera, and CSI1 (MIPIIO, 1-lane) the front camera.
 *
 * Until then power_on() returns success without touching the hardware.  That
 * is deliberate: it keeps the media graph complete and lets the receiver
 * probe, and the resulting failure is visible as a dead video stream rather
 * than as writes to wrong registers.  Any DT that enables this node should
 * be treated as experimental.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <dt-bindings/phy/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#include "../phy-mtk-io.h"

/*
 * Lane count, in the same 0..3 encoding the MT6589 CSI0 pin list implies:
 * RD0, RD1, RD2, RD3 plus RCK.
 */
#define MT6589_MIPI_CSI_DPHY_MAX_LANES	4

/*
 * Operating modes, mirroring the in-tree MT8365 SENINF driver's enum.
 * The MT6589 has D-PHY only: there is no C-PHY on this sensor front end.
 */
enum mtk_mipi_csi_dphy_mode {
	DPHY_MODE = 0,
	CPHY_MODE,
};

struct mtk_mipi_csi_dphy {
	struct device *dev;
	void __iomem *regs;
	struct phy *phy;

	/*
	 * Which D-PHY this is.  0 selects the 4-lane CSI0 on DVDD18_MIPIRX,
	 * 1 selects the 1-lane CSI1 on DVDD18_MIPIIO.
	 */
	u32 port;

	/*
	 * Operating mode, following the DT contract shared with the in-tree
	 * MT8365 SENINF PHY (see
	 * Documentation/devicetree/bindings/phy/mediatek,mt8365-csi-rx.yaml):
	 *
	 *   #phy-cells = <1>  mode carried in the PHY cells
	 *   #phy-cells = <0>  mode given by the "phy-type" property
	 *
	 * The MT6589 has no C-PHY, so only PHY_TYPE_DPHY is accepted, exactly
	 * as the in-tree driver does.
	 */
	u32 mode;

	/*
	 * Value of the node's #phy-cells: 0 or 1, per the shared binding.
	 */
	u32 phy_cells;

	/*
	 * Whether the register window has been established.  This stays
	 * false until the register map is known, and power_on() checks it.
	 */
	bool have_regs;

	u32 num_lanes;
};

/*
 * TODO(register map unknown): the analogue power-on sequence belongs here.
 *
 * For reference, the equivalent sequence in the in-tree mt8365 CD-PHY
 * driver is: disable the CPHY_EN bits, program the lane-to-clock routing,
 * invert the byte clocks, run the analogue EQ tuning, enable the bandgap and
 * low-pass filter, and finally release the wrapper from reset.  None of
 * those register offsets may be assumed to apply to the MT6589.
 */
static int mtk_mipi_csi_dphy_power_on(struct phy *phy)
{
	struct mtk_mipi_csi_dphy *dphy = phy_get_drvdata(phy);

	if (!dphy->have_regs) {
		dev_warn(dphy->dev,
			 "MIPI CSI D-PHY %u: register map unknown, power_on() is a no-op; video will not work\n",
			 dphy->port);
		return 0;
	}

	return -EOPNOTSUPP;
}

static int mtk_mipi_csi_dphy_power_off(struct phy *phy)
{
	struct mtk_mipi_csi_dphy *dphy = phy_get_drvdata(phy);

	if (!dphy->have_regs)
		return 0;

	return -EOPNOTSUPP;
}

static const struct phy_ops mtk_mipi_csi_dphy_ops = {
	.power_on = mtk_mipi_csi_dphy_power_on,
	.power_off = mtk_mipi_csi_dphy_power_off,
	.owner = THIS_MODULE,
};

/*
 * The D-PHY sits in the sensor front end register window.  Its xlate does
 * not need to pass arguments: unlike the CD-PHY of later MediaTek SoCs,
 * there is no DPHY/CPHY mode selection to negotiate here, so the number of
 * lanes is a property of the port and comes from the DT.
 */
static struct phy *mtk_mipi_csi_dphy_xlate(struct device *dev,
					   const struct of_phandle_args *args)
{
	struct mtk_mipi_csi_dphy *dphy = dev_get_drvdata(dev);

	/*
	 * Mirror of the in-tree MT8365 SENINF xlate: with #phy-cells = <1>
	 * the mode arrives as a PHY cell, with #phy-cells = <0> it must
	 * already have come from the "phy-type" property.
	 */
	if (args && args->args_count) {
		if (args->args_count != 1) {
			dev_err(dev, "invalid number of arguments\n");
			return ERR_PTR(-EINVAL);
		}

		switch (args->args[0]) {
		case PHY_TYPE_DPHY:
			dphy->mode = DPHY_MODE;
			break;
		default:
			dev_err(dev, "Unsupported PHY type: %i\n",
				args->args[0]);
			return ERR_PTR(-EINVAL);
		}
	} else {
		dphy->mode = DPHY_MODE;
	}

	return dphy->phy;
}

static int mtk_mipi_csi_dphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_mipi_csi_dphy *dphy;
	struct phy_provider *provider;
	int ret;

	dphy = devm_kzalloc(dev, sizeof(*dphy), GFP_KERNEL);
	if (!dphy)
		return -ENOMEM;

	dphy->dev = dev;
	dev_set_drvdata(dev, dphy);

	/*
	 * The reg resource is optional for now, because the address in the
	 * DT is a placeholder.  Mapping it unconditionally would fail probe
	 * on a machine where the guessed address is not a valid aperture.
	 */
	ret = of_property_read_u32(dev->of_node, "#phy-cells",
				   &dphy->phy_cells);
	if (ret)
		return dev_err_probe(dev, ret, "missing #phy-cells\n");

	if (dphy->phy_cells > 1)
		return dev_err_probe(dev, -EINVAL,
				     "#phy-cells must be 0 or 1, got %u\n",
				     dphy->phy_cells);

	dphy->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dphy->regs)) {
		dev_warn(dev, "no register window, running with no PHY programming\n");
		dphy->regs = NULL;
		dphy->have_regs = false;
	} else {
		dphy->have_regs = true;
	}

	/*
	 * Optional, defaulting to 0 (CSI0, the 4-lane D-PHY that carries the
	 * main camera).  It is not in the shared mediatek,mt8365-csi-rx
	 * binding, so a node written against that binding still works: the
	 * driver simply programs the primary receiver.
	 */
	dphy->port = 0;
	of_property_read_u32(dev->of_node, "mediatek,mipi-csi-port",
			     &dphy->port);

	/*
	 * When #phy-cells is 0 the mode cannot arrive in a PHY cell, so it
	 * must come from the "phy-type" property.  This mirrors the in-tree
	 * MT8365 driver, where the property is only consulted when the node
	 * declares #phy-cells = <0>.
	 */
	if (dphy->phy_cells == 0) {
		u32 phy_type;

		ret = of_property_read_u32(dev->of_node, "phy-type",
					   &phy_type);
		if (ret)
			return dev_err_probe(dev, ret,
					     "#phy-cells is 0 so phy-type is required\n");

		switch (phy_type) {
		case PHY_TYPE_DPHY:
			dphy->mode = DPHY_MODE;
			break;
		default:
			return dev_err_probe(dev, -EINVAL,
					     "Unsupported PHY type: %u\n",
					     phy_type);
		}
	}

	ret = of_property_read_u32(dev->of_node, "num-lanes", &dphy->num_lanes);
	if (ret)
		return dev_err_probe(dev, ret, "missing num-lanes\n");

	if (!dphy->num_lanes ||
	    dphy->num_lanes > MT6589_MIPI_CSI_DPHY_MAX_LANES)
		return dev_err_probe(dev, -EINVAL,
				     "num-lanes %u out of range (1..%u)\n",
				     dphy->num_lanes,
				     MT6589_MIPI_CSI_DPHY_MAX_LANES);

	/* CSI1 is the 1-lane interface; CSI0 is the 4-lane one. */
	if (dphy->port == 0 && dphy->num_lanes > MT6589_MIPI_CSI_DPHY_MAX_LANES)
		return dev_err_probe(dev, -EINVAL,
				     "CSI0 supports at most %u lanes\n",
				     MT6589_MIPI_CSI_DPHY_MAX_LANES);
	if (dphy->port == 1 && dphy->num_lanes != 1)
		return dev_err_probe(dev, -EINVAL,
				     "CSI1 is a 1-lane interface\n");

	dphy->phy = devm_phy_create(dev, NULL, &mtk_mipi_csi_dphy_ops);
	if (IS_ERR(dphy->phy))
		return dev_err_probe(dev, PTR_ERR(dphy->phy),
				     "Failed to create PHY\n");

	phy_set_drvdata(dphy->phy, dphy);

	provider = devm_of_phy_provider_register(dev,
						mtk_mipi_csi_dphy_xlate);
	if (IS_ERR(provider))
		return PTR_ERR(provider);

	return 0;
}

static const struct of_device_id mtk_mipi_csi_dphy_of_match[] = {
	{ .compatible = "mediatek,mt6589-mipi-csi-dphy" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_mipi_csi_dphy_of_match);

static struct platform_driver mtk_mipi_csi_dphy_driver = {
	.probe = mtk_mipi_csi_dphy_probe,
	.driver = {
		.name = "mtk-mipi-csi-dphy-mt6589",
		.of_match_table = mtk_mipi_csi_dphy_of_match,
	},
};
module_platform_driver(mtk_mipi_csi_dphy_driver);

MODULE_DESCRIPTION("MediaTek MT6589 MIPI CSI-2 D-PHY (stub)");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");