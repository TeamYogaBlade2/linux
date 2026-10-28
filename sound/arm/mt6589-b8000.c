// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal ALSA driver for Lenovo B8000-H (MT6589 + MT6320 + SN2145B).
 *
 * The register programming is taken from Lenovo's MT6589 kernel headers and
 * the stripped stock audio HAL (B8000_A442_001_020_140620_WW_3GDATA).
 * Playback: 48 kHz, stereo, S16_LE on DL1 to the speaker.  Capture: the
 * built-in analog microphone on MT6320 MIC1, 8/16/32/48 kHz, mono or stereo,
 * S16_LE on the VUL memif.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

#ifndef DRV_NAME
#define DRV_NAME		"b8000-mt6589-audio"
#endif
#ifndef CARD_ID
#define CARD_ID			"B8000"
#endif
#define AFE_PHYS		0x12070000
#define AFE_SIZE		0x1000
#define INFRACFG_PHYS		0x10001000
#define INFRACFG_SIZE		0x1000
#define INFRA_PDN_SET		0x0040
#define INFRA_PDN_CLR		0x0044
#define INFRA_PDN_STA		0x0048
#define INFRA_AUDIO_BIT		BIT(5)

#define AUDIO_AFE_TOP_CON0	0x0000
#define AFE_DAC_CON0		0x0010
#define AFE_DAC_CON1		0x0014
#define AFE_CONN1		0x0024
#define AFE_CONN2		0x0028
#define AFE_CONN3		0x002c
#define AFE_I2S_CON1		0x0034
#define AFE_I2S_CON2		0x0038
#define AFE_DL1_BASE		0x0040
#define AFE_DL1_CUR		0x0044
#define AFE_DL1_END		0x0048
#define AFE_VUL_BASE		0x0080
#define AFE_VUL_END		0x0088
#define AFE_VUL_CUR		0x008c

/* AFE_DAC_CON0 memif enables, AFE_DAC_CON1 VUL fields. */
#define AFE_ON			BIT(0)
#define DL1_ON			BIT(1)
#define VUL_ON			BIT(3)
#define VUL_MODE_SHIFT		16
#define VUL_MODE_MASK		(0xf << VUL_MODE_SHIFT)
#define VUL_MONO		BIT(27)
#define VUL_R_MONO		BIT(28)

/* MT6320 PMIC audio registers (16-bit, byte addressed through pwrap). */
#define TOP_CKPDN_CLR		0x0106
#define AFE_UL_DL_CON0		0x4000
#define AFE_DL_SRC2_CON0_H	0x4002
#define AFE_DL_SRC2_CON0_L	0x4004
#define AFE_DL_SRC2_CON1_H	0x4006
#define AFE_DL_SDM_CON1		0x400c
#define AFE_UL_SRC_CON0_H	0x400e
#define AFE_UL_SRC_CON0_L	0x4010
#define AFE_PREDIS_CON0_H	0x4016
#define AFE_PREDIS_CON0_L	0x4018
#define AFE_PREDIS_CON1_H	0x401a
#define AFE_PREDIS_CON1_L	0x401c
#define ANA_AFE_I2S_CON1	0x401e
#define AFE_I2S_FIFO_UL_CFG0	0x4020
#define AFE_I2S_FIFO_DL_CFG0	0x4022
#define ANA_AFE_TOP_CON0	0x4024
#define ANA_AUDIO_TOP_CON0	0x4026
#define AFUNC_AUD_CON0		0x4034
#define AFUNC_AUD_CON2		0x4038

#define AUDDAC_CON0		0x0700
#define AUDBUF_CFG0		0x0702
#define AUDBUF_CFG1		0x0704
#define AUDBUF_CFG2		0x0706
#define IBIASDIST_CFG0		0x070c
#define AUDCLKGEN_CFG0		0x0712
#define AUDLDO_CFG0		0x0714
#define AUDNVREGGLB_CFG0	0x0718
#define AUD_NCP0		0x071a
#define AUDPREAMP_CON0		0x071c
#define AUDADC_CON0		0x071e
#define AUDDIGMI_CON0		0x072c
#define AUDLSBUF_CON0		0x072e
#define AUDPREAMPGAIN_CON0	0x0736
#define ZCD_CON0		0x0738
#define ZCD_CON2		0x073c
#define NCP_CLKDIV_CON0		0x0744
#define NCP_CLKDIV_CON1		0x0746

