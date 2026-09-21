// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6320 PMIC analog audio codec.
 *
 * Analog audio back-end (DAC, headphone driver) of the MT6320 PMIC,
 * reached over the parent mt6397 MFD pwrap regmap.  The SoC-side AFE
 * (mt6589 AFE driver) and the sound card are separate drivers.
 *
 * Register map follows the MT6589 BSP AudDrv_ANA: the analog blocks sit
 * at 0x0700.. (AUDBUF/ZCD) and the ABB AFE bridge at 0x2000.., which
 * differs from the MT6323 layout.  Audio clocks come from CCF through
 * the mt6320-clk provider instead of direct TOP_CKPDN poking.
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/mfd/mt6320/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#define MT6320_CODEC_RATES	SNDRV_PCM_RATE_8000_48000
#define MT6320_CODEC_FORMATS	SNDRV_PCM_FMTBIT_S16_LE

/*
 * Audio registers in the PMIC 16-bit space: ABB_AFE<->PMIC bridge @ 0x2000,
 * AUDTOP analog DAC/headphone block @ 0x0700.
 */
#define MT6320_ABB_AFE_CON(n)		(0x2000 + (n) * 2)
#define MT6320_AUDTOP_CON(n)		(0x0700 + (n) * 2)
#define MT6320_ABB_AFE_DL_SRC2_CON0_H	0x2002
#define MT6320_ABB_AFE_DL_SRC2_CON0_H_RATE	GENMASK(3, 0)
#define MT6320_ABB_AFE_UP8X_FIFO_CFG0	0x202c
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG0	0x2038
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG1	0x203a
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG2	0x203c
#define MT6320_ABB_AFE_PMIC_NEWIF_CFG3	0x203e

#define MT6320_CID			0x0100
#define MT6320_AFUNC_AUD_CON2		0x2020

#define MT6320_AUDDAC_CON0		0x0700
#define MT6320_AUDBUF_CFG0		0x0702
#define MT6320_AUDBUF_CFG1		0x0704
#define MT6320_AUDBUF_CFG2		0x0706
#define MT6320_AUDBUF_CFG3		0x0708
#define MT6320_AUDBUF_CFG4		0x070a
#define MT6320_IBIASDIST_CFG0		0x070c
#define MT6320_AUDACCDEPOP_CFG0	0x070e
#define MT6320_AUD_IV_CFG0		0x0710
#define MT6320_AUDCLKGEN_CFG0		0x0712
#define MT6320_AUDLDO_CFG0		0x0714
#define MT6320_AUDNVREGGLB_CFG0	0x0718
#define MT6320_AUD_NCP0			0x071a
#define MT6320_ZCD_CON0			0x0738
#define MT6320_NCP_CLKDIV_CON0		0x0744
#define MT6320_NCP_CLKDIV_CON1		0x0746

#define MT6320_PMIC_TRIM_ADDRESS1	0x01c2
#define MT6320_PMIC_TRIM_ADDRESS2	0x01c4
#define MT6320_PMIC_TRIM_REG1_DEFAULT	0x0220
#define MT6320_PMIC_TRIM_REG2_DEFAULT	0x0006
#define MT6320_E2_CID			0x2020
#define MT6320_PMIC_TRIM_SPK		0x01ca
#define MT6320_SPK_AUTO_TRIM_CTRL	0x013a
#define MT6320_SPK_AUTO_TRIM		0x014e
#define MT6320_SPK_TRIM_DEFAULT		0x0010
#define MT6320_SPK_CON0			0x0600
#define MT6320_SPK_CON1			0x0602
#define MT6320_SPK_CON2			0x0604
#define MT6320_SPK_CON9			0x0612
#define MT6320_SPK_CON11		0x0616

/* ZCD output gain block (different offsets from the MT6323!). */
#define MT6320_ZCD_CON1			0x073a	/* lineout L/R gain */
#define MT6320_ZCD_CON2			0x073c	/* headphone L/R gain */
#define MT6320_ZCD_CON3			0x073e	/* handset gain */
#define MT6320_ZCD_CON4			0x0740	/* IV buffer gain */
#define ZCD_GAIN_0DB			8
#define ZCD_GAIN_CTL_MAX		0x0c	/* +8dB .. -4dB */
#define ZCD_GAIN_REG(g)			(((g) << 8) | (g))

struct mt6320_codec_priv {
	struct device *dev;
	struct regmap *regmap;		/* borrowed from the parent MFD */
	struct clk *clk_aud26m;		/* codec master clock via CCF */
};

