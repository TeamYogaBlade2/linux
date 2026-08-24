// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6320 PMIC analog audio codec.
 *
 * Analog audio back-end (DAC, headphone driver) of the MT6320 PMIC,
 * reached over the parent mt6397 MFD pwrap regmap.  The SoC-side AFE
 * (mt6589 AFE driver) and the sound card are separate drivers.
 *
 * Register map follows the MT6589 BSP AudDrv_ANA: the analog blocks sit
 * at 0x0700.. (AUDBUF/ZCD) and the ABB AFE bridge at 0x4000.., which
 * differs from the MT6323 layout.  Audio clocks come from CCF through
 * the mt6320-clk provider instead of direct TOP_CKPDN poking.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#define MT6320_CODEC_RATES	SNDRV_PCM_RATE_8000_48000
#define MT6320_CODEC_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
				 SNDRV_PCM_FMTBIT_S24_LE | \
				 SNDRV_PCM_FMTBIT_S32_LE)

/*
 * Audio registers in the PMIC 16-bit space: ABB_AFE<->PMIC bridge @ 0x4000,
 * AUDTOP analog DAC/headphone block @ 0x0700.
 */
#define MT6320_ABB_AFE_CON(n)		(0x4000 + (n) * 2)
#define MT6320_AUDTOP_CON(n)		(0x0700 + (n) * 2)
#define MT6320_ABB_AFE_UP8X_FIFO_CFG0	0x401e
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG0	0x4024
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG1	0x4026
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG2	0x4028
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG3	0x402a

/* ZCD output gain block (different offsets from the MT6323!). */
#define MT6320_ZCD_CON1			0x073a	/* lineout L/R gain */
#define MT6320_ZCD_CON2			0x073c	/* headphone L/R gain */
#define ZCD_GAIN_0DB			8
#define ZCD_GAIN_CTL_MAX		0x12	/* +8dB .. -10dB */
#define ZCD_GAIN_REG(g)			(((g) << 7) | (g))

struct mt6320_codec_priv {
	struct device *dev;
	struct regmap *regmap;		/* borrowed from the parent MFD */
	struct clk *clk_aud26m;		/* codec master clock via CCF */
};

/* Analog idle baseline from the stock power-on sequence. */
static const struct reg_sequence mt6320_codec_init[] = {
	{ MT6320_ABB_AFE_CON(1),  0x0009 },
	{ MT6320_ABB_AFE_CON(3),  0x0221 },
	{ MT6320_ABB_AFE_CON(4),  0x0255 },
	{ MT6320_ABB_AFE_CON(5),  0x0028 },
	{ MT6320_ABB_AFE_CON(6),  0x0218 },
	{ MT6320_ABB_AFE_CON(7),  0x0204 },
	{ MT6320_ABB_AFE_CON(10), 0x0001 },
	/* NewIF serial link to the SoC AFE (up8x FIFO + DL/UL config). */
	{ MT6320_ABB_AFE_UP8X_FIFO_CFG0,  0x0001 },
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG0, 0x7330 },	/* DL rate<<12|0x330 */
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG1, 0x0018 },
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG2, 0x302f },	/* UL up8x rxif ADC */
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG3, 0xf872 },
	/* Conservative default analog gains: headphone 0dB, lineout -10dB. */
	{ MT6320_ZCD_CON1, ZCD_GAIN_REG(ZCD_GAIN_0DB - 10 + 18) },
	{ MT6320_ZCD_CON2, ZCD_GAIN_REG(ZCD_GAIN_0DB) },
};

/* Codec master clock gating handled by CCF (aud26m gate in mt6320-clk). */
static int mt6320_dac_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(5), 0x0014);
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(0), 0x7010);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(0), 0x6010);
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(5), 0x0014);
		break;
	}
	return 0;
}

static int mt6320_hp_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(6), 0xf5ba);
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(4), 0x007c);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(4), 0x0000);
		regmap_write(priv->regmap, MT6320_AUDTOP_CON(6), 0x37e2);
		break;
	}
	return 0;
}