/*
 * Bits the two directions share in the PMIC.  The stock HAL's loopback test,
 * which runs both at once, writes AUD_NCP0 = 0xf000: playback's 0xe000 and
 * capture's 0x9000 together.
 */
#define NCP0_BASE		0x8000
#define NCP0_PLAYBACK		0x6000
#define NCP0_CAPTURE		0x1000
#define CLKGEN_DAC		BIT(0)
#define CLKGEN_ADC		BIT(1)

/* AUDPREAMP_CON0: preamp L/R on [1:0], input select L [4:2], R [7:5]. */
#define PREAMP_ON		0x0003
#define PREAMP_SEL_MASK		0x00fc
#define PREAMP_MIC1		((1 << 2) | (1 << 5))
/* AUDDIGMI_CON0 bit 8 powers the microphone bias (RG_AUDPWDBMICBIAS). */
#define MICBIAS_ON		0x0180
/*
 * AUDPREAMPGAIN_CON0: L [2:0], R [6:4] in 6 dB steps.  Steps 6 and 7 give
 * an all-zero stream on this board, so the control stops at 5 (30 dB).  The
 * stock HAL uses 3; at 5 normal speech at 25 cm peaks near -20 dBFS.
 */
#define MIC_GAIN_MAX		5

#define SPEAKER_EN_GPIO		73
#define BUFFER_BYTES		16384
#define CAPTURE_BUFFER_BYTES	32768
#define RATE_HZ			48000

struct b8k_stream {
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	struct work_struct elapsed;
	u32 cur_reg;
	u32 last_period;
	bool running;
};

struct b8k_audio {
	struct device *dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct b8k_stream play;
	struct b8k_stream cap;
	void __iomem *afe;
	void __iomem *infra;
	struct regmap *pmic;
	struct gpio_desc *speaker_enable;
	struct mutex power_lock;	/* AFE/PMIC state shared by both directions */
	spinlock_t lock;		/* running flags vs. the timers */
	unsigned int volume;
	unsigned int mic_gain;
	unsigned int afe_users;
	bool playing;
	bool capturing;
	bool amp_on;
	bool infra_was_off;
};

static struct platform_device *b8k_pdev;

static struct gpiod_lookup_table b8k_gpio_table = {
	.dev_id = DRV_NAME,
	.table = {
		GPIO_LOOKUP("pinctrl_paris", SPEAKER_EN_GPIO, "speaker-enable",
			    GPIO_ACTIVE_HIGH),
		{ }
	},
};

static const struct snd_pcm_hardware b8k_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = RATE_HZ,
	.rate_max = RATE_HZ,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES,
	.period_bytes_min = 256,
	.period_bytes_max = 4096,
	.periods_min = 2,
	.periods_max = 64,
};

static const struct snd_pcm_hardware b8k_capture_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
		 SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = CAPTURE_BUFFER_BYTES,
	.period_bytes_min = 256,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 64,
};

static inline void afe_update(struct b8k_audio *a, u32 reg, u32 val, u32 mask)
{
	u32 old = readl(a->afe + reg);

	writel((old & ~mask) | (val & mask), a->afe + reg);
}

static int ana_write(struct b8k_audio *a, unsigned int reg, unsigned int val)
{
	return regmap_write(a->pmic, reg, val);
}

static int ana_update(struct b8k_audio *a, unsigned int reg,
		      unsigned int val, unsigned int mask)
{
	return regmap_update_bits(a->pmic, reg, mask, val);
}

/* AFE rate code (AFE_DAC_CON1, AFE_I2S_CON*) and MT6320 UL voice mode. */
static u32 afe_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 16000:	return 4;
	case 32000:	return 8;
	default:	return 10;
	}
}

static u32 ul_voice_mode(unsigned int rate)
{
	/* AudioPlatformDevice::GetULFrequency */
	switch (rate) {
	case 8000:	return 0x00;
	case 16000:	return 0x0a;
	case 32000:	return 0x14;
	default:	return 0x1e;
	}
}

/* Infra clock and AFE power, taken by whichever direction starts first. */
static void b8k_afe_get(struct b8k_audio *a)
{
	if (a->afe_users++)
		return;
	a->infra_was_off = !!(readl(a->infra + INFRA_PDN_STA) & INFRA_AUDIO_BIT);
	writel(INFRA_AUDIO_BIT, a->infra + INFRA_PDN_CLR);
	/* Vendor clock manager clears bit 2 (AFE gate); bit 14 stays set. */
	afe_update(a, AUDIO_AFE_TOP_CON0, BIT(14), BIT(14) | BIT(2));
}

