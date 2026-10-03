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

#include <linux/bitfield.h>
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
 * Audio registers in the PMIC 16-bit space: ABB_AFE<->PMIC bridge @ 0x4000,
 * AUDTOP analog DAC/headphone block @ 0x0700.
 */
#define MT6320_ABB_AFE_BASE		0x4000
#define MT6320_ABB_AFE_CON(n)		(MT6320_ABB_AFE_BASE + (n) * 2)
#define MT6320_ABB_AFE_DL_SRC2_CON0_H	(MT6320_ABB_AFE_CON(1))
/*
 * The sample rate lives in bits [15:12], not a low nibble. The stock
 * driver sets this register as 0x0300 | GetDLFrequency(rate), where
 * GetDLFrequency() returns the code shifted left by twelve: 48 kHz gives
 * 8 << 12, so the register becomes 0x8300.
 */
#define MT6320_ABB_AFE_DL_SRC2_CON0_H_RATE	GENMASK(15, 12)
#define MT6320_ABB_AFE_DL_SRC2_CON0_H_BASE	0x0300

#define MT6320_AFUNC_AUD_CON2		(MT6320_ABB_AFE_CON(0x1a))

/*
 * AUXADC channel select is CHSEL[10:7] (upmu_hw.h: RG_AUXADC_CHSEL mask 0xF,
 * shift 7).  Channel 5 is the accessory-detect key voltage, the same one the
 * IIO AUXADC driver uses; select it here so the audio mic path does not depend
 * on that driver having run first.
 */
#define MT6320_AUXADC_CON0_CHSEL	GENMASK(10, 7)
#define MT6320_AUXADC_CON0_CHSEL_ACCDET	0x5
/*
 * Mic bias / AUXADC switch.
 *
 * This board is built with ACCDET_28V_MODE (see the vendor
 * accdet_custom_def.h), where the downstream driver does not use the
 * ACCDET_RSV encoding at all: it drives AUDENCSPARE_CON0 (0x0732) to
 * 0x01 to enable the switch and 0x00 to disable it.  The 0x1090 value is the
 * 1.9 V path and selects RG_AUDACCDETVIN1PULLLOW, which is not what this
 * hardware wants.
 */
#define MT6320_AUDENCSPARE_CON0		0x0732
#define MT6320_ACCDET_MICBIAS_ENABLE	0x01
#define MT6320_ACCDET_MICBIAS_DISABLE	0x00

/*
 * Headphone amplifier trim lives in the efuse data-out words.  The
 * kernel's MT6320 register header still labels these
 * EFUSE_DOUT_112_127 / EFUSE_DOUT_128_143; name them for what they are
 * used for here.  Bit 12 of the first word is the "trim valid" flag.
 */
#define MT6320_EFUSE_DOUT_112_127	0x01c2
#define MT6320_EFUSE_DOUT_128_143	0x01c4
#define MT6320_PMIC_TRIM_REG1_DEFAULT	0x0220
#define MT6320_PMIC_TRIM_REG2_DEFAULT	0x0006
/*
 * PMIC chip ID read from MT6320_CID, not a register address:
 * PMIC6320_E1_CID_CODE / PMIC6320_E2_CID_CODE.  The E2 revision selects the
 * auto-trim path and the E2 regulator idle values.
 */
#define MT6320_E1_CID			0x1020
#define MT6320_E2_CID			0x2020
#define MT6320_EFUSE_DOUT_176_191	0x01ca
#define MT6320_SPK_TRIM_DEFAULT		0x0010
/*
 * SPK_CON1 carries the measured class-D offset on E2 silicon.  E1 reads
 * it from the trim efuse instead (see mt6320_apply_spk_trim()).
 */
