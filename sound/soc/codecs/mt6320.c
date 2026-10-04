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

/*
 * Register names for the 0x4xxx window, all derived from
 * MT6320_ABB_AFE_CON() so the 0x4000 base is written down exactly once.
 *
 * The vendor names are AudioAnalogReg.h (AFE_PMICDIG_AUDIO_BASE == 0x4000).
 * Deriving them here rather than spelling out 0x4000/0x4002/... is the whole
 * point: the 0x04xx block is a *different* window, holding the PMIC
 * regulator/power registers (ANALDO_CON*, DIGLDO_CON*, VPROC_CON*, ...), and
 * the two differ only in the high byte.  A raw 0x4xxx literal is therefore
 * the single easiest way to reintroduce this bug, so every AFE address in
 * this file goes through the helper.
 */
#define MT6320_AFE_UL_DL_CON0		(MT6320_ABB_AFE_CON(0x00))
#define MT6320_AFE_DL_SRC2_CON0_L	(MT6320_ABB_AFE_CON(0x02))
#define MT6320_AFE_DL_SRC2_CON1_H	(MT6320_ABB_AFE_CON(0x03))
#define MT6320_AFE_DL_SDM_CON1		(MT6320_ABB_AFE_CON(0x06))
#define MT6320_AFE_UL_SRC_CON0_H	(MT6320_ABB_AFE_CON(0x07))
#define MT6320_AFE_UL_SRC_CON0_L	(MT6320_ABB_AFE_CON(0x08))
#define MT6320_AFE_UL_SRC_CON1_H	(MT6320_ABB_AFE_CON(0x09))
#define MT6320_AFE_I2S_FIFO_UL_CFG0	(MT6320_ABB_AFE_CON(0x10))
#define MT6320_AFE_I2S_FIFO_DL_CFG0	(MT6320_ABB_AFE_CON(0x11))
#define MT6320_AFE_ANA_AFE_TOP_CON0	(MT6320_ABB_AFE_CON(0x12))
#define MT6320_AFE_ANA_AUDIO_TOP_CON0	(MT6320_ABB_AFE_CON(0x13))
#define MT6320_AFE_AFUNC_AUD_CON0	(MT6320_ABB_AFE_CON(0x1a))
#define MT6320_AFE_AFUNC_AUD_CON2	(MT6320_ABB_AFE_CON(0x1c))
#define MT6320_ABB_AFE_DL_SRC2_CON0_H	(MT6320_ABB_AFE_CON(0x01))
/*
 * The sample rate lives in bits [15:12], not a low nibble. The stock
 * driver sets this register as 0x0300 | GetDLFrequency(rate), where
 * GetDLFrequency() returns the code shifted left by twelve: 48 kHz gives
 * 8 << 12, so the register becomes 0x8300.
 */
#define MT6320_ABB_AFE_DL_SRC2_CON0_H_RATE	GENMASK(15, 12)
#define MT6320_ABB_AFE_DL_SRC2_CON0_H_BASE	0x0300

/*
 * Capture (uplink) rate lives in UL_SRC_CON0_H at [4:1], which is a
 * different position and a different encoding from the downlink one above:
 * the vendor's GetULFrequency() returns the *unshifted* code and the
 * sequence itself does the "<< 1", so 8 kHz (code 0x0) reaches the register
 * as 0x00 and 48 kHz (code 0xf) as 0x1e.  The field therefore needs bit 4
 * as well, not just [3:1].  Note also that GetULFrequency() only decodes
 * 8/16/32/48 kHz and warns on anything else, so a rate it does not know
 * silently programs 0 - which is why mt6320_ul_src_rate_code() rejects
 * unsupported rates instead of reproducing that behaviour.
 */
#define MT6320_ABB_AFE_UL_SRC_CON0_H_RATE	GENMASK(4, 1)

/*
 * AUDCLKGEN_CFG0 bit 1 gates the ADC clock.  The vendor reaches this
 * register as 0x0712 with mask 0x0002, so it only ever touches that one bit
 * even though the register is shared with the SRC gate in bit 0.
 */