static void b8k_afe_put(struct b8k_audio *a)
{
	if (--a->afe_users)
		return;
	afe_update(a, AFE_DAC_CON0, 0, AFE_ON);
	if (a->infra_was_off)
		writel(INFRA_AUDIO_BIT, a->infra + INFRA_PDN_SET);
}

static unsigned int ncp0_value(struct b8k_audio *a)
{
	return NCP0_BASE | (a->playing ? NCP0_PLAYBACK : 0) |
	       (a->capturing ? NCP0_CAPTURE : 0);
}

/*
 * HP output muxes in AUDBUF_CFG0: right [12:9], left [8:5]; 4'b0100 selects
 * the audio DAC (stock HAL AudioMachineDevice::AnalogSetMux, MUX_AUDIO).
 * Without it the DAC runs but the HP buffers, which feed the SN2145B, stay
 * disconnected.
 */
#define HP_MUX_MASK		0x1fe0
static unsigned int hp_mux = 0x0880;
module_param(hp_mux, uint, 0644);
MODULE_PARM_DESC(hp_mux, "AUDBUF_CFG0 HP mux bits (0x0880 = audio DAC)");

/* Values used by the stock HAL for the headset DAC path used by speaker. */
static const u16 hp_gain_table[] = {
	0x0f9f, 0x0e0e, 0x0d0d, 0x0c0c, 0x0909, 0x0606, 0x0303, 0x0000,
};

/* Called with power_lock held and a->playing already set. */
static int b8k_analog_on(struct b8k_audio *a)
{
	int ret;

	/* Digital PMIC downlink, exact 48 kHz stock-HAL sequence. */
	ret = ana_write(a, TOP_CKPDN_CLR, 0x0003);
	if (ret)
		return ret;
	ana_write(a, AFUNC_AUD_CON2, 0x0006);
	ana_write(a, AFUNC_AUD_CON0, 0xc3a1);
	ana_write(a, AFUNC_AUD_CON2, 0x0003);
	ana_write(a, AFUNC_AUD_CON2, 0x000b);
	ana_write(a, AFE_DL_SDM_CON1, 0x001e);
	ana_write(a, AFE_DL_SRC2_CON0_H, 0x8300);
	ana_write(a, AFE_UL_DL_CON0, 0x007f);
	ana_write(a, AFE_DL_SRC2_CON0_L, 0x1801);
	ana_write(a, AFE_DL_SRC2_CON1_H, 0x0000);
	ana_write(a, ANA_AFE_I2S_CON1, 0x00e1);
	ana_write(a, ANA_AFE_TOP_CON0, 0x0000);
	ana_write(a, AFE_I2S_FIFO_DL_CFG0, 0x004f);

	/* Cap-less HP/line path; keep gain at the quietest table entry. */
	ana_update(a, AFUNC_AUD_CON2, 0x0080, 0x0080);
	ana_write(a, AUDLDO_CFG0, 0x0d92);
	ana_write(a, AUDNVREGGLB_CFG0, 0x000c);
	ana_write(a, AUD_NCP0, ncp0_value(a));
	ana_write(a, NCP_CLKDIV_CON0, 0x102b);
	ana_write(a, NCP_CLKDIV_CON1, 0x0000);
	usleep_range(1000, 1500);
	ana_write(a, AUDBUF_CFG0, 0x0008);
	ana_write(a, ZCD_CON0, 0x0101);
	ana_write(a, IBIASDIST_CFG0, 0x0552);
	ana_write(a, AUDBUF_CFG1, 0x0900);
	ana_write(a, AUDBUF_CFG2, 0x0082);
	ana_write(a, AUDBUF_CFG0, 0x0009);
	usleep_range(1000, 1500);
	ana_write(a, AUDBUF_CFG1, 0x0940);
	ana_write(a, AUDBUF_CFG0, 0x000f);
	usleep_range(1000, 1500);
	ana_write(a, AUDBUF_CFG1, 0x0100);
	ana_write(a, AUDBUF_CFG2, 0x0082);
	ana_write(a, ZCD_CON2, hp_gain_table[a->volume]);
	ana_update(a, AUDCLKGEN_CFG0, CLKGEN_DAC, CLKGEN_DAC);
	ana_write(a, AUDDAC_CON0, 0x000f);
	usleep_range(1000, 1500);
	ana_update(a, AUDBUF_CFG0, 0x0006, 0x0007);
	ana_update(a, AUDBUF_CFG0, hp_mux & HP_MUX_MASK, HP_MUX_MASK);
	ana_update(a, AFUNC_AUD_CON2, 0x0000, 0x0080);

	gpiod_set_value_cansleep(a->speaker_enable, 1);
	msleep(40);
	a->amp_on = true;
	return 0;
}