static int mt6320_newif_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return 0;
	case 11025:
		return 1;
	case 12000:
		return 2;
	case 16000:
		return 3;
	case 22050:
		return 4;
	case 24000:
		return 5;
	case 32000:
		return 6;
	case 44100:
		return 7;
	case 48000:
		return 8;
	default:
		return -EINVAL;
	}
}

static int mt6320_codec_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params,
				  struct snd_soc_dai *dai)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(dai->component);
	unsigned int rate = params_rate(params);
	int rate_code;
	int ret;

	rate_code = mt6320_newif_rate_code(rate);
	if (rate_code < 0)
		return rate_code;

	ret = regmap_update_bits(priv->regmap,
				 MT6320_ABB_AFE_PMIC_NEWIF_CFG0,
				 GENMASK(15, 12),
				 rate_code << 12);
	if (ret)
		return ret;

	return regmap_update_bits(priv->regmap,
				  MT6320_ABB_AFE_DL_SRC2_CON0_H,
				  MT6320_ABB_AFE_DL_SRC2_CON0_H_RATE,
				  FIELD_PREP(MT6320_ABB_AFE_DL_SRC2_CON0_H_RATE,
					     rate_code));
}

static const struct snd_soc_dai_ops mt6320_dai_ops = {
	.hw_params = mt6320_codec_hw_params,
};

static int mt6320_apply_hp_trim(struct mt6320_codec_priv *priv)
{
	u32 reg1, reg2, trim;
	int ret;

	ret = regmap_read(priv->regmap, MT6320_PMIC_TRIM_ADDRESS1, &reg1);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, MT6320_PMIC_TRIM_ADDRESS2, &reg2);
	if (ret)
		return ret;

	if (!(reg1 & BIT(12))) {
		reg1 = MT6320_PMIC_TRIM_REG1_DEFAULT & 0xfff0;
		reg2 = MT6320_PMIC_TRIM_REG2_DEFAULT & 0x0fff;
	}

	trim = BIT(8) |
		FIELD_PREP(GENMASK(12, 11),
			   ((reg1 >> 15) & 0x1) | ((reg2 & 0x1) << 1)) |
		FIELD_PREP(GENMASK(10, 9), (reg1 >> 13) & 0x3) |
		FIELD_PREP(GENMASK(7, 4), (reg1 >> 8) & 0xf) |
		FIELD_PREP(GENMASK(3, 0), (reg1 >> 4) & 0xf);

	return regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG3,
				  GENMASK(12, 0), trim);
}

static int mt6320_analog_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	unsigned int cid;
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					 BIT(7), BIT(7));
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AUDLDO_CFG0, 0x0d92);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDNVREGGLB_CFG0, 0x000c);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUD_NCP0, 0xe000);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_NCP_CLKDIV_CON0, 0x102b);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_NCP_CLKDIV_CON1, 0x0000);
		if (ret)
			return ret;

		usleep_range(900, 1100);
		return 0;

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_NCP_CLKDIV_CON1, 0x0001);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, MT6320_AUD_NCP0,
					 GENMASK(14, 13), 0);
		if (ret)
			return ret;

		ret = regmap_read(priv->regmap, MT6320_CID, &cid);
		if (ret)
			return ret;

		if (cid >= MT6320_E2_CID) {
			ret = regmap_write(priv->regmap,
					   MT6320_AUDNVREGGLB_CFG0, 0x0006);
			if (ret)
				return ret;
			ret = regmap_write(priv->regmap, MT6320_AUDLDO_CFG0, 0x0192);
		} else {
			ret = regmap_write(priv->regmap,
					   MT6320_AUDNVREGGLB_CFG0, 0x0004);
			if (ret)
				return ret;
			ret = regmap_write(priv->regmap, MT6320_AUDLDO_CFG0, 0x0992);
		}
		if (ret)
			return ret;

		return regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					  BIT(7), 0);
	}

	return 0;
}

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
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG0, 0x8330 },	/* 48 kHz idle rate */
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG1, 0x0018 },
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG2, 0x302f },	/* UL up8x rxif ADC */
	{ MT6320_ABB_AFE_PMIC_NEWIF_CFG3, 0xf872 },
	/* Conservative default analog gain: headphone 0dB. */
	{ MT6320_ZCD_CON2, ZCD_GAIN_REG(ZCD_GAIN_0DB) },
};

