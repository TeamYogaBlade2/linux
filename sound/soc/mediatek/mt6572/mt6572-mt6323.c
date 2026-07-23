// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6572 + MT6323 sound card.
 *
 * Machine driver binding the MT6572 AFE platform (DL1 playback) to the MT6323
 * PMIC analog codec, with headphone-jack detection that auto-routes between the
 * speaker and the headphones, and an optional external speaker amplifier.
 * Modelled on the mt8183-mt6358 PMIC-codec card.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include "../../codecs/mt6323-accdet.h"
#include "../common/mtk-soundcard-driver.h"
#include "../common/mtk-soc-card.h"

#include <sound/jack.h>
#include <sound/soc.h>

static const struct snd_soc_dapm_widget mt6572_mt6323_widgets[] = {
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_SPK("Earpiece", NULL),
};

SND_SOC_DAILINK_DEFS(playback,
	DAILINK_COMP_ARRAY(COMP_CPU("mt6572-afe-dl1")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6323-sound", "mt6323-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6572_mt6323_dai_links[] = {
	{
		.name = "DL1",
		.stream_name = "DL1 Playback",
		SND_SOC_DAILINK_REG(playback),
	},
};

static struct snd_soc_jack mt6572_mt6323_hp_jack;

static struct snd_soc_jack_pin mt6572_mt6323_jack_pins[] = {
	{ .pin = "Headphone", .mask = SND_JACK_HEADPHONE },
	{ .pin = "Speaker", .mask = SND_JACK_HEADPHONE, .invert = 1 },
};

static int mt6572_mt6323_accdet_init(struct snd_soc_pcm_runtime *rtd)
{
	struct mtk_soc_card_data *soc_card_data = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	if (!soc_card_data->accdet)
		return 0;

	ret = snd_soc_card_jack_new_pins(rtd->card, "Headphone Jack",
				   SND_JACK_HEADSET,
				   &mt6572_mt6323_hp_jack, mt6572_mt6323_jack_pins,
				   ARRAY_SIZE(mt6572_mt6323_jack_pins));
	if (ret) {
		dev_err(rtd->dev, "Headphone Jack create failed: %d\n", ret);
		return ret;
	}

	ret = mt6323_accdet_enable_jack_detect(soc_card_data->accdet, &mt6572_mt6323_hp_jack);
	if (ret) {
		dev_err(rtd->dev, "Headphone Jack enable failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int mt6572_mt6323_soc_card_probe(struct mtk_soc_card_data *soc_card_data,
					bool legacy)
{
	struct snd_soc_card *card = soc_card_data->card_data->card;
	struct snd_soc_dai_link *dai_link;
	int i;

	if (legacy)
		return -EINVAL;

	for_each_card_prelinks(card, i, dai_link) {
		if (strcmp(dai_link->name, "DL1") == 0) {
			dai_link->init = mt6572_mt6323_accdet_init;
			break;
		}
	}

	return 0;
}

static struct snd_soc_card mt6572_mt6323_card = {
	.name = "mt6572-mt6323",
	.owner = THIS_MODULE,
	.dai_link = mt6572_mt6323_dai_links,
	.num_links = ARRAY_SIZE(mt6572_mt6323_dai_links),
	.dapm_widgets = mt6572_mt6323_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6572_mt6323_widgets),
};

static const struct mtk_soundcard_pdata mt6572_card = {
	.card_name = "mt6572_mt6323",
	.card_data = &(struct mtk_platform_card_data) {
		.card = &mt6572_mt6323_card,
		.num_jacks = 1,
	},
	.soc_probe = mt6572_mt6323_soc_card_probe,
};

static const struct of_device_id mt6572_mt6323_dt_match[] = {
	{ .compatible = "mediatek,mt6572-mt6323-sound", .data = &mt6572_card },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6572_mt6323_dt_match);

static struct platform_driver mt6572_mt6323_driver = {
	.driver = {
		.name = "mt6572-mt6323",
		.of_match_table = mt6572_mt6323_dt_match,
	},
	.probe = mtk_soundcard_common_probe,
};
module_platform_driver(mt6572_mt6323_driver);

MODULE_DESCRIPTION("MediaTek MT6572 MT6323 sound card");
MODULE_LICENSE("GPL");