/* Called with power_lock held and a->playing already cleared. */
static void b8k_analog_off(struct b8k_audio *a)
{
	if (a->amp_on) {
		gpiod_set_value_cansleep(a->speaker_enable, 0);
		a->amp_on = false;
	}
	ana_write(a, AUDDAC_CON0, 0x0000);
	ana_update(a, AUDCLKGEN_CFG0, 0, CLKGEN_DAC);
	ana_write(a, AUDBUF_CFG0, 0x0000);
	ana_write(a, AUD_NCP0, ncp0_value(a));
	if (a->capturing)
		return;
	ana_write(a, AUDNVREGGLB_CFG0, 0x0006);
	ana_write(a, AUDLDO_CFG0, 0x0192);
	ana_write(a, AFE_UL_DL_CON0, 0x0000);
}

/*
 * Microphone on: AudioPlatformDevice::AnalogOpen (digital uplink) and
 * AudioMachineDevice::AnalogOpen (ADC) of the stock HAL for DEVICE_IN_ADC.
 * Called with power_lock held and a->capturing already set.
 */
static int b8k_mic_on(struct b8k_audio *a, unsigned int rate)
{
	int ret;

	ret = ana_write(a, TOP_CKPDN_CLR, 0x0003);
	if (ret)
		return ret;
	ana_update(a, AUDCLKGEN_CFG0, 0, CLKGEN_ADC);
	ana_write(a, AFE_UL_SRC_CON0_L, 0x0000);
	ana_update(a, AUDCLKGEN_CFG0, CLKGEN_ADC, CLKGEN_ADC);
	ana_write(a, ANA_AUDIO_TOP_CON0, 0x0000);
	ana_write(a, AFE_UL_SRC_CON0_H, ul_voice_mode(rate));
	ana_write(a, AFE_UL_DL_CON0, 0x007f);
	ana_write(a, AFE_UL_SRC_CON0_L, 0x0201);
	ana_write(a, AFE_I2S_FIFO_UL_CFG0, 0x004f);

	ana_write(a, AUDNVREGGLB_CFG0, 0x000c);
	ana_write(a, AUDLDO_CFG0, 0x0d92);
	ana_write(a, AUD_NCP0, ncp0_value(a));
	ana_update(a, AUDPREAMP_CON0, PREAMP_MIC1, PREAMP_SEL_MASK);
	ana_update(a, AUDPREAMP_CON0, PREAMP_ON, PREAMP_ON);
	usleep_range(1000, 1500);
	ana_write(a, AUDADC_CON0, 0x0093);
	ana_update(a, NCP_CLKDIV_CON0, 0x102b, 0x102b);
	ana_write(a, NCP_CLKDIV_CON1, 0x0000);
	ana_update(a, AUDDIGMI_CON0, MICBIAS_ON, MICBIAS_ON);
	return ana_update(a, AUDPREAMPGAIN_CON0, a->mic_gain | (a->mic_gain << 4),
			  0x0077);
}

/* AudioMachineDevice/AudioPlatformDevice::AnalogClose for DEVICE_IN_ADC. */
static void b8k_mic_off(struct b8k_audio *a)
{
	ana_update(a, AUDPREAMP_CON0, 0, PREAMP_ON);
	ana_write(a, AUDADC_CON0, 0x00b4);
	ana_write(a, AUDDIGMI_CON0, 0x0080);
	ana_write(a, AUD_NCP0, ncp0_value(a));
	/*
	 * AUDLSBUF_CON0[1:0] power the line-in level-shift buffers. Nothing in
	 * this driver turns them on; the stock HAL's ADC close clears them, so
	 * this does too, in case LK or a test left them on.
	 */
	ana_update(a, AUDLSBUF_CON0, 0, 0x0003);
	if (!a->playing) {
		ana_write(a, AUDNVREGGLB_CFG0, 0x0006);
		ana_write(a, AUDLDO_CFG0, 0x0192);
	}
	ana_update(a, AUDCLKGEN_CFG0, 0, CLKGEN_ADC);
	ana_write(a, AFE_UL_SRC_CON0_L, 0x0000);
	if (!a->playing)
		ana_write(a, AFE_UL_DL_CON0, 0x0000);
}

