// SPDX-License-Identifier: GPL-2.0
/*
 * MT6589 display backlight through the BLS PWM (Lenovo B8000-H).
 *
 * The vendor kernel (mediatek/platform/mt6589/kernel/drivers/dispsys/ddp_bls.c,
 * cust_leds.c: MT65XX_LED_MODE_CUST_BLS_PWM, div 0x47) keeps the BLS in
 * "PWM only" mode (BLS_EN = 0x80000000) and sets the brightness as the duty
 * 0..255 in BLS_PWM_DUTY.  LK already programs BLS_EN/PWM_CON, so this driver
 * only touches the duty (and the PWM idle level for 0).  The BLS registers
 * belong to the mediatek-drm BLS component: map them without claiming.
 */

#include <linux/backlight.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#define BLS_EN			0x00
#define BLS_EN_PWM		BIT(31)
#define BLS_PWM_DUTY		0x80
#define BLS_PWM_CON		0x90
#define BLS_PWM_CON_IDLE_HIGH	BIT(7)
#define BLS_PWM_CON_DEF		0x00050000
#define BLS_PWM_DIV_DEF		0x47
#define BLS_MAX_LEVEL		255

struct mt6589_bls_bl {
	void __iomem *base;
};

static int mt6589_bls_bl_update(struct backlight_device *bl)
{
	struct mt6589_bls_bl *priv = bl_get_data(bl);
	int level = backlight_get_brightness(bl);
	u32 con = readl(priv->base + BLS_PWM_CON);

	if (!(readl(priv->base + BLS_EN) & BLS_EN_PWM)) {
		writel(BLS_PWM_CON_DEF | BLS_PWM_DIV_DEF, priv->base + BLS_PWM_CON);
		writel(BLS_EN_PWM, priv->base + BLS_EN);
		con = readl(priv->base + BLS_PWM_CON);
	}

	/* Vendor: idle level low when off, high otherwise. */
	if (level)
		con |= BLS_PWM_CON_IDLE_HIGH;
	else
		con &= ~BLS_PWM_CON_IDLE_HIGH;
	writel(con, priv->base + BLS_PWM_CON);
	writel(level, priv->base + BLS_PWM_DUTY);

	return 0;
}

static const struct backlight_ops mt6589_bls_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = mt6589_bls_bl_update,
};

static int mt6589_bls_bl_probe(struct platform_device *pdev)
{
	struct backlight_properties props = {
		/* GNOME ignores RAW backlights not parented by a DRM connector */
		.type = BACKLIGHT_PLATFORM,
		.max_brightness = BLS_MAX_LEVEL,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct device *dev = &pdev->dev;
	struct backlight_device *bl;
	struct mt6589_bls_bl *priv;
	struct device_node *np;
	u32 duty;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	np = dev->of_node ? of_parse_phandle(dev->of_node, "mediatek,bls", 0) :
	     of_find_compatible_node(NULL, NULL, "mediatek,mt6589-disp-bls");
	if (!np)
		return dev_err_probe(dev, -ENODEV, "no BLS node\n");
	priv->base = of_iomap(np, 0);
	of_node_put(np);
	if (!priv->base)
		return -ENOMEM;

	/* Keep what LK set so the screen does not flash at probe. */
	duty = readl(priv->base + BLS_PWM_DUTY) & 0x3ff;
	props.brightness = min_t(u32, duty ? duty : BLS_MAX_LEVEL, BLS_MAX_LEVEL);

	bl = devm_backlight_device_register(dev, "mt6589-bls", dev, priv,
					    &mt6589_bls_bl_ops, &props);
	if (IS_ERR(bl)) {
		iounmap(priv->base);
		return PTR_ERR(bl);
	}
	platform_set_drvdata(pdev, priv);

	return 0;
}

static void mt6589_bls_bl_remove(struct platform_device *pdev)
{
	struct mt6589_bls_bl *priv = platform_get_drvdata(pdev);

	iounmap(priv->base);
}

static const struct of_device_id mt6589_bls_bl_of_match[] = {
	{ .compatible = "mediatek,mt6589-bls-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6589_bls_bl_of_match);

static struct platform_driver mt6589_bls_bl_driver = {
	.probe = mt6589_bls_bl_probe,
	.remove = mt6589_bls_bl_remove,
	.driver = {
		.name = "mt6589-bls-backlight",
		.of_match_table = mt6589_bls_bl_of_match,
	},
};

#ifdef MODULE
/* Out-of-tree test without the DT node: create the device ourselves. */
static struct platform_device *mt6589_bls_bl_pdev;

static int __init mt6589_bls_bl_init(void)
{
	struct device_node *np;
	int ret;

	ret = platform_driver_register(&mt6589_bls_bl_driver);
	if (ret)
		return ret;

	np = of_find_matching_node(NULL, mt6589_bls_bl_of_match);
	of_node_put(np);
	if (!np) {
		mt6589_bls_bl_pdev = platform_device_register_simple(
			"mt6589-bls-backlight", PLATFORM_DEVID_NONE, NULL, 0);
		if (IS_ERR(mt6589_bls_bl_pdev)) {
			platform_driver_unregister(&mt6589_bls_bl_driver);
			return PTR_ERR(mt6589_bls_bl_pdev);
		}
	}
	return 0;
}
module_init(mt6589_bls_bl_init);

static void __exit mt6589_bls_bl_exit(void)
{
	platform_device_unregister(mt6589_bls_bl_pdev);
	platform_driver_unregister(&mt6589_bls_bl_driver);
}
module_exit(mt6589_bls_bl_exit);
#else
module_platform_driver(mt6589_bls_bl_driver);
#endif

MODULE_DESCRIPTION("MT6589 BLS PWM backlight");
MODULE_LICENSE("GPL");