/* NEWIF serial link to the SoC AFE. */
static int mt6320_newif_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		regmap_write(priv->regmap, MT6320_ABB_AFE_CON(0),  0x0001);
		regmap_write(priv->regmap, MT6320_ABB_AFE_CON(11), 0x0303);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_write(priv->regmap, MT6320_ABB_AFE_CON(11), 0x0000);
		regmap_write(priv->regmap, MT6320_ABB_AFE_CON(0),  0x0000);
		break;
	}
	return 0;
}

static const struct snd_soc_dapm_widget mt6320_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("NEWIF", SND_SOC_NOPM, 0, 0, mt6320_newif_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, mt6320_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("HP Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6320_hp_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("Headphone"),
};

static const struct snd_soc_dapm_route mt6320_dapm_routes[] = {
	{ "DAC", NULL, "AIF1 Playback" },
	{ "DAC", NULL, "NEWIF" },
	{ "HP Driver", NULL, "DAC" },
	{ "Headphone", NULL, "HP Driver" },
};

/*
 * Output volume: -10dB .. +8dB in 1dB steps (the inverted ZCD gain field).
 * Same field layout as the MT6323, different register offsets.
 */
static const DECLARE_TLV_DB_SCALE(mt6320_dl_tlv, -1000, 100, 0);

static const struct snd_kcontrol_new mt6320_snd_controls[] = {
	SOC_DOUBLE_TLV("Headphone Volume",
		       MT6320_ZCD_CON2, 0, 7, ZCD_GAIN_CTL_MAX, 1,
		       mt6320_dl_tlv),
	SOC_DOUBLE_TLV("Lineout Volume",
		       MT6320_ZCD_CON1, 0, 7, ZCD_GAIN_CTL_MAX, 1,
		       mt6320_dl_tlv),
};

static int mt6320_component_probe(struct snd_soc_component *component)
{
	struct mt6320_codec_priv *priv = snd_soc_component_get_drvdata(component);

	/* Route mixer controls to the PMIC regmap (see mt6323.c note). */
	snd_soc_component_init_regmap(component, priv->regmap);
	return 0;
}

static const struct snd_soc_component_driver mt6320_soc_component_driver = {
	.probe			= mt6320_component_probe,
	.controls		= mt6320_snd_controls,
	.num_controls		= ARRAY_SIZE(mt6320_snd_controls),
	.dapm_widgets		= mt6320_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(mt6320_dapm_widgets),
	.dapm_routes		= mt6320_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(mt6320_dapm_routes),
	.endianness		= 1,
};

static struct snd_soc_dai_driver mt6320_dai_driver[] = {
	{
		.name = "mt6320-snd-codec-aif1",
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT6320_CODEC_RATES,
			.formats = MT6320_CODEC_FORMATS,
		},
	},
};

static int mt6320_codec_probe(struct platform_device *pdev)
{
	struct mt6397_chip *pmic = dev_get_drvdata(pdev->dev.parent);
	struct mt6320_codec_priv *priv;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = pmic->regmap;
	platform_set_drvdata(pdev, priv);

	/*
	 * Codec master clock through the CCF: aud26m is provided by
	 * mt6320-clk and gates the TOP_CKPDN AUD_26M bit internally.
	 */
	priv->clk_aud26m = devm_clk_get_enabled(&pdev->dev, "aud26m");
	if (IS_ERR(priv->clk_aud26m))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->clk_aud26m),
				     "failed to get aud26m clock\n");

	/* Analog + NEWIF idle baseline; DAPM powers the path per stream. */
	ret = regmap_multi_reg_write(priv->regmap, mt6320_codec_init,
				     ARRAY_SIZE(mt6320_codec_init));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to init analog codec\n");

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &mt6320_soc_component_driver,
					      mt6320_dai_driver,
					      ARRAY_SIZE(mt6320_dai_driver));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to register component\n");

	return 0;
}

static const struct of_device_id mt6320_codec_of_match[] = {
	{ .compatible = "mediatek,mt6320-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6320_codec_of_match);

static struct platform_driver mt6320_codec_driver = {
	.driver = {
		.name = "mt6320-sound",
		.of_match_table = mt6320_codec_of_match,
	},
	.probe = mt6320_codec_probe,
};
module_platform_driver(mt6320_codec_driver);

MODULE_DESCRIPTION("MediaTek MT6320 PMIC audio codec");
MODULE_LICENSE("GPL");