static int b8k_play_start(struct b8k_audio *a, struct snd_pcm_runtime *rt)
{
	int ret;

	mutex_lock(&a->power_lock);
	b8k_afe_get(a);
	writel(lower_32_bits(rt->dma_addr), a->afe + AFE_DL1_BASE);
	writel(lower_32_bits(rt->dma_addr + snd_pcm_lib_buffer_bytes(a->play.substream) - 1),
	       a->afe + AFE_DL1_END);
	/* DL1 L/R (I05/I06) to PMIC DAC L/R (O03/O04). */
	afe_update(a, AFE_CONN1, BIT(21), BIT(21));
	afe_update(a, AFE_CONN2, BIT(6), BIT(6));
	afe_update(a, AFE_DAC_CON1, 10, 0xf); /* 48 kHz */
	afe_update(a, AFE_I2S_CON1, (10 << 8) | 1, 0xf01);

	a->playing = true;
	ret = b8k_analog_on(a);
	if (ret) {
		a->playing = false;
		afe_update(a, AFE_CONN1, 0, BIT(21));
		afe_update(a, AFE_CONN2, 0, BIT(6));
		b8k_afe_put(a);
		mutex_unlock(&a->power_lock);
		return ret;
	}
	afe_update(a, AFE_DAC_CON0, AFE_ON | DL1_ON, AFE_ON | DL1_ON);
	mutex_unlock(&a->power_lock);
	return 0;
}

static void b8k_play_stop(struct b8k_audio *a)
{
	mutex_lock(&a->power_lock);
	afe_update(a, AFE_DAC_CON0, 0, DL1_ON);
	a->playing = false;
	b8k_analog_off(a);
	afe_update(a, AFE_CONN1, 0, BIT(21));
	afe_update(a, AFE_CONN2, 0, BIT(6));
	b8k_afe_put(a);
	mutex_unlock(&a->power_lock);
}

static int b8k_cap_start(struct b8k_audio *a, struct snd_pcm_runtime *rt)
{
	u32 rc = afe_rate_code(rt->rate);
	int ret;

	mutex_lock(&a->power_lock);
	b8k_afe_get(a);
	a->capturing = true;
	ret = b8k_mic_on(a, rt->rate);
	if (ret) {
		a->capturing = false;
		b8k_mic_off(a);
		b8k_afe_put(a);
		mutex_unlock(&a->power_lock);
		return ret;
	}

	writel(lower_32_bits(rt->dma_addr), a->afe + AFE_VUL_BASE);
	writel(lower_32_bits(rt->dma_addr + snd_pcm_lib_buffer_bytes(a->cap.substream) - 1),
	       a->afe + AFE_VUL_END);
	/* Mono takes the left channel: preamp L, the same MIC1 as R. */
	afe_update(a, AFE_DAC_CON1, (rc << VUL_MODE_SHIFT) |
		   (rt->channels == 1 ? VUL_MONO : 0),
		   VUL_MODE_MASK | VUL_MONO | VUL_R_MONO);
	/*
	 * I2S in from the MT6320 ADC (AudioDigitalControl::SetI2SAdcIn): buffer
	 * update word 8, I2S format, 16-bit, then the enable bit.
	 */
	writel((8 << 24) | (rc << 8) | BIT(3), a->afe + AFE_I2S_CON2);
	afe_update(a, AFE_I2S_CON2, 1, 1);
	/* ADC L/R (I03/I04) to VUL L/R (O09/O10). */
	afe_update(a, AFE_CONN3, BIT(0) | BIT(3), BIT(0) | BIT(3));
	afe_update(a, AFE_DAC_CON0, AFE_ON | VUL_ON, AFE_ON | VUL_ON);
	mutex_unlock(&a->power_lock);
	return 0;
}

static void b8k_cap_stop(struct b8k_audio *a)
{
	mutex_lock(&a->power_lock);
	afe_update(a, AFE_DAC_CON0, 0, VUL_ON);
	afe_update(a, AFE_I2S_CON2, 0, 1);
	afe_update(a, AFE_CONN3, 0, BIT(0) | BIT(3));
	a->capturing = false;
	b8k_mic_off(a);
	b8k_afe_put(a);
	mutex_unlock(&a->power_lock);
}

static struct b8k_stream *b8k_stream_of(struct b8k_audio *a,
					struct snd_pcm_substream *substream)
{
	return substream->stream == SNDRV_PCM_STREAM_CAPTURE ? &a->cap : &a->play;
}