#define MT6320_ADCCLK_ENABLE		BIT(1)

/*
 * AFUNC_AUD_CON2 is the analog mute (bit 7), kept by name because the
 * pre/post-PMU handlers below use it for both directions.
 */
#define MT6320_AFUNC_AUD_CON2		MT6320_AFE_AFUNC_AUD_CON2

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
 * ACCDET_RSV encoding at all: it drives AUDENCSPARE_CON0 to 0x01 to enable
 * the switch and 0x00 to disable it.  The 0x1090 value is the 1.9 V path
 * and selects RG_AUDACCDETVIN1PULLLOW, which is not what this hardware
 * wants.
 *
 * MT6320_AUDENCSPARE_CON0 itself comes from <linux/mfd/mt6320/registers.h>;
 * only the two values are local.
 */
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
/*
 * ZCD_CON2 bits [3:0] and [11:8]: the headphone left/right volume indexes,
 * i.e. exactly the two fields SOC_DOUBLE_TLV below drives (shift 0 and 8).
 * Named here so the power-down path can park the gain without also trampling
 * the rest of the register the way a full 0x0c0c write does.
 */
#define MT6320_ZCD_CON2_VOL_MASK	(GENMASK(11, 8) | GENMASK(3, 0))

struct mt6320_codec_priv {
	struct device *dev;
	struct regmap *regmap;		/* borrowed from the parent MFD */
	struct clk *clk_aud26m;		/* codec master clock via CCF */
	/*
	 * Sample rate code for the current stream, saved by hw_params() and
	 * read by the DAC and ADC power-up handlers.  Those run from the
	 * DAPM stream events, which ASoC issues after hw_params() for the
	 * same substream, so the value is already valid by then.  The vendor
	 * instead keeps the rate in mBlockSampleRate[] and consults it from
	 * AnalogOpen(); this is the equivalent for a codec that does not
	 * implement hw_params() before its widgets power up.
	 */
	unsigned int rate_code;
	/*
	 * Same, for the capture (uplink) SRC, whose rate encoding is a
	 * different one from the downlink - see mt6320_ul_src_rate_code().
	 */
	unsigned int ul_rate_code;
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

/*
 * Uplink rate code for the capture SRC, i.e. the vendor's GetULFrequency().
 *
 * This is deliberately *not* mt6320_dl_src_rate_code(): the uplink uses a
 * different encoding (see MT6320_ABB_AFE_UL_SRC_CON0_H_RATE) and the vendor
 * only decodes 8/16/32/48 kHz, warning - and programming 0 - for anything
 * else.  The codec's capture DAI advertises the full 8k..48k ladder via
 * MT6320_CODEC_RATES, so a rate outside this set can legitimately arrive
 * here; return an error instead of silently recording at the wrong rate.
 */
static int mt6320_ul_src_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return 0x0;
	case 16000:
		return 0x5;
	case 32000:
		return 0xa;
	case 48000:
		return 0xf;
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
	bool playback = substream->stream == SNDRV_PCM_STREAM_PLAYBACK;
	int rate_code, ul_rate_code;

	/*
	 * Validate only the ladder this direction actually uses.  The two are
	 * different sizes: the downlink SRC covers 8/11.025/12/16/22.05/24/32/
	 * 44.1/48 kHz while the uplink SRC covers only 8/16/32/48 kHz, so
	 * resolving the uplink code during a playback hw_params rejected
	 * perfectly valid playback rates such as 44.1 kHz.
	 */
	if (playback) {
		rate_code = mt6320_dl_src_rate_code(rate);
		if (rate_code < 0)
			return rate_code;

		priv->rate_code = rate_code;
	} else {
		ul_rate_code = mt6320_ul_src_rate_code(rate);
		if (ul_rate_code < 0)
			return ul_rate_code;

		priv->ul_rate_code = ul_rate_code;
	}