#define MT6320_SPK_OFFSET_L_MODE	BIT(14)
#define MT6320_SPK_OFFSET_L_SW		GENMASK(12, 8)
/* SPK_CON11 is a one-hot software override selector, not a plain ramp. */
#define MT6320_SPK_OUTSTG_EN_L_SW	BIT(11)
#define MT6320_SPK_EN_L_SW		BIT(9)
#define MT6320_SPK_OUTSTG_EN_R_SW	BIT(10)
#define MT6320_SPK_EN_R_SW		BIT(8)
#define ZCD_GAIN_0DB			8
#define ZCD_GAIN_CTL_MAX		0x0c	/* +8dB .. -4dB */
#define ZCD_GAIN_REG(g)			(((g) << 8) | (g))

struct mt6320_codec_priv {
	struct device *dev;
	struct regmap *regmap;		/* borrowed from the parent MFD */
	struct clk *clk_aud26m;		/* codec master clock via CCF */
};

static int mt6320_dl_src_rate_code(unsigned int rate)
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

	rate_code = mt6320_dl_src_rate_code(rate);
	if (rate_code < 0)
		return rate_code;

	return regmap_update_bits(priv->regmap,
				  MT6320_ABB_AFE_DL_SRC2_CON0_H,
				  MT6320_ABB_AFE_DL_SRC2_CON0_H_RATE,
				  MT6320_ABB_AFE_DL_SRC2_CON0_H_BASE |
				  (rate_code << 12));
}

static const struct snd_soc_dai_ops mt6320_dai_ops = {
	.hw_params = mt6320_codec_hw_params,
};

static int mt6320_apply_hp_trim(struct mt6320_codec_priv *priv)
{
	u32 reg1, reg2, trim;
	int ret;

	ret = regmap_read(priv->regmap, MT6320_EFUSE_DOUT_112_127, &reg1);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, MT6320_EFUSE_DOUT_128_143, &reg2);
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
		/*
		 * Mute while reconfiguring, as the stock driver does, then
		 * clear it again at the end of this branch.
		 */
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
		ret = regmap_write(priv->regmap, MT6320_NCP_CLKDIV_CON0, 0x102b);
		if (ret)
			return ret;
		ret = regmap_write(priv->regmap, MT6320_NCP_CLKDIV_CON1, 0x0000);
		if (ret)
			return ret;

		usleep_range(900, 1100);
		return 0;

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_read(priv->regmap, MT6320_CID, &cid);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_NCP_CLKDIV_CON1, 0x0001);
		if (ret)
			return ret;

		/*
		 * Return NCP to the documented idle value for this silicon
		 * revision, which is also what power_init() leaves behind.
		 */
		ret = regmap_write(priv->regmap, MT6320_AUD_NCP0,
				   cid >= MT6320_E2_CID ? 0x8000 : 0x9000);
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

		/*
		 * Unmute.  AFUNC_AUD_CON2 bit 7 is a mute, not an enable: the
		 * stock driver asserts it while it configures the path and
		 * clears it once the configuration is done
		 * (AudioMachineDevice, SetAnalogReg(AFUNC_AUD_CON2, mute << 7)).
		 * Leaving it set keeps the output muted for as long as the
		 * path is powered - that is, for as long as it is playing.
		 */
		return regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					  BIT(7), 0);
	}

	return 0;
}

/*
 * Analog idle baseline, before any stream is running.  The ABB digital
 * path registers are programmed per-stream in mt6320_dac_event() rather
 * than here, since the stock driver does that from AnalogOpen() each time
 * the DAC path is opened and they depend on the sample rate.
 *
 * The values are the stock power-on sequence from
 * AudioPlatformDevice::AnalogOpen().  MT6320_ABB_AFE_CON(n) computes
 * 0x4000 + n * 2, which does not always land on the register the stock
 * driver means, so the entries name their registers explicitly.
 */
