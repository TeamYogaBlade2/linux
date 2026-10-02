// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 + MT6320 sound card.
 *
 * Machine driver binding the MT6589 AFE platform (DL1 playback) to the MT6320 PMIC analog codec, with headphone-jack detection that auto-routes between the
 * speaker and the headphones, and an optional external speaker amplifier.
 * Modelled on the mt8183-mt6358 PMIC-codec card.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/string.h>

#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-jack.h>

SND_SOC_DAILINK_DEFS(playback,
	DAILINK_COMP_ARRAY(COMP_CPU("mt6589-afe-dl1")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6320-sound", "mt6320-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture,
	DAILINK_COMP_ARRAY(COMP_CPU("mt6589-afe-vul")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6320-sound", "mt6320-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6589_mt6320_dai_links[] = {
	{
		.name = "DL1",
		.stream_name = "DL1 Playback",
		SND_SOC_DAILINK_REG(playback),
	},
	{
		.name = "VUL Capture",
		.stream_name = "VUL Capture",
		SND_SOC_DAILINK_REG(capture),
	},
};

static struct snd_soc_jack mt6589_mt6320_hp_jack;

static struct snd_soc_jack_pin mt6589_mt6320_jack_pins[] = {
	{ .pin = "Headphone", .mask = SND_JACK_HEADPHONE },
	{ .pin = "Speaker", .mask = SND_JACK_HEADPHONE, .invert = 1 },
};

static int mt6589_mt6320_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_component *accdet = NULL;
	struct snd_soc_component *component;
	int ret;

	ret = snd_soc_card_jack_new_pins(card, "Headphone Jack", SND_JACK_HEADSET,
					 &mt6589_mt6320_hp_jack,
					 mt6589_mt6320_jack_pins,
					 ARRAY_SIZE(mt6589_mt6320_jack_pins));
	if (ret)
		return ret;

	ret = snd_jack_set_key(mt6589_mt6320_hp_jack.jack,
			       SND_JACK_BTN_0, KEY_PLAYPAUSE);
	if (ret)
		return ret;

	ret = snd_jack_set_key(mt6589_mt6320_hp_jack.jack,
			       SND_JACK_BTN_1, KEY_PREVIOUSSONG);
	if (ret)
		return ret;

	ret = snd_jack_set_key(mt6589_mt6320_hp_jack.jack,
			       SND_JACK_BTN_2, KEY_NEXTSONG);
	if (ret)
		return ret;

	/*
	 * Find the headset detector among this card's aux components.
	 *
	 * This must not use snd_soc_lookup_component_by_name().  That helper
	 * takes client_mutex, and late_probe runs with client_mutex already
	 * held: snd_soc_register_card() acquires it, then reaches
	 * snd_soc_card_late_probe() through snd_soc_bind_card(), so a lookup
	 * from here re-acquires a non-recursive mutex the caller already
	 * holds and the card deadlocks part-way through probe.  That is why
	 * enabling the AFE froze the boot: with the AFE node disabled the
	 * DAI lookup defers and late_probe is never reached.
	 *
	 * aux-devs are bound by soc_bind_aux_dev(), which runs in the same
	 * locked region but takes no lock of its own, so the component is
	 * already on card->aux_comp_list by the time we get here.
	 */
	for_each_card_auxs(card, component) {
		if (component->driver && component->driver->name &&
		    !strcmp(component->driver->name, "mt6320-accdet")) {
			accdet = component;
			break;
		}
	}

	if (!accdet) {
		dev_info(card->dev, "no headset detection support\n");
		return 0;
	}

	return snd_soc_component_set_jack(accdet, &mt6589_mt6320_hp_jack,
					  NULL);
}

static struct snd_soc_card mt6589_mt6320_card = {
	.name = "mt6589-mt6320",
	.owner = THIS_MODULE,
	.dai_link = mt6589_mt6320_dai_links,
	.num_links = ARRAY_SIZE(mt6589_mt6320_dai_links),
	.late_probe = mt6589_mt6320_late_probe,
};

static int mt6589_mt6320_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt6589_mt6320_card;
	struct device_node *platform_node;
	struct snd_soc_dai_link *dai_link;
	int i, ret;

	card->dev = &pdev->dev;

	platform_node = of_parse_phandle(pdev->dev.of_node, "mediatek,platform", 0);
	if (!platform_node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing mediatek,platform\n");

	/*
	 * Both links declare their CPU by name (COMP_CPU("mt6589-afe-dl1")),
	 * so leave the CPU component alone - adding of_node as well would make
	 * it invalid, since snd_soc_dlc_component_is_invalid() rejects a dlc
	 * that has both a name and a node.
	 *
	 * The platform component is declared COMP_EMPTY(), and that is fatal:
	 * the card fails to register with
	 *	ASoC: Neither Component name/of_node are set for DL1
	 * so fill it on every link.
	 *
	 * The codec is likewise found by the name the dai_link already
	 * declares, so no phandle is needed for it.
	 */
	for_each_card_prelinks(card, i, dai_link) {
		dai_link->platforms->of_node = platform_node;
	}

	if (of_property_present(pdev->dev.of_node, "audio-routing")) {
		ret = snd_soc_of_parse_audio_routing(card, "audio-routing");
		if (ret)
			goto put_node;
	}

	/* Optional external speaker amplifier(s) via "aux-devs". */
	ret = snd_soc_of_parse_aux_devs(card, "aux-devs");
	if (ret)
		goto put_node;

	ret = devm_snd_soc_register_card(&pdev->dev, card);
put_node:
	of_node_put(platform_node);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to set up sound card\n");

	return 0;
}

static const struct of_device_id mt6589_mt6320_dt_match[] = {
	{ .compatible = "mediatek,mt6589-mt6320-sound" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6589_mt6320_dt_match);

static struct platform_driver mt6589_mt6320_driver = {
	.driver = {
		.name = "mt6589-mt6320",
		.of_match_table = mt6589_mt6320_dt_match,
	},
	.probe = mt6589_mt6320_dev_probe,
};
module_platform_driver(mt6589_mt6320_driver);

MODULE_DESCRIPTION("MediaTek MT6589 MT6320 sound card");
MODULE_LICENSE("GPL");