static snd_pcm_uframes_t b8k_hw_pos(struct b8k_audio *a, struct b8k_stream *st,
				    struct snd_pcm_substream *s)
{
	u32 base = lower_32_bits(s->runtime->dma_addr);
	u32 cur = readl(a->afe + st->cur_reg);

	if (cur < base || cur >= base + snd_pcm_lib_buffer_bytes(s))
		return 0;
	return cur - base;
}

/*
 * The PCM is nonatomic (its trigger sleeps on the PMIC bus), so period
 * notifications go through a work item rather than the hrtimer callback.
 */
static void b8k_elapsed_work(struct work_struct *work)
{
	struct b8k_stream *st = container_of(work, struct b8k_stream, elapsed);
	struct snd_pcm_substream *s = READ_ONCE(st->substream);

	if (s && READ_ONCE(st->running))
		snd_pcm_period_elapsed(s);
}

static enum hrtimer_restart b8k_timer(struct hrtimer *timer)
{
	struct b8k_stream *st = container_of(timer, struct b8k_stream, timer);
	struct b8k_audio *a;
	struct snd_pcm_substream *s;
	unsigned long flags;
	u32 period;

	s = READ_ONCE(st->substream);
	if (!s)
		return HRTIMER_NORESTART;
	a = snd_pcm_substream_chip(s);
	spin_lock_irqsave(&a->lock, flags);
	if (!st->running) {
		spin_unlock_irqrestore(&a->lock, flags);
		return HRTIMER_NORESTART;
	}
	period = b8k_hw_pos(a, st, s) / snd_pcm_lib_period_bytes(s);
	if (period != st->last_period) {
		st->last_period = period;
		queue_work(system_highpri_wq, &st->elapsed);
	}
	hrtimer_forward_now(timer, ms_to_ktime(2));
	spin_unlock_irqrestore(&a->lock, flags);
	return HRTIMER_RESTART;
}

static int b8k_pcm_open(struct snd_pcm_substream *substream)
{
	struct b8k_audio *a = snd_pcm_substream_chip(substream);
	struct b8k_stream *st = b8k_stream_of(a, substream);

	int ret;

	substream->runtime->hw =
		substream->stream == SNDRV_PCM_STREAM_CAPTURE ? b8k_capture_hw : b8k_pcm_hw;
	/* Keep buffer and period sizes in whole 16-byte memif units. */
	ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
					 SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 16);
	if (!ret)
		ret = snd_pcm_hw_constraint_step(substream->runtime, 0,
						 SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret)
		return ret;
	st->substream = substream;
	return 0;
}

static int b8k_pcm_close(struct snd_pcm_substream *substream)
{
	struct b8k_audio *a = snd_pcm_substream_chip(substream);
	struct b8k_stream *st = b8k_stream_of(a, substream);

	hrtimer_cancel(&st->timer);
	cancel_work_sync(&st->elapsed);
	st->substream = NULL;
	return 0;
}

static int b8k_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct b8k_audio *a = snd_pcm_substream_chip(substream);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		memset(substream->runtime->dma_area, 0, substream->runtime->dma_bytes);
	b8k_stream_of(a, substream)->last_period = 0;
	return 0;
}

static int b8k_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct b8k_audio *a = snd_pcm_substream_chip(substream);
	struct b8k_stream *st = b8k_stream_of(a, substream);
	bool capture = substream->stream == SNDRV_PCM_STREAM_CAPTURE;
	unsigned long flags;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		ret = capture ? b8k_cap_start(a, substream->runtime) :
				b8k_play_start(a, substream->runtime);
		if (ret)
			return ret;
		spin_lock_irqsave(&a->lock, flags);
		st->running = true;
		spin_unlock_irqrestore(&a->lock, flags);
		hrtimer_start(&st->timer, ms_to_ktime(2), HRTIMER_MODE_REL);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&a->lock, flags);
		st->running = false;
		spin_unlock_irqrestore(&a->lock, flags);
		hrtimer_cancel(&st->timer);
		if (capture)
			b8k_cap_stop(a);
		else
			b8k_play_stop(a);
		return 0;
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t b8k_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct b8k_audio *a = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime,
			       b8k_hw_pos(a, b8k_stream_of(a, substream), substream));
}

static const struct snd_pcm_ops b8k_pcm_ops = {
	.open = b8k_pcm_open,
	.close = b8k_pcm_close,
	.prepare = b8k_pcm_prepare,
	.trigger = b8k_pcm_trigger,
	.pointer = b8k_pcm_pointer,
};