static const struct reg_sequence mt6320_codec_init[] = {
	{ MT6320_ABB_AFE_CON(1),  0x0009 },
	{ MT6320_ABB_AFE_CON(3),  0x0221 },
	{ MT6320_ABB_AFE_CON(4),  0x0255 },
	{ MT6320_ABB_AFE_CON(5),  0x0028 },
	{ MT6320_ABB_AFE_CON(6),  0x0218 },
	{ MT6320_ABB_AFE_CON(7),  0x0204 },
	{ MT6320_ABB_AFE_CON(10), 0x0001 },
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
		ret = regmap_write(priv->regmap, MT6320_TOP_CKPDN_CLR, 0x0003);
		if (ret)
			return ret;

		/*
		 * The digital path registers, following
		 * AudioPlatformDevice::AnalogOpen() for DEVICE_OUT_DAC.  The
		 * addresses are the vendor's, so each entry names its
		 * register explicitly: MT6320_ABB_AFE_CON(n) computes
		 * 0x4000 + n * 2, which lands elsewhere for some of these.
		 *
		 * 0x4000 (ANALDO_CON0) is the register that enables the
		 * digital path.  DAPM used to set only BIT(0) of it; the stock
		 * driver writes 0x007f, and the rest is what the chip needs to
		 * pass samples at all.
		 */
		ret = regmap_write(priv->regmap, MT6320_DIGLDO_CON12, 0xc3a1);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_DIGLDO_CON14, 0x0006);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_DIGLDO_CON14, 0x0003);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_DIGLDO_CON14, 0x000b);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ANALDO_CON6, 0x001e);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ANALDO_CON0, 0x007f);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ANALDO_CON2, 0x1801);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ANALDO_CON1, 0x0000);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ANALDO_CON9, 0x00e1);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_DIGLDO_CON3, 0x0000);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_DIGLDO_CON2, 0x004f);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AUDBUF_CFG4, 0x0014);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_AUDDAC_CON0, 0x7010);

	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_AUDDAC_CON0, 0x6010);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_AUDBUF_CFG4, 0x0014);
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
		/*
		 * Leave ZCD_CON2's volume fields alone.  Bits [3:0] and
		 * [11:8] are the headphone left/right volume indexes - the
		 * stock driver writes them only from its volume setter
		 * (AudioMachineDevice, masks 0x0000000f and 0x00000f00) - so
		 * a full-word 0x0c0c here both forced index 12, the maximum
		 * of +8 dB, and discarded whatever the "Headphone Volume"
		 * control had set.
		 */
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
	int i, ret, cleanup_ret;

	ret = regmap_read(priv->regmap, MT6320_CID, &cid);
	if (ret)
		return ret;

	if (cid < MT6320_E2_CID) {
		ret = regmap_read(priv->regmap, MT6320_EFUSE_DOUT_176_191, &reg);
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

		/*
		 * The trim engine has latched its result into SPK_CON1 by
		 * the time the status bit clears; read it back from there.
		 */
		ret = regmap_read(priv->regmap, MT6320_SPK_CON1, &reg);
		if (ret)
			goto trim_stop;

		polarity = FIELD_GET(MT6320_SPK_OFFSET_L_MODE, reg);
		trim = FIELD_GET(MT6320_SPK_OFFSET_L_SW, reg);

trim_stop:
		cleanup_ret = regmap_write(priv->regmap, MT6320_SPK_CON9, 0x0000);
		if (!ret)
			ret = cleanup_ret;

		cleanup_ret = regmap_clear_bits(priv->regmap,
						MT6320_SPK_CON0, BIT(0));
		if (!ret)
			ret = cleanup_ret;

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
		/*
		 * TOP_CKPDN_CLR selects the audio clock for the speaker
		 * route: the stock driver writes 0x0607 here when opening
		 * DEVICE_OUT_SPEAKER, against 0x0003 for the DAC and
		 * headphone paths (AudioPlatformDevice::AnalogOpen).
		 */
		ret = regmap_write(priv->regmap, MT6320_TOP_CKPDN_CLR, 0x0607);
		if (ret)
			return ret;

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
		/*
		 * As in the headphone path, do not write ZCD_CON2 wholesale:
		 * bits [3:0] and [11:8] are the headphone volume indexes and
		 * belong to the volume control.
		 */
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
		/*
		 * The stock speaker sequence does not touch ZCD_CON2 at all;
		 * its volume indexes belong to the volume control.  The
		 * preceding write used to set 0x0505 here, forcing index 5.
		 */
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

		/*
		 * Take both output stages and both driver enables under
		 * software control, as the stock driver does with 0x0f00.
		 * SPK_EN_MODE is deliberately not set: it makes the hardware
		 * follow the register mode instead of reading the EN_L/EN_R_SW
		 * bits, which would leave the enables above inert.
		 */
		return regmap_write(priv->regmap, MT6320_SPK_CON11,
				    MT6320_SPK_OUTSTG_EN_L_SW |
				    MT6320_SPK_OUTSTG_EN_R_SW |
				    MT6320_SPK_EN_L_SW |
				    MT6320_SPK_EN_R_SW);

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

/* Analog mic input: AUXADC channel 0, buffer on, analog switch to 1.9 V. */
static int mt6320_mic_event(struct snd_soc_dapm_widget *w,
			    struct snd_kcontrol *kcontrol, int event)
{
	struct mt6320_codec_priv *priv =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regmap_update_bits(priv->regmap, MT6320_AUXADC_CON0,
					 MT6320_AUXADC_CON0_CHSEL,
					 FIELD_PREP(MT6320_AUXADC_CON0_CHSEL,
						    MT6320_AUXADC_CON0_CHSEL_ACCDET));
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				    MT6320_ACCDET_MICBIAS_ENABLE);
	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				   MT6320_ACCDET_MICBIAS_DISABLE);
		if (ret)
			return ret;

		return regmap_write(priv->regmap, MT6320_AUXADC_CON0, 0);
	}

	return 0;
}