/* Codec master clock gating handled by CCF (top-aud26m gate in mt6320-clk). */
static int mt6320_dac_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regmap_write(priv->regmap, 0x2014, 0x0000);
		if (ret)
			return ret;

		/*
		 * AFUNC_AUD_CON2 bit 7 is owned by the shared Analog DAPM
		 * supply.  Only touch the SDM/FIFO bits here.
		 */
		ret = regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					 GENMASK(3, 0), 0x0006);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, 0x201c, 0xc3a1);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					 GENMASK(3, 0), 0x0003);
		if (ret)
			return ret;

		ret = regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					 GENMASK(3, 0), 0x000b);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, 0x2008, 0x001e);
		if (ret)
			return ret;

		ret = regmap_set_bits(priv->regmap, 0x2000, BIT(0));
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, 0x2004, 0x1801);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, 0x2012, 0x0000);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AUDTOP_CON(5), 0x0014);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_AUDTOP_CON(0), 0x7010);

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_AUDTOP_CON(0), 0x6010);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_AUDTOP_CON(5), 0x0014);
	}
	return 0;
}

static int mt6320_hp_event(struct snd_soc_dapm_widget *w,
			   struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = mt6320_apply_hp_trim(priv);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x0008);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON0, 0x0101);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x0008);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_IBIASDIST_CFG0, 0x0552);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG1, 0x0900);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG2, 0x0082);
		if (ret)
			return ret;

		usleep_range(29000, 31000);

		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x0009);
		if (ret)
			return ret;

		usleep_range(29000, 31000);

		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG1, 0x0940);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x000f);
		if (ret)
			return ret;

		usleep_range(29000, 31000);

		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG1, 0x0100);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG2, 0x0082);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON2, 0x0c0c);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDCLKGEN_CFG0,
					 BIT(0), BIT(0));
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDDAC_CON0, 0x000f);
		if (ret)
			return ret;

		usleep_range(29000, 31000);

		/*
		 * The stock HP sequence leaves the common AUDBUF mux in state 6
		 * before selecting the L/R DAC muxes.
		 */
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG0,
					 GENMASK(2, 0), 0x0006);
		if (ret)
			return ret;

		/* HP L/R mux: DAC. */
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG0,
					 GENMASK(7, 5), 4 << 5);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG0,
					 GENMASK(11, 9), 4 << 9);
		if (ret)
			return ret;

		return 0;

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON2, 0x0c0c);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG0,
					 GENMASK(12, 5), 0x0880);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG0,
					 GENMASK(2, 0), 0x0000);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_IBIASDIST_CFG0, 0x1552);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG1,
					 BIT(8), 0);
		if (ret)
			return ret;
		return 0;
	}
	return 0;
}

static int mt6320_apply_spk_trim(struct mt6320_codec_priv *priv)
{
	unsigned int cid, reg;
	unsigned int polarity, trim;
	int i, ret;

	ret = regmap_read(priv->regmap, MT6320_CID, &cid);
	if (ret)
		return ret;

	if (cid < MT6320_E2_CID) {
		ret = regmap_read(priv->regmap, MT6320_PMIC_TRIM_SPK, &reg);
		if (ret)
			return ret;

		if (!(reg & BIT(13)))
			reg = MT6320_SPK_TRIM_DEFAULT;

		polarity = FIELD_GET(BIT(12), reg);
		trim = FIELD_GET(GENMASK(11, 7), reg);
	} else {
		ret = regmap_write(priv->regmap, MT6320_SPK_CON9, 0x2018);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_SPK_CON0, 0x0008);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_SPK_CON0,
					 GENMASK(15, 12), 0x3000);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_SPK_CON9,
					 GENMASK(11, 8), 0x0a00);
		if (ret)
			return ret;
		ret = regmap_set_bits(priv->regmap, MT6320_SPK_CON0, BIT(0));
		if (ret)
			return ret;

		for (i = 0; i < 10; i++) {
			ret = regmap_read(priv->regmap, MT6320_SPK_CON1, &reg);
			if (ret)
				goto trim_stop;
			if (reg & BIT(15))
				break;
			msleep(10);
		}

		if (i == 10) {
			ret = -ETIMEDOUT;
			goto trim_stop;
		}

		ret = regmap_write(priv->regmap, MT6320_SPK_AUTO_TRIM_CTRL,
				   0x0802);
		if (ret)
			goto trim_stop;
		ret = regmap_read(priv->regmap, MT6320_SPK_AUTO_TRIM, &reg);
		if (ret)
			goto trim_stop;

		polarity = FIELD_GET(BIT(9), reg);
		trim = FIELD_GET(GENMASK(14, 10), reg);