static int b8k_volume_info(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = ARRAY_SIZE(hp_gain_table) - 1;
	return 0;
}

static int b8k_volume_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct b8k_audio *a = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = a->volume;
	return 0;
}

static int b8k_volume_put(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	struct b8k_audio *a = snd_kcontrol_chip(kcontrol);
	unsigned int v = clamp_val(ucontrol->value.integer.value[0], 0,
				   ARRAY_SIZE(hp_gain_table) - 1);
	int changed;

	mutex_lock(&a->power_lock);
	changed = v != a->volume;
	a->volume = v;
	if (a->playing)
		ana_write(a, ZCD_CON2, hp_gain_table[v]);
	mutex_unlock(&a->power_lock);
	return changed;
}

static const struct snd_kcontrol_new b8k_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker Playback Volume",
	.info = b8k_volume_info,
	.get = b8k_volume_get,
	.put = b8k_volume_put,
};

static const DECLARE_TLV_DB_SCALE(b8k_mic_tlv, 0, 600, 0);

static int b8k_mic_gain_info(struct snd_kcontrol *kcontrol,
			     struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = MIC_GAIN_MAX;
	return 0;
}

static int b8k_mic_gain_get(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct b8k_audio *a = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = a->mic_gain;
	return 0;
}

static int b8k_mic_gain_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	struct b8k_audio *a = snd_kcontrol_chip(kcontrol);
	unsigned int v = clamp_val(ucontrol->value.integer.value[0], 0,
				   MIC_GAIN_MAX);
	int changed;

	mutex_lock(&a->power_lock);
	changed = v != a->mic_gain;
	a->mic_gain = v;
	if (a->capturing)
		ana_update(a, AUDPREAMPGAIN_CON0, v | (v << 4), 0x0077);
	mutex_unlock(&a->power_lock);
	return changed;
}

static const struct snd_kcontrol_new b8k_mic_gain_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Mic Capture Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = b8k_mic_gain_info,
	.get = b8k_mic_gain_get,
	.put = b8k_mic_gain_put,
	.tlv.p = b8k_mic_tlv,
};