static const struct snd_soc_dapm_widget mt6320_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Analog", SND_SOC_NOPM, 0, 0,
			    mt6320_analog_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	/*
	 * NEWIF is the serial link to the SoC AFE.  This generation of
	 * MT6320 has no PMIC-side NEWIF configuration registers, so there
	 * is nothing for the codec to program on that link: the AFE drives
	 * the I2S2 interface and the DAC is reached through AFE_CONN1/CON2.
	 * A "NEWIF" widget was here once, but as a supply with no sink path
	 * it never powered and only obscured that.
	 */
	SND_SOC_DAPM_DAC_E("DAC", NULL, SND_SOC_NOPM, 0, 0, mt6320_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("HP Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6320_hp_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUT_DRV_E("Speaker Driver", SND_SOC_NOPM, 0, 0, NULL, 0,
			       mt6320_speaker_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_ADC_E("AIF1 Capture", NULL, SND_SOC_NOPM, 0, 0,
			   mt6320_mic_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_INPUT("Mic Bias"),
	SND_SOC_DAPM_OUTPUT("Headphone"),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route mt6320_dapm_routes[] = {
	/*
	 * The AFE side of these two routes is the AFE's DAI stream widget,
	 * named after its DAI stream_name - "DL1 Playback" and "VUL Capture"
	 * in mt6589-afe-pcm.c - not a separate set of AFE endpoints.  A DAI
	 * gets an auto-created DAPM widget under that name and
	 * dapm_connect_dai_pair() joins the two DAIs through it.
	 */
	{ "DAC", NULL, "DL1 Playback" },
	{ "HP Driver", NULL, "DAC" },
	{ "HP Driver", NULL, "Analog" },
	{ "Headphone", NULL, "HP Driver" },
	{ "Speaker Driver", NULL, "DAC" },
	{ "Speaker Driver", NULL, "Analog" },
	{ "Speaker", NULL, "Speaker Driver" },
	{ "Mic Bias", NULL, "VUL Capture" },
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
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 1,
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

	/* Analog idle baseline; DAPM powers the path per stream. */
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