	/*
	 * Write the whole register rather than updating only the rate field.
	 * regmap_update_bits() computes (orig & ~mask) | (val & mask), and the
	 * mask covers just the rate bits - so the SRC-enable/base field
	 * (0x0300) was ANDed away and never reached the register, leaving the
	 * PMIC downlink SRC disabled.  A plain write is what the stock driver
	 * effectively does, and what the comment above this call describes.
	 *
	 * Only the downlink SRC is programmed from here, and only for a
	 * playback stream: it is the playback SRC2 rate register, so a capture
	 * hw_params writing it would reprogram the running playback stream's
	 * rate.  The uplink SRC is written by mt6320_mic_event() during stream
	 * start, because the vendor sequences it there - it needs the mic path
	 * powered first, and writes the rate, clears it, then writes it back.
	 */
	if (substream->stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	return regmap_write(priv->regmap, MT6320_ABB_AFE_DL_SRC2_CON0_H,
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

		/*
		 * Unmute.  AFUNC_AUD_CON2 bit 7 is a mute, not an enable: the
		 * stock driver asserts it while it configures a path and
		 * clears it once the configuration is done
		 * (AudioMachineDevice: SetAnalogReg(AFUNC_AUD_CON2, mute << 7),
		 * set to 0x0080 on open and 0x0000 once configured). Leaving
		 * it set keeps the output muted for as long as the path is
		 * powered, i.e. for as long as it is playing.
		 */
		ret = regmap_update_bits(priv->regmap, MT6320_AFUNC_AUD_CON2,
					 BIT(7), 0);
		if (ret)
			return ret;

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
 * Analog idle baseline, before any stream is running.
 *
 * The ABB digital path is deliberately NOT programmed here.  Every 0x4xxx
 * register this list used to open with is programmed per-stream in
 * mt6320_dac_event() and mt6320_mic_event() instead, because that is where
 * the vendor does it (AudioPlatformDevice::AnalogOpen, every DEVICE_OUT_*
 * and DEVICE_IN_ADC* case) and several of them carry the sample rate.
 *
 * The block that was here was the MT6323 idle baseline, verbatim from the
 * now-removed sound/soc/codecs/mt6323.c, MT6323 register numbering and all:
 *
 *	{ MT6320_ABB_AFE_CON(1),  0x0009 },
 *	{ MT6320_ABB_AFE_CON(3),  0x0221 },
 *	{ MT6320_ABB_AFE_CON(4),  0x0255 },
 *	{ MT6320_ABB_AFE_CON(5),  0x0028 },
 *	{ MT6320_ABB_AFE_CON(6),  0x0218 },
 *	{ MT6320_ABB_AFE_CON(7),  0x0204 },
 *	{ MT6320_ABB_AFE_CON(10), 0x0001 },
 *
 * MT6320_ABB_AFE_CON() is the same arithmetic on both chips (0x4000 + n * 2),
 * but the register *layout* behind that arithmetic is not the same, so the
 * same n lands on a different register.  Against the MT6320 map this driver
 * follows (AudDrv_Ana.h) those seven entries are:
 *
 *	CON(1)  0x4002  AFE_DL_SRC2_CON0_H   the downlink sample-rate field
 *	CON(3)  0x4006  AFE_DL_SRC2_CON1_H
 *	CON(4)  0x4008  AFE_DL_SRC2_CON1_L
 *	CON(5)  0x400a  AFE_DL_SDM_CON0
 *	CON(6)  0x400c  AFE_DL_SDM_CON1
 *	CON(7)  0x400e  AFE_UL_SRC_CON0_H   the uplink sample-rate field
 *	CON(10) 0x4014  AFE_UL_SRC_CON1_L
 *
 * So probe was writing MT6323 SDM and UL-SRC trim values into MT6320's
 * downlink-SRC and uplink-SRC *rate* registers, with the SRC enable/base
 * bits left at their reset value.  Both power-up handlers rewrite all of
 * them before each stream, which is what made this survivable rather than
 * fatal, but there is no reason to leave a wrong value behind at probe.
 */
static const struct reg_sequence mt6320_codec_init[] = {
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
		 * AudioPlatformDevice::AnalogOpen() for DEVICE_OUT_DAC
		 * (AudioPlatformDevice.cpp, the DEVICE_OUT_EARPIECE case).
		 *
		 * Every address here is derived from MT6320_ABB_AFE_CON(), so
		 * they all land in the 0x4000 AFE window.  They used to be
		 * spelled with the MT6320_ANALDO_CON and MT6320_DIGLDO_CON
		 * macros, which are *not* the same registers: those live at
		 * 0x0400.., the PMIC regulator block, and only the low byte
		 * agreed, so 0x4034 became 0x0434, 0x4022 became 0x0422, and so
		 * on.  Every microphone power-up was then overwriting
		 * power-management registers - VGP6, VSIM2, VMC1, VUSB - and
		 * disabling rails the rest of the system depends on.  The values
		 * themselves were right; only the addresses were wrong.
		 *
		 * Note on 0x4000 (AFE_UL_DL_CON0, written 0x007f): this is an
		 * ABB/AFE register reached through the PMIC digital-audio
		 * bridge at base 0x4000 - not the regulator register that the
		 * MT6320_ANALDO_CON0 macro names at 0x0400.  It is the register
		 * that enables the shared digital audio path for both
		 * directions, and the full 0x007f is what the vendor writes;
		 * a partial write of just BIT(0) does not get samples through.
		 */
		ret = regmap_write(priv->regmap, MT6320_AFE_AFUNC_AUD_CON0, 0xc3a1);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_AFUNC_AUD_CON2, 0x0006);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_AFUNC_AUD_CON2, 0x0003);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_AFUNC_AUD_CON2, 0x000b);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_DL_SDM_CON1, 0x001e);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_ABB_AFE_DL_SRC2_CON0_H,
				   MT6320_ABB_AFE_DL_SRC2_CON0_H_BASE |
				   (priv->rate_code << 12));
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_UL_DL_CON0, 0x007f);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_DL_SRC2_CON0_L, 0x1801);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_DL_SRC2_CON1_H, 0x0000);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON1_H, 0x00e1);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_ANA_AFE_TOP_CON0, 0x0000);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AFE_I2S_FIFO_DL_CFG0, 0x004f);
		if (ret)
			return ret;

		/*
		 * Power the DAC cores up: 0x0003 is RG_AUDDACLPWRUP_VAUDP12
		 * (bit 0) | RG_AUDDACRPWRUP_VAUDP12 (bit 1).  These two bits
		 * are the entire DAC power-up control - the vendor writes no
		 * other bit of this register on any path (power_init, all four
		 * AnalogOpen cases and all four AnalogClose cases use 0x0009,
		 * 0x000f or 0x0000).
		 *
		 * This used to write 0x7010 here and 0x6010 on POST_PMD.
		 * Both are the *MT6323* AUDTOP_CON0 values, carried over from
		 * the now-removed sound/soc/codecs/mt6323.c, and the MT6320
		 * register map is not compatible with them: bits [15:13] are
		 * RG_AUDHPRSCDISABLE / RG_AUDHPLSCDISABLE / RG_AUDHSSCDISABLE
		 * and bits [10:9] are RG_HPOUTPUTRESET0 / RG_HPINPUTRESET0
		 * (upmu_hw.h, VAUDP12 group).  So 0x7010 powered the DAC on
		 * *and* held the headphone amplifiers in their disabled, input-
		 * and output-reset state: the path settled hard enough to make
		 * one pop as the bias ramped, and then nothing came out.
		 * 0x6010 differs from 0x7010 only in bit 12, so power-down
		 * cleared two of the three HS disable bits while leaving
		 * RG_AUDHSSCDISABLE (bit 13) asserted, which would have kept
		 * the headphone silent on every stream after the first even
		 * once power-up was right.  (Bit 12 is not named in the
		 * VAUDP12 group in upmu_hw.h.)
		 *
		 * Written as an explicit full value, not a bit update: the
		 * surrounding sequence clears the HP power-up bits (bit 0) and
		 * sets the HP-only enable (bit 3) on the same register in
		 * mt6320_hp_event(), so leaving bits [15:9] untouched here is
		 * what let the disable bits survive.
		 *
		 * The AUDBUF_CFG4 = 0x0014 write that used to sit immediately
		 * before this one was also MT6323 carry-over (mt6323.c wrote
		 * AUDTOP_CON(5) = 0x0014).  On MT6320 that register holds the ABI
		 * de-click reserved bits - RG_ABIDEC_RESERVED_VAUDP12 at [15:8]
		 * and RG_ABIDEC_RESERVED_VA28 at [7:0] (upmu_hw.h) - and
		 * upmu_set_rg_abidec_reserved_*(), the only vendor code that
		 * touches it, is never called from anywhere in the BSP.  It is
		 * dropped rather than restored on POST_PMD for the same reason:
		 * there is no power-sequence state on this register to restore.
		 */
		return regmap_write(priv->regmap, MT6320_AUDDAC_CON0, 0x0003);

	case SND_SOC_DAPM_POST_PMD:
		/*
		 * The vendor's power-down for the DAC is a plain 0x0000 on
		 * AUDDAC_CON0 (AudioMachineDevice::AnalogClose, every case).
		 * Nothing else: no AUDBUF_CFG4 restore, because there is
		 * nothing on this register for a power sequence to restore.
		 */
		return regmap_write(priv->regmap, MT6320_AUDDAC_CON0, 0x0000);
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
		/*
		 * The vendor does write ZCD_CON2 0x0c0c here (AnalogClose,
		 * DEVICE_OUT_HEADSET*), but that register is the headphone volume
		 * control's own register: bits [3:0] and [11:8] are the left and
		 * right volume indexes, which the "Headphone Volume" control and
		 * the mixer read and write.  Forcing index 12 on every power-down
		 * throws the user's volume away, and it is exactly the bug this
		 * file already fixed once on the power-*up* side (commit
		 * "stop power-up from overwriting the headphone volume").
		 *
		 * Park the volume at 0 dB instead - index 8, which is what the
		 * probe-time baseline in mt6320_codec_init[] uses and what
		 * GetAnalogGain() reports as volume 0 - and leave the control's
		 * own value otherwise alone.  Powering the amplifier down is what
		 * silences it; the index is what it comes back at.
		 */
		ret = regmap_update_bits(priv->regmap, MT6320_ZCD_CON2,
					 MT6320_ZCD_CON2_VOL_MASK,
					 ZCD_GAIN_REG(ZCD_GAIN_0DB));
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
		/*
		 * Mic bias / accessory-detect switch first, then the vendor's
		 * capture power-up sequence, following
		 * AudioPlatformDevice::AnalogOpen() for
		 * DEVICE_IN_ADC1/DEVICE_IN_ADC2.
		 *
		 * The order and the usleep(600) gaps are load-bearing, not
		 * cosmetic: after enabling the UL FIFO the vendor writes the
		 * uplink SRC rate, clears it, waits, writes the rate back,
		 * clears the SRC, and re-enables it.  That clear/reload cycle
		 * is what latches the sample rate into the SRC after its clock
		 * domain has just been started; collapsing the sequence into a
		 * single write leaves the uplink SRC running on whatever rate
		 * was previously latched, i.e. on the wrong rate or not at all.
		 */
		ret = regmap_update_bits(priv->regmap, MT6320_AUXADC_CON0,
					 MT6320_AUXADC_CON0_CHSEL,
					 FIELD_PREP(MT6320_AUXADC_CON0_CHSEL,
						    MT6320_AUXADC_CON0_CHSEL_ACCDET));
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				   MT6320_ACCDET_MICBIAS_ENABLE);
		if (ret)
			return ret;

		/*
		 * 0x0712: AUDCLKGEN_CFG0, bit 1 - the ADC clock gate.  The
		 * vendor writes this register with mask 0x0002, i.e. it only
		 * touches bit 1, so update just that bit rather than the whole
		 * word: bit 0 of the same register is the SRC gate that
		 * mt6320_hp_event() and mt6320_speaker_event() set, and a full
		 * write here would clear it out from under a concurrently
		 * active output path.
		 */
		ret = regmap_update_bits(priv->regmap, MT6320_AUDCLKGEN_CFG0,
					 MT6320_ADCCLK_ENABLE, 0);
		if (ret)
			return ret;

		/* 0x4010: AFE_UL_SRC_CON0_L - low half of the UL SRC. */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_L,
				   0x0000);
		if (ret)
			return ret;

		/* 0x0712, bit 1 back on: enable the ADC clock. */
		ret = regmap_update_bits(priv->regmap, MT6320_AUDCLKGEN_CFG0,
					 MT6320_ADCCLK_ENABLE,
					 MT6320_ADCCLK_ENABLE);
		if (ret)
			return ret;

		/* 0x4026: ANA_AUDIO_TOP_CON0. */
		ret = regmap_write(priv->regmap, MT6320_AFE_ANA_AUDIO_TOP_CON0,
				   0x0000);
		if (ret)
			return ret;

		/* 0x400e: AFE_UL_SRC_CON0_H - UL SRC high half, with rate. */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_H,
				   FIELD_PREP(MT6320_ABB_AFE_UL_SRC_CON0_H_RATE,
					      priv->ul_rate_code));
		if (ret)
			return ret;

		/* 0x4000: AFE_UL_DL_CON0 - enable the shared digital path. */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_DL_CON0, 0x007f);
		if (ret)
			return ret;

		/*
		 * 0x4010: AFE_UL_SRC_CON0_L again.  0x0201 because this board
		 * is built with MTK_AUDIO_HD_REC_SUPPORT (it appears in
		 * eastaeon89_wet_td/ProjectConfig.mk), which is the branch the
		 * vendor takes here; the non-HD build would use 0x0601.
		 */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_L,
				   0x0201);
		if (ret)
			return ret;

		/* 0x4020: AFE_I2S_FIFO_UL_CFG0 - UL FIFO enable. */
		ret = regmap_write(priv->regmap, MT6320_AFE_I2S_FIFO_UL_CFG0,
				   0x004f);
		if (ret)
			return ret;

		usleep_range(600, 600);

		/* Clear the UL SRC rate, then let it settle. */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_H,
				   0x0000);
		if (ret)
			return ret;

		usleep_range(600, 600);

		/* Reload the rate into the freshly cleared UL SRC. */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_H,
				   FIELD_PREP(MT6320_ABB_AFE_UL_SRC_CON0_H_RATE,
					      priv->ul_rate_code));
		if (ret)
			return ret;

		usleep_range(600, 600);

		/* Drop the UL SRC, then bring it back up around the reload. */
		ret = regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_L,
				   0x0000);
		if (ret)
			return ret;

		usleep_range(600, 600);

		return regmap_write(priv->regmap, MT6320_AFE_UL_SRC_CON0_L,
				    0x0201);
	case SND_SOC_DAPM_POST_PMD:
		/*
		 * The vendor has no explicit capture power-down in this file,
		 * so this only undoes what the pre-PMU path asserted: the mic
		 * bias switch, the AUXADC channel select, and the ADC clock
		 * that the pre-PMU sequence enabled via AUDCLKGEN_CFG0 bit 1.
		 * Leaving that clock on would hold the ADC domain powered
		 * between streams.
		 */
		ret = regmap_write(priv->regmap, MT6320_AUDENCSPARE_CON0,
				   MT6320_ACCDET_MICBIAS_DISABLE);
		if (ret)
			return ret;

		ret = regmap_write(priv->regmap, MT6320_AUXADC_CON0, 0);
		if (ret)
			return ret;

		return regmap_clear_bits(priv->regmap, MT6320_AUDCLKGEN_CFG0,
					 MT6320_ADCCLK_ENABLE);
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
	/*
	 * The analog mic front end, named after the "DAC" it mirrors on
	 * playback, and the widget that carries mt6320_mic_event (mic bias and
	 * AUXADC channel select).
	 *
	 * It must NOT be called "VUL Capture": that is the name ASoC gives the
	 * capture DAI widget, which it auto-creates from the DAI's stream_name
	 * below (snd_soc_dapm_new_dai_widgets()).  This widget used to be
	 * called "AIF1 Capture" - the very same name as that auto-created DAI
	 * widget - so two widgets in this one component shared a name, and
	 * route lookup for the codec's own routes could not tell them apart.
	 * It needs a name of its own.
	 */
	SND_SOC_DAPM_ADC_E("ADC", NULL, SND_SOC_NOPM, 0, 0,
			   mt6320_mic_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_INPUT("Mic Bias"),
	SND_SOC_DAPM_OUTPUT("Headphone"),
	SND_SOC_DAPM_SPK("Speaker", NULL),
};