trim_stop:
		regmap_write(priv->regmap, MT6320_SPK_CON9, 0x0000);
		regmap_clear_bits(priv->regmap, MT6320_SPK_CON0, BIT(0));
		if (ret)
			return ret;
	}

	return regmap_update_bits(priv->regmap, MT6320_SPK_CON1,
				  0x7f00,
				  BIT(14) |
				  FIELD_PREP(BIT(13), polarity) |
				  FIELD_PREP(GENMASK(12, 8), trim));
}

static int mt6320_speaker_event(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = mt6320_apply_spk_trim(priv);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ZCD_CON0, 0x0301);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDACCDEPOP_CFG0, 0x0030);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x0008);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_IBIASDIST_CFG0, 0x0552);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON2, 0x0c0c);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON3, 0x000f);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG1, 0x0900);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG2, 0x0082);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x0009);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG1, 0x0940);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG0, 0x0007);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG1, 0x0000);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG2, 0x0022);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON2, 0x0505);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_ZCD_CON4, 0x0505);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUD_IV_CFG0, 0x0011);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDCLKGEN_CFG0,
					 BIT(0), BIT(0));
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_AUDDAC_CON0, 0x0009);
		if (ret)
			return ret;

		usleep_range(900, 1100);

		/* Speaker mux: DAC through IV buffer. */
		ret = regmap_update_bits(priv->regmap, MT6320_AUD_IV_CFG0,
					 GENMASK(4, 2), 4 << 2);
		if (ret)
			return ret;
		ret = regmap_update_bits(priv->regmap, MT6320_AUDBUF_CFG0,
					 GENMASK(2, 0), 0);
		if (ret)
			return ret;

		usleep_range(900, 1100);

		ret = regmap_write(priv->regmap, MT6320_SPK_CON0, 0x3009);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_SPK_CON2, 0x0014);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_SPK_CON9, 0x0800);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_SPK_CON11, 0x0f00);

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_SPK_CON11, 0x0000);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_SPK_CON9, 0x0000);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_SPK_CON0, 0x0000);
		if (ret)
			return ret;

		return regmap_clear_bits(priv->regmap, MT6320_AUD_IV_CFG0, BIT(0));
	}

	return 0;
}

/* NEWIF serial link to the SoC AFE. */
static int mt6320_newif_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regmap_write(priv->regmap, MT6320_ABB_AFE_CON(0),
				   0x0001);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_ABB_AFE_CON(11),
				    0x0303);

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_ABB_AFE_CON(11),
				   0x0000);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_ABB_AFE_CON(0),
				    0x0000);
	}

	return 0;
}

static const struct snd_soc_dapm_widget mt6320_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Analog", SND_SOC_NOPM, 0, 0,
			    mt6320_analog_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("NEWIF", SND_SOC_NOPM, 0, 0, mt6320_newif_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, mt6320_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("HP Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6320_hp_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Speaker Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6320_speaker_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("Headphone"),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route mt6320_dapm_routes[] = {
	{ "DAC", NULL, "AIF1 Playback" },
	{ "DAC", NULL, "NEWIF" },
	{ "HP Driver", NULL, "DAC" },
	{ "HP Driver", NULL, "Analog" },
	{ "Headphone", NULL, "HP Driver" },
	{ "Speaker Driver", NULL, "DAC" },
	{ "Speaker Driver", NULL, "Analog" },
	{ "Speaker", NULL, "Speaker Driver" },
};

/* Output volume: -4dB .. +8dB in 1dB steps. */
static const DECLARE_TLV_DB_SCALE(mt6320_dl_tlv, -400, 100, 0);

static const struct snd_kcontrol_new mt6320_snd_controls[] = {
	SOC_DOUBLE_TLV("Headphone Volume",
		       MT6320_ZCD_CON2, 0, 8, ZCD_GAIN_CTL_MAX, 1,
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
		.ops = &mt6320_dai_ops,
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

	if (!pmic || !pmic->regmap)
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "missing PMIC regmap\n");

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	priv->regmap = pmic->regmap;
	platform_set_drvdata(pdev, priv);

	/*
	 * Codec master clock through the CCF: the top-aud26m gate of
	 * mt6320-clk (TOP_CKPDN bit 0), fed by the aud26m fixed clock.
	 */
	priv->clk_aud26m = devm_clk_get_enabled(&pdev->dev, "top-aud26m");
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