static void b8k_stream_init(struct b8k_stream *st, u32 cur_reg)
{
	st->cur_reg = cur_reg;
	hrtimer_setup(&st->timer, b8k_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	INIT_WORK(&st->elapsed, b8k_elapsed_work);
}

static int b8k_audio_probe(struct platform_device *pdev)
{
	struct b8k_audio *a;
	struct device *pwrap;
	int ret;

	a = devm_kzalloc(&pdev->dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = &pdev->dev;
	/* Full analog gain; PipeWire/PulseAudio scales in software. */
	a->volume = ARRAY_SIZE(hp_gain_table) - 1;
	a->mic_gain = MIC_GAIN_MAX;
	spin_lock_init(&a->lock);
	mutex_init(&a->power_lock);
	b8k_stream_init(&a->play, AFE_DL1_CUR);
	b8k_stream_init(&a->cap, AFE_VUL_CUR);

	a->afe = devm_ioremap(&pdev->dev, AFE_PHYS, AFE_SIZE);
	if (!a->afe)
		return -ENOMEM;
	a->infra = devm_ioremap(&pdev->dev, INFRACFG_PHYS, INFRACFG_SIZE);
	if (!a->infra)
		return -ENOMEM;
	pwrap = bus_find_device_by_name(&platform_bus_type, NULL, "1000f000.pwrap");
	if (!pwrap)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "cannot find pwrap device\n");
	a->pmic = dev_get_regmap(pwrap, NULL);
	put_device(pwrap);
	if (!a->pmic)
		return dev_err_probe(&pdev->dev, -EPROBE_DEFER,
				     "cannot get pwrap regmap\n");
	a->speaker_enable = devm_gpiod_get(&pdev->dev, "speaker-enable",
					  GPIOD_OUT_LOW);
	if (IS_ERR(a->speaker_enable))
		return dev_err_probe(&pdev->dev, PTR_ERR(a->speaker_enable),
				     "cannot request speaker enable GPIO\n");

	ret = snd_card_new(&pdev->dev, -1, CARD_ID, THIS_MODULE, 0, &a->card);
	if (ret)
		return ret;
	strscpy(a->card->driver, "mt6589-audio", sizeof(a->card->driver));
	strscpy(a->card->shortname, "Lenovo B8000-H", sizeof(a->card->shortname));
	strscpy(a->card->longname, "MT6589 AFE + MT6320 + SN2145B",
		sizeof(a->card->longname));
	ret = snd_pcm_new(a->card, "MT6589", 0, 1, 1, &a->pcm);
	if (ret)
		goto err_card;
	a->pcm->private_data = a;
	a->pcm->nonatomic = true;
	strscpy(a->pcm->name, "MT6589", sizeof(a->pcm->name));
	snd_pcm_set_ops(a->pcm, SNDRV_PCM_STREAM_PLAYBACK, &b8k_pcm_ops);
	snd_pcm_set_ops(a->pcm, SNDRV_PCM_STREAM_CAPTURE, &b8k_pcm_ops);
	snd_pcm_set_managed_buffer(a->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream,
				   SNDRV_DMA_TYPE_DEV, &pdev->dev,
				   BUFFER_BYTES, BUFFER_BYTES);
	snd_pcm_set_managed_buffer(a->pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream,
				   SNDRV_DMA_TYPE_DEV, &pdev->dev,
				   CAPTURE_BUFFER_BYTES, CAPTURE_BUFFER_BYTES);
	ret = snd_ctl_add(a->card, snd_ctl_new1(&b8k_volume_control, a));
	if (ret)
		goto err_card;
	ret = snd_ctl_add(a->card, snd_ctl_new1(&b8k_mic_gain_control, a));
	if (ret)
		goto err_card;
	ret = snd_card_register(a->card);
	if (ret)
		goto err_card;
	platform_set_drvdata(pdev, a);
	dev_info(&pdev->dev, "ALSA card registered (speaker, microphone)\n");
	return 0;

err_card:
	snd_card_free(a->card);
	return ret;
}

static void b8k_audio_remove(struct platform_device *pdev)
{
	struct b8k_audio *a = platform_get_drvdata(pdev);

	snd_card_free(a->card);
}

/*
 * System sleep (s2idle). The ALSA core's PCM device PM already suspends a
 * running stream (trigger SUSPEND: DMA, amplifier and analog path off, the
 * period timer stopped), which stopped the ~85 ms loop the DMA played with
 * the amplifier on. Here the card's power state follows: D3hot while
 * asleep, so nothing reopens or starts a stream before the wake, and D0
 * after it, when the sound server prepares and restarts the stream (no
 * SNDRV_PCM_INFO_RESUME). snd_pcm_suspend_all() is kept for kernels whose
 * core does not do it; a second call finds nothing running.
 */
static int b8k_audio_suspend(struct device *dev)
{
	struct b8k_audio *a = dev_get_drvdata(dev);

	snd_power_change_state(a->card, SNDRV_CTL_POWER_D3hot);
	snd_pcm_suspend_all(a->pcm);
	return 0;
}

static int b8k_audio_resume(struct device *dev)
{
	struct b8k_audio *a = dev_get_drvdata(dev);

	snd_power_change_state(a->card, SNDRV_CTL_POWER_D0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(b8k_audio_pm_ops, b8k_audio_suspend,
				b8k_audio_resume);

static struct platform_driver b8k_audio_driver = {
	.probe = b8k_audio_probe,
	.remove = b8k_audio_remove,
	.driver = {
		.name = DRV_NAME,
		.pm = pm_sleep_ptr(&b8k_audio_pm_ops),
	},
};

static int __init b8k_audio_init(void)
{
	int ret;

	gpiod_add_lookup_table(&b8k_gpio_table);
	ret = platform_driver_register(&b8k_audio_driver);
	if (ret) {
		gpiod_remove_lookup_table(&b8k_gpio_table);
		return ret;
	}
	b8k_pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(b8k_pdev)) {
		ret = PTR_ERR(b8k_pdev);
		platform_driver_unregister(&b8k_audio_driver);
		gpiod_remove_lookup_table(&b8k_gpio_table);
		return ret;
	}
	return 0;
}

static void __exit b8k_audio_exit(void)
{
	platform_device_unregister(b8k_pdev);
	platform_driver_unregister(&b8k_audio_driver);
	gpiod_remove_lookup_table(&b8k_gpio_table);
}

module_init(b8k_audio_init);
module_exit(b8k_audio_exit);

MODULE_DESCRIPTION("Lenovo B8000-H MT6589/MT6320 minimal ALSA playback and capture");
MODULE_AUTHOR("kz-creator");
MODULE_LICENSE("GPL");