static const struct snd_soc_dapm_route mt6320_dapm_routes[] = {
	/*
	 * "DL1 Playback" and "VUL Capture" are the AFE's DAI stream widgets.
	 * Each DAI gets an auto-created DAPM widget named after its
	 * stream_name (snd_soc_dapm_new_dai_widgets()), and
	 * dapm_connect_dai_pair() walks those widgets by pointer to join the
	 * codec DAI to the CPU DAI.  The names matter for two other reasons,
	 * which is why they have to agree across components:
	 *
	 *  - the routes below are looked up by name across every widget on the
	 *    card (snd_soc_dapm_add_route()), so a route named for the wrong
	 *    stream resolves to nothing, or worse to a same-named widget that
	 *    is not on the path; and
	 *  - snd_soc_dapm_link_dai_widgets() pairs widgets up by substring
	 *    match on their stream names, so only equal names pair.
	 *
	 * Both AFE names are fixed by the hardware interface (AFE memory
	 * interfaces DL1 and VUL), so the codec matches them end to end rather
	 * than imposing "AIF1 ...", and the analog front ends hang off the
	 * matching widget just as "DAC" hangs off "DL1 Playback" above.
	 *
	 * A route is { sink, control, source }: the signal flows source ->
	 * sink, and the widget that consumes the audio is the sink.  So the
	 * mic front end is the SOURCE feeding "Mic Bias", and the capture DAI
	 * widget is the SINK the front end feeds.  Compare the playback pair
	 * just above, { "DAC", NULL, "DL1 Playback" }, where "DL1 Playback" is
	 * the sink.  This matches upstream sound/soc/codecs/mt6357.c, which has
	 * the same { "ADC", NULL, ... } shape with the ADC as the sink of its
	 * supply route.
	 */
	{ "DAC", NULL, "DL1 Playback" },
	{ "HP Driver", NULL, "DAC" },
	{ "HP Driver", NULL, "Analog" },
	{ "Headphone", NULL, "HP Driver" },
	{ "Speaker Driver", NULL, "DAC" },
	{ "Speaker Driver", NULL, "Analog" },
	{ "Speaker", NULL, "Speaker Driver" },
	{ "Mic Bias", NULL, "ADC" },
	{ "VUL Capture", NULL, "ADC" },
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
			/*
			 * Named for the AFE playback DAI's memory interface,
			 * which is also the machine dai_link's stream_name
			 * ("DL1 Playback" in both mt6589-afe-pcm.c and
			 * mt6589-mt6320.c).  It has to be this exact string:
			 * it names the DAI widget the routes connect to, and
			 * only equal stream names pair up in
			 * snd_soc_dapm_link_dai_widgets().
			 *
			 * This was "AIF1 Playback", which matched nothing:
			 * the route below connects "DL1 Playback" to the DAC,
			 * so the DAC had no path from the DAI and playback ran
			 * with the analog path powered but no data reaching it
			 * - one pop as the outputs settled, then silence.
			 */
			.stream_name = "DL1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT6320_CODEC_RATES,
			.formats = MT6320_CODEC_FORMATS,
		},
		.capture = {
			/*
			 * Named for the AFE capture DAI's memory interface,
			 * which is also the machine dai_link's stream_name
			 * ("VUL Capture" in both mt6589-afe-pcm.c and
			 * mt6589-mt6320.c).  It has to be this exact string:
			 * it names the DAI widget the routes connect to, and
			 * only equal stream names pair up in
			 * snd_soc_dapm_link_dai_widgets().
			 */
			.stream_name = "VUL Capture",
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
