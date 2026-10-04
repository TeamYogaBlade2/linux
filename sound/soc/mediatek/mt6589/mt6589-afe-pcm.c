// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 AFE platform driver.
 *
 * DL1 playback front-end feeding the ADDA downlink SRC and the AFE<->PMIC
 * serial link to the mt6320 codec. The AFE registers are in the parent audsys
 * syscon; a fast_io regmap keeps the trigger and the period IRQ atomic.
 *
 * based on mt6572-afe-pcm.c
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

/* AFE registers (classic mt65xx layout); stock magic values noted inline. */
#define AUDIO_TOP_CON0		0x0000
/*
 * AUDIO_TOP_CON0 power bits: PDN_AFE at bit 2 and PDN_I2S at bit 6, both
 * active-low ("0: Power on").  Bit 14 is APB3_SEL, the APB protocol select,
 * and is left alone.  These are the same bits the clock driver gates
 * CLK_AUDIO_AFE and CLK_AUDIO_I2S on, so the AFE must not touch them
 * directly - see mt6589_afe_pcm_dev_probe().
 */
#define AUDIO_TOP_CON0_PDN_AFE		BIT(2)
#define AUDIO_TOP_CON0_PDN_I2S		BIT(6)
#define AFE_DAC_CON0		0x0010
#define AFE_DAC_CON0_AFE_ON	BIT(0)
#define AFE_DAC_CON0_DL1_ON	BIT(1)
/* AFE_DAC_CON0 per-memif enables: DL1 bit1, DL2 bit2, VUL bit3, AWB bit4.
 * The downstream driver uses 1 << (block + 1) with MEM_DL1 == 0.
 */
#define AFE_DAC_CON0_VUL_ON	BIT(3)
#define AFE_DAC_CON1		0x0014
#define AFE_DAC_CON1_DL1_RATE	GENMASK(3, 0)
#define AFE_DAC_CON1_VUL_RATE	GENMASK(19, 16)
/*
 * AFE_DAC_CON1 is a bank of mode fields, one 4-bit slice per memory
 * interface, followed by the per-interface data/mono bits:
 *
 *	[3:0]   DL1_MODE	[7:4]   DL2_MODE	[11:8]  I2S_MODE
 *	[15:12] AWB_MODE	[19:16] VUL_MODE	[20]    DAI_MODE
 *	[21]    DL1_DATA	[22]    DL2_DATA	[23]    I2S_DATA
 *	[24]    AWB_DATA	[25]    AWB_R_MONO	[27]    VUL_DATA
 *	[28]    VUL_R_MONO
 *
 * VUL_R_MONO is bit 28 - bit 27 is VUL_DATA, the VUL data width.  Writing
 * bit 27 here set the data width instead of the right-justification flag,
 * which the stock driver never does for a 16-bit stream.
 */
#define AFE_DAC_CON1_VUL_DATA	BIT(27)
#define AFE_DAC_CON1_VUL_R_MONO	BIT(28)
#define AFE_VUL_BASE		0x0080
#define AFE_VUL_CUR		0x008c
#define AFE_VUL_END		0x0088		/* ring end, inclusive */

/*
 * There is no ADDA block on MT6589.  AFE_ADDA_TOP_CON0 (+0x120) and
 * AFE_ADDA_UL_SRC_CON0 (+0x114) are MT6797 registers: this part's AFE map
 * has nothing between +0x00e0 (AFE_MEMIF_MON4) and +0x0170 (AFE_FOC_CON),
 * in both the data sheet and the vendor header.  Source selection is done
 * with the interconnect CONN registers below.
 */

#define AFE_DL1_BASE		0x0040
#define AFE_DL1_CUR		0x0044
#define AFE_DL1_END		0x0048		/* ring end, inclusive */

/*
 * DL2, AWB, DAI, VUL - present in the hardware, deliberately not driven.
 *
 * Register map (AudDrv_Afe.h:449-463, the AFE's own map, not the codec's):
 *
 *	DL1  BASE/CUR/END  +0x40 / +0x44 / +0x48	driven by this driver
 *	DL2  BASE/CUR/END  +0x50 / +0x54 / +0x58	not driven
 *	AWB  BASE/END/CUR  +0x70 / +0x78 / +0x7c	not driven
 *	VUL  BASE/END/CUR  +0x80 / +0x88 / +0x8c	driven by this driver
 *	DAI  BASE/END/CUR  +0x90 / +0x98 / +0x9c	not driven
 *
 * Note DL2 is at +0x50..+0x58, NOT +0x80..; +0x80/+0x8c is VUL, which this
 * driver already uses for capture.  Anyone adding DL2 by copying the VUL
 * offsets would silently overwrite the capture ring registers.
 *
 * DL2 is deliberately left unimplemented rather than wired up, because nothing
 * in the vendor tree ever drives it as a stream:
 *
 *	- it has no dai_link at all (no reference in mt_soc_dai_routing.c or
 *	  mt_soc_pcm_routing.c), so there is no PCM to attach to it;
 *	- the only mention in the playback driver is
 *	  mt_soc_pcm_afe.c:573, which sets DL2's *fetch format* while starting
 *	  DL1 - it never enables DL2;
 *	- DL2 has no channel/mono configuration: SetChannels()
 *	  (mt_soc_afe_control.c:468-487) handles AWB and VUL only and returns
 *	  false from its default branch for MEM_DL1 and MEM_DL2 alike.
 *
 * Its enable bit does exist (AFE_DAC_CON0, DL2_ON = bit 2,
 * AudDrv_Afe.h:574) and its rate lives in AFE_DAC_CON1[7:4], so a future
 * second-output path is possible, but it would be untestable dead code today.
 *
 * AWB (asynchronous write buffer, the FM/modem output interface) and DAI are
 * likewise absent.  The vendor FM radio path is kernel/sound/soc/__mediatek/
 * mt_soc_fm_i2s2.c, which has no mainline equivalent; wiring it up would need
 * a second AFE output, the 2nd I2S input and an MT6320 input, none of which
 * this card's DT or DAI links describe.  Left out on purpose.
 */
#define AFE_IRQ_MCU_CON		0x03a0
#define AFE_IRQ_MCU_CON_IRQ1_ON		BIT(0)
#define AFE_IRQ_MCU_CON_IRQ2_ON		BIT(1)
#define AFE_IRQ_MCU_CON_IRQ1_RATE	GENMASK(7, 4)
#define AFE_IRQ_MCU_CON_IRQ2_RATE	GENMASK(11, 8)
#define AFE_IRQ_MCU_STATUS	0x03a4
#define AFE_IRQ_MCU_STATUS_IRQ1	BIT(0)
#define AFE_IRQ_MCU_STATUS_IRQ2	BIT(1)
#define AFE_IRQ_MCU_STATUS_MASK	GENMASK(3, 0)
#define AFE_IRQ_MCU_CLR		0x03a8
#define AFE_IRQ_MCU_CLR_NOSTATUS (BIT(6) | GENMASK(4, 0))
#define AFE_IRQ_MCU_CNT1	0x03ac	/* IRQ1 MCU counter */
#define AFE_IRQ_MCU_CNT2	0x03b0	/* IRQ2 MCU counter */

/* DL1 -> interconnect -> I2S2 DAC path. */
#define AFE_I2S_CON1		0x0034
#define AFE_I2S_CON1_BASE	0x00000008	/* I2S2_FMT: 0=EIAJ, 1=I2S; select I2S */
#define AFE_I2S_CON1_RATE	GENMASK(11, 8)
#define AFE_I2S_CON1_ON		BIT(0)
#define AFE_CONN1		0x0024
#define AFE_CONN1_DL1_O3	BIT(21)		/* DL1 ch1 -> O3 */
#define AFE_CONN2		0x0028
#define AFE_CONN2_DL1_O4	BIT(6)		/* DL1 ch2 -> O4 */
/*
 * I2S ADC -> VUL, the two connections the downstream capture driver makes:
 * I03 -> O09 and I04 -> O10.  Both are in AFE_CONN3, at bit 0 and bit 3
 * respectively.  The downstream mConnection tables agree on the registers
 * and bits; only their output *numbering* is offset by one against the
 * data sheet, which names these bits I03_O09_S and I04_O10_S.
 */
#define AFE_CONN3		0x002c
#define AFE_CONN3_VUL_O9	BIT(0)		/* I03 -> O09 */
#define AFE_CONN3_VUL_O10	BIT(3)		/* I04 -> O10 */

/*
 * The downlink pre-distortion block is real on this part and lives at
 * +0x260, so keep it - only the ADDA registers above it were wrong.
 */
#define AFE_ADDA_PREDIS_CON0	0x0260
#define AFE_ADDA_PREDIS_CON1	0x0264
/*
 * The AFE's own 16 KiB SRAM.  The DL1 and VUL memory interfaces fetch
 * directly out of it with no DMA engine behind them: DL1_BASE/DL1_END are
 * plain physical addresses the AFE dereferences itself.  A buffer anywhere
 * else is never fetched, and the failure is silent - the memif runs, but
 * DL1_CUR never advances past the base while the period interrupt still
 * fires off the counter, so ALSA sees a healthy stream carrying no audio.
 *
 * AFE_INTERNAL_SRAM_PHY_BASE in the stock header is written
 * (AUDIO_HW_PHYSICAL_BASE - 0x70000 + 0x8000), which evaluates to
 * 0x12008000, yet the comment immediately above it states the range is
 * 0x12004000..0x12007fff.  The comment is right: 0x12008000 is not in the
 * audsys window (reg = <0x12070000 0x1000>, ending at 0x12070fff) and no
 * vendor code ever allocates from it, whereas the DT sram@12004000 matches
 * the comment exactly.  So use 0x12004000.
 */
#define AFE_SRAM_PHYS_BASE	0x12004000
#define AFE_SRAM_PHYS_END	0x12007fff	/* inclusive */
/*
 * There is no NEWIF (AFE<->PMIC serial link) register on MT6589.  The
 * offsets this driver used to program - 0x0138 and 0x013c - are MT6797
 * ADDA registers: MT6589's AFE map goes from AFE_MEMIF_MON4 at +0x00e0
 * straight to AFE_FOC_CON at +0x0170 with nothing in between, in both the
 * data sheet and the vendor header.  The values written (0x03f87201 and a
 * GENMASK(11,10) update) come from the same MT6797 code, so both were
 * landing on unbacked addresses.
 *
 * The AFE<->PMIC link on this part is brought up by the MT6320 codec side
 * plus the AFE clocks taken above; nothing needs programming here.
 */

static const struct regmap_config mt6589_afe_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.fast_io = true,
	.max_register = 0x0ffc,
};

static int mt6589_afe_vul_prepare(struct snd_soc_component *comp,
				  struct snd_pcm_substream *substream);
static int mt6589_afe_vul_start(struct snd_soc_component *comp,
				struct snd_pcm_substream *substream);
static int mt6589_afe_vul_stop_substream(struct snd_soc_component *comp,
					 struct snd_pcm_substream *substream);

struct mt6589_afe {
	struct device *dev;
	struct regmap *regmap;
	struct clk *clk;
	struct clk *clk_i2s;
	struct snd_pcm_substream *dl1_substream;	/* active DL1 stream */
	struct snd_pcm_substream *vul_substream;	/* active VUL stream */
};

/*
 * Hz -> the AFE sample-rate code, as SampleRateTransform() produces it
 * downstream (Soc_Aud_I2S_SAMPLERATE_*): 8k=0, 11.025k=1, 12k=2, 16k=3,
 * 22.05k=4, 24k=5, 32k=6, 44.1k=7, 48k=8.
 *
 * Every rate field the driver programs - IRQ_MCU_CON[7:4] and [11:8],
 * DAC_CON1[3:0] for DL1 and [19:16] for VUL, and I2S_CON1[11:8] - takes
 * this same code, because SetMemIfSampleRate() and SetIRQMCUAttribute()
 * both pass their argument through SampleRateTransform() before
 * shifting it into place.  There is no second, sparse table in that path.
 *
 * The data sheet's "6 kHz to 96 kHz" audio figure is not achievable on this
 * part.  SampleRateTransform() - the single function every AFE rate field goes
 * through - has cases for exactly the nine rates below and falls through to
 * `return Soc_Aud_I2S_SAMPLERATE_I2S_44K` for anything else
 * (mt_soc_afe_control.c:374-400).  The shared audio V2 header does define
 * AFE_88K/96K/174K/192K and the AFE kernel driver's own enum repeats them
 * (mediatek/platform/common/hardware/audio/V2/include/AudioStreamAttribute.h:50-61,
 * mt_soc_digital_type.h:256-259), but nothing ever emits those values, so
 * asking for 88.2 or 96 kHz would reach the hardware as 44.1 kHz.
 * The 4-bit rate fields are likewise full at the highest documented code (10),
 * leaving no spare code to extend into.  Those rates are therefore rejected
 * below with -EINVAL, and mt6589_afe_rate_code_sparse() returning an error is
 * what keeps them out of the advertised mask.  There is also no 6 kHz code
 * anywhere in the ladder.
 *
 * 12000 and 24000 *are* encodable (codes 2 and 6) and are offered; see the
 * rate masks below, which have to add them by hand because ALSA gives those
 * two their own bits.
 */

/*
 * Hz -> the AFE's own rate code.
 *
 * This is a *sparse* table, distinct from the dense 0..8 one the PMIC
 * codec uses for its DL SRC2 register (mt6320.c).  The stock driver
 * writes u4SamplingRateConvert[] = {0, 1, 2, 4, 5, 6, 8, 9, 10}
 * (AudioAfe.c) into AFE_DAC_CON1's DL1 mode, AFE_I2S_CON1 and the IRQ
 * counter select.  The two agree only up to 12 kHz, so using the dense
 * code here programs the wrong divider for nearly every rate.
 *
 * The table is verbatim u4SamplingRateConvert[] (AudioAfe.c:66) indexed by the
 * dense rate enum, so the {0,1,2,4,5,6,8,9,10} code sequence must not be
 * "tidied" into 0..8.  Values below 8000 and above 48000 return -EINVAL on
 * purpose - see the rate-transform note above this function.
 */
static int mt6589_afe_rate_code_sparse(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 11025:	return 1;
	case 12000:	return 2;
	case 16000:	return 4;
	case 22050:	return 5;
	case 24000:	return 6;
	case 32000:	return 8;
	case 44100:	return 9;
	case 48000:	return 10;
	default:	return -EINVAL;
	}
}

/*
 * Rates the AFE hardware can encode, as an ALSA rate mask.
 *
 * SNDRV_PCM_RATE_8000_48000 looks like it should cover everything the table
 * above encodes, but it does not contain 12000 or 24000: ALSA assigns those two
 * bits of their own, added long after the range macro was defined
 * (SNDRV_PCM_RATE_12000 = 1U<<17, SNDRV_PCM_RATE_24000 = 1U<<18,
 * include/sound/pcm.h:127-128).  Without the explicit OR they are supported by
 * the hardware and by the ladder above yet unavailable to userspace.
 */
#define MT6589_AFE_RATES	(SNDRV_PCM_RATE_8000_48000 |	\
				 SNDRV_PCM_RATE_12000 |		\
				 SNDRV_PCM_RATE_24000)

static struct snd_soc_dai_driver mt6589_afe_dais[] = {
	{
		.name = "mt6589-afe-dl1",
		.playback = {
			.stream_name = "DL1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = MT6589_AFE_RATES,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
	{
		.name = "mt6589-afe-vul",
		.capture = {
			.stream_name = "VUL Capture",
			.channels_min = 1,
			.channels_max = 2,
			/*
			 * The AFE's own VUL ladder does encode 12 kHz and
			 * 24 kHz, so this side advertises the full AFE set.
			 *
			 * A capture stream is nevertheless limited to
			 * 8/16/32/48 kHz end to end, because the codec's
			 * uplink SRC encodes only those four rates
			 * (MT6320_CODEC_UL_RATES, mt6320_ul_src_rate_code()).
			 * ALSA intersects the per-DAI rate masks of a link, so
			 * that narrower codec mask is what userspace
			 * negotiates against and the two extra rates never
			 * reach hw_params.  They are deliberately not repeated
			 * here as a restriction: the AFE really can encode
			 * them, and it is the codec that cannot.
			 */
			.rates = MT6589_AFE_RATES,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
};

static const struct snd_pcm_hardware mt6589_afe_hardware = {
	/* on-chip SRAM buffer, no mmap */
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = MT6589_AFE_RATES,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 1,
	.channels_max = 2,
	.period_bytes_min = 1024,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 16,
	/*
	 * The AFE SRAM is 16 KB and is shared by both directions. Handing
	 * the whole window to one stream leaves nothing for the other, so
	 * cap it at half. speaker-test asks for 8192 with a 4096 period,
	 * which still fits.
	 */
	.buffer_bytes_max = 8 * 1024,
};

/*
 * Memory-interface power event for the DL1 and VUL widgets.
 *
 * The interconnect bits and the DAC_CON0 enables are programmed in
 * prepare() and trigger(); this only turns the memory interface on and off
 * as DAPM walks the graph, matching what the stock driver does between
 * SetMEMIFEnable() and the stream teardown.
 */
static int mt6589_afe_memif_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(
					snd_soc_dapm_to_component(w->dapm));
	int ret;
	bool capture;

	if (!afe)
		return -ENODEV;

	/* Capture widgets are VUL and "VUL Capture"; the rest are playback. */
	capture = !strcmp(w->name, "VUL") || !strcmp(w->name, "VUL Capture");

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (capture)
			ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
						 AFE_DAC_CON0_VUL_ON,
						 AFE_DAC_CON0_VUL_ON);
		else
			ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
						 AFE_DAC_CON0_DL1_ON,
						 AFE_DAC_CON0_DL1_ON);
		if (ret)
			return ret;

		return regmap_update_bits(afe->regmap, AFE_DAC_CON0,
					 AFE_DAC_CON0_AFE_ON,
					 AFE_DAC_CON0_AFE_ON);
	case SND_SOC_DAPM_POST_PMD:
		if (capture) {
			ret = regmap_clear_bits(afe->regmap, AFE_DAC_CON0,
						AFE_DAC_CON0_VUL_ON);
			if (ret)
				return ret;
		} else {
			ret = regmap_clear_bits(afe->regmap, AFE_DAC_CON0,
						AFE_DAC_CON0_DL1_ON);
			if (ret)
				return ret;
		}

		return regmap_clear_bits(afe->regmap, AFE_DAC_CON0,
					 AFE_DAC_CON0_AFE_ON);
	}

	return 0;
}

static int mt6589_afe_pcm_open(struct snd_soc_component *comp,
			       struct snd_pcm_substream *substream)
{
	snd_soc_set_runtime_hwparams(substream, &mt6589_afe_hardware);
	/*
	 * AFE_DL1_END[2:0] must be 7, so keep the period (and therefore the
	 * buffer) 8-byte aligned, and honour the AFE's 32-byte sample
	 * alignment requirement as well so the ring does not need masking.
	 */
	return snd_pcm_hw_constraint_step(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 32);
}

static int mt6589_afe_pcm_hw_params(struct snd_soc_component *comp,
				    struct snd_pcm_substream *substream,
				    struct snd_pcm_hw_params *params)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int bytes = params_buffer_bytes(params);
	dma_addr_t dma = runtime->dma_addr;
	u64 base = lower_32_bits(dma);
	int ret;

	/*
	 * The AFE has no DMA engine for these ring buffers - it dereferences
	 * DL1_BASE/DL1_END itself - so the buffer has to sit in the AFE SRAM.
	 *
	 * SNDRV_DMA_TYPE_DEV_IRAM cannot be trusted to have put it there:
	 * snd_dma_iram_alloc() falls back to plain dma_alloc_coherent() the
	 * moment the gen_pool lookup or the allocation fails (it does so
	 * silently, only retyping dmab->dev.type to SNDRV_DMA_TYPE_DEV), and
	 * it falls back whenever of_gen_pool_get() cannot resolve the "iram"
	 * phandle to a live pool - including when the sram@ node probed late
	 * or was never instantiated at all, since the phandle then has no
	 * platform_device and gen_pool_get() returns NULL.
	 *
	 * That fallback yields a perfectly ordinary SDRAM buffer that the AFE
	 * can never fetch, and the resulting failure is silent: the stream
	 * opens, periods elapse on schedule off the MCU counter, and the
	 * DAC is fed nothing.  So refuse it loudly instead.
	 */
	if (dma < AFE_SRAM_PHYS_BASE ||
	    dma > AFE_SRAM_PHYS_END ||
	    dma + bytes - 1 > AFE_SRAM_PHYS_END)
		return dev_err_probe(comp->dev, -EINVAL,
				     "PCM buffer at %pa (%u bytes) is outside the AFE SRAM window %pa..%pa; the AFE cannot fetch it\n",
				     &dma, bytes,
				     (phys_addr_t *)AFE_SRAM_PHYS_BASE,
				     (phys_addr_t *)AFE_SRAM_PHYS_END);

	/* Program this direction's memif DMA ring, in the AFE on-chip SRAM. */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = regmap_write(afe->regmap, AFE_DL1_BASE, base);
		if (ret)
			return ret;

		ret = regmap_write(afe->regmap, AFE_DL1_END, base + bytes - 1);
		if (ret)
			return ret;

		/*
		 * DL1_CUR is the memif's read pointer.  The stock driver does
		 * not program it either, but it works around the consequence
		 * twice - in both its ISR and its pointer callback it reads
		 * DL1_CUR and, when it comes back 0, substitutes the buffer
		 * address.  It really can read 0, so do the substitution here
		 * rather than in the callback: seed the register explicitly so
		 * the very first pointer read after start is already sane.
		 */
		return regmap_write(afe->regmap, AFE_DL1_CUR, base);
	}

	ret = regmap_write(afe->regmap, AFE_VUL_BASE, base);
	if (ret)
		return ret;

	ret = regmap_write(afe->regmap, AFE_VUL_END, base + bytes - 1);
	if (ret)
		return ret;

	return 0;
}

static int mt6589_afe_pcm_prepare(struct snd_soc_component *comp,
				  struct snd_pcm_substream *substream)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int sparse_code = mt6589_afe_rate_code_sparse(runtime->rate);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return mt6589_afe_vul_prepare(comp, substream);
	int ret;

	if (sparse_code < 0)
		return -EINVAL;

	/* IRQ1 rate + per-period frame count (enabled in the trigger) */
	ret = regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
				 AFE_IRQ_MCU_CON_IRQ1_RATE,
				 FIELD_PREP(AFE_IRQ_MCU_CON_IRQ1_RATE,
					    sparse_code));
	if (ret)
		return ret;

	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CNT1,
			   runtime->period_size);
	if (ret)
		return ret;

	/* interconnect: DL1 ch1/ch2 -> O3/O4 */
	ret = regmap_set_bits(afe->regmap, AFE_CONN1, AFE_CONN1_DL1_O3);
	if (ret)
		return ret;

	ret = regmap_set_bits(afe->regmap, AFE_CONN2, AFE_CONN2_DL1_O4);
	if (ret)
		return ret;

	ret = regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON0, 0);
	if (ret)
		return ret;

	ret = regmap_write(afe->regmap, AFE_ADDA_PREDIS_CON1, 0);
	if (ret)
		return ret;

	/*
	 * No sample-rate conversion block is programmed here.  Normal DL1
	 * playback does not go through the ASRC: the stock driver starts
	 * I2S_OUT_DAC by connecting I05 -> O03 and I06 -> O04 (the
	 * connections set above), enabling the memory interface, and turning
	 * on the I2S DAC.  The rate itself goes to the DAC through
	 * DAC_CON1, which is programmed further down.
	 */
	ret = regmap_write(afe->regmap, AFE_I2S_CON1,
			   AFE_I2S_CON1_BASE |
			   FIELD_PREP(AFE_I2S_CON1_RATE, sparse_code));
	if (ret)
		return ret;

	/*
	 * DAC_CON1 carries the DL1 memory-interface rate in bits [3:0].
	 * SetMemIfSampleRate() writes it there for MEM_DL1, and passes it
	 * u4SamplingRateConvert[] - the same sparse table used above.
	 */
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1,
				 AFE_DAC_CON1_DL1_RATE,
				 FIELD_PREP(AFE_DAC_CON1_DL1_RATE,
					    sparse_code));
	if (ret)
		return ret;

	return 0;
}

static int mt6589_afe_stop(struct mt6589_afe *afe)
{
	int ret, first_err = 0;

	ret = regmap_clear_bits(afe->regmap, AFE_IRQ_MCU_CON,
				AFE_IRQ_MCU_CON_IRQ1_ON);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_DAC_CON0,
				AFE_DAC_CON0_DL1_ON);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_CONN1,
				AFE_CONN1_DL1_O3);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_CONN2,
				AFE_CONN2_DL1_O4);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_I2S_CON1,
				AFE_I2S_CON1_ON);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_DAC_CON0,
				AFE_DAC_CON0_AFE_ON);
	if (ret && !first_err)
		first_err = ret;

	/*
	 * Ack a period interrupt that arrived after IRQ1 was disabled above.
	 * Leaving it latched makes it fire again as soon as the next stream
	 * re-enables IRQ1, where the handler would report a period elapsed
	 * for a stream that has not started: the hw pointer jumps, and ALSA
	 * can see an xrun on the first buffer after a stop/start.
	 */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR,
			   AFE_IRQ_MCU_CLR_NOSTATUS);
	if (ret && !first_err)
		first_err = ret;

	afe->dl1_substream = NULL;
	return first_err;
}

static int mt6589_afe_pcm_trigger(struct snd_soc_component *comp,
				  struct snd_pcm_substream *substream, int cmd)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);
	int ret;

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		switch (cmd) {
		case SNDRV_PCM_TRIGGER_START:
		case SNDRV_PCM_TRIGGER_RESUME:
			return mt6589_afe_vul_start(comp, substream);
		case SNDRV_PCM_TRIGGER_STOP:
		case SNDRV_PCM_TRIGGER_SUSPEND:
			return mt6589_afe_vul_stop_substream(comp, substream);
		default:
			return -EINVAL;
		}
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		/*
		 * Publish the substream before enabling IRQ1 so the first
		 * period interrupt cannot race with this assignment.
		 */
		afe->dl1_substream = substream;

		/* Match the stock SetI2SDacEnable()/EnableAfe() ordering. */
		ret = regmap_set_bits(afe->regmap, AFE_I2S_CON1,
				      AFE_I2S_CON1_ON);
		if (ret)
			goto err_stop;

		/*
		 * Start the DL1 memory path before the period interrupt is
		 * enabled, as the stock mtk_pcm_dl1_start() does, so a
		 * period interrupt cannot arrive against a stopped DL1.
		 */
		ret = regmap_set_bits(afe->regmap, AFE_DAC_CON0,
				      AFE_DAC_CON0_DL1_ON);
		if (ret)
			goto err_stop;

		ret = regmap_set_bits(afe->regmap, AFE_IRQ_MCU_CON,
				      AFE_IRQ_MCU_CON_IRQ1_ON);
		if (ret)
			goto err_stop;

		/* EnableAfe() is last in the stock start sequence. */
		ret = regmap_set_bits(afe->regmap, AFE_DAC_CON0,
				      AFE_DAC_CON0_AFE_ON);
		if (ret)
			goto err_stop;

		return 0;

err_stop:
		mt6589_afe_stop(afe);
		return ret;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		return mt6589_afe_stop(afe);
	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t mt6589_afe_pcm_pointer(struct snd_soc_component *comp,
						struct snd_pcm_substream *substream)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 base = lower_32_bits(runtime->dma_addr);
	unsigned int cur = 0;

	regmap_read(afe->regmap,
		    substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
		    AFE_VUL_CUR : AFE_DL1_CUR, &cur);

	/*
	 * DL1_CUR legitimately reads back 0 until the memif has started
	 * fetching; the stock driver treats that one value specially and
	 * substitutes the buffer address rather than reporting a wrapped
	 * pointer.  hw_params now seeds the register, but keep the same
	 * substitution here so a hardware reset under a running stream
	 * cannot turn into a wild pointer.
	 */
	if (!cur)
		cur = base;

	if (cur < base || cur >= base + runtime->dma_bytes)
		return 0;
	return bytes_to_frames(runtime, cur - base);
}

static int mt6589_afe_pcm_new(struct snd_soc_component *comp,
				    struct snd_soc_pcm_runtime *rtd)
{
	size_t size = mt6589_afe_hardware.buffer_bytes_max;
	struct gen_pool *pool;

	/*
	 * The ring is not allocated yet - snd_pcm_lib_malloc_pages() runs
	 * from snd_pcm_hw_params(), not from here - but the AFE SRAM pool it
	 * will come from has to exist.  snd_dma_iram_alloc() silently degrades
	 * to ordinary SDRAM when it does not, and the AFE cannot fetch SDRAM,
	 * so refuse to create the PCM at all rather than hand the user a
	 * stream that opens and then produces nothing.
	 */
	pool = of_gen_pool_get(comp->dev->of_node, "iram", 0);
	if (!pool)
		return dev_err_probe(comp->dev, -ENODEV,
				     "no gen_pool for the AFE SRAM (iram phandle)\n");

	if (gen_pool_avail(pool) < size)
		return dev_err_probe(comp->dev, -ENOMEM,
				     "AFE SRAM has %zu bytes free, need %zu\n",
				     gen_pool_avail(pool), size);

	return snd_pcm_set_managed_buffer_all(rtd->pcm,
					      SNDRV_DMA_TYPE_DEV_IRAM,
					      comp->dev, size, size);
}

static int mt6589_afe_vul_stop(struct mt6589_afe *afe);
static int mt6589_afe_vul_stop_substream(struct snd_soc_component *comp,
					 struct snd_pcm_substream *substream);

/* VUL capture: internal ADC -> I2S in -> VUL memif. */
static int mt6589_afe_vul_prepare(struct snd_soc_component *comp,
				  struct snd_pcm_substream *substream)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	/* AFE rate fields use the sparse table, as SetMemIfSampleRate() does. */
	int rate_code = mt6589_afe_rate_code_sparse(runtime->rate);
	int ret;

	/*
	 * Mirror the playback-side check in mt6589_afe_pcm_prepare(): that
	 * function returns to vul_prepare() before its own check, so without
	 * this a negative code reaches FIELD_PREP().  FIELD_PREP only masks
	 * the field, it does not reject the value, so -EINVAL would be
	 * truncated into the 4-bit VUL rate field and program a plausible
	 * looking but wrong divider.
	 */
	if (rate_code < 0)
		return -EINVAL;

	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON1,
				 AFE_DAC_CON1_VUL_RATE,
				 FIELD_PREP(AFE_DAC_CON1_VUL_RATE, rate_code));
	if (ret)
		return ret;

	/* One VUL buffer holds a single interleaved stream. */
	return regmap_set_bits(afe->regmap, AFE_DAC_CON1,
			       AFE_DAC_CON1_VUL_R_MONO);
}

static int mt6589_afe_vul_start(struct snd_soc_component *comp,
				struct snd_pcm_substream *substream)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u32 base = lower_32_bits(runtime->dma_addr);
	int rate_code = mt6589_afe_rate_code_sparse(runtime->rate);
	int ret;

	/* Same guard as in vul_prepare(): this recomputes the code itself. */
	if (rate_code < 0)
		return -EINVAL;

	afe->vul_substream = substream;

	ret = regmap_write(afe->regmap, AFE_VUL_CUR, base);
	if (ret)
		goto err;

	ret = regmap_update_bits(afe->regmap, AFE_IRQ_MCU_CON,
				 AFE_IRQ_MCU_CON_IRQ2_RATE,
				 FIELD_PREP(AFE_IRQ_MCU_CON_IRQ2_RATE, rate_code));
	if (ret)
		goto err;

	/* IRQ2 counts into its own counter register, not IRQ1's. */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CNT2,
			   runtime->period_size);
	if (ret)
		goto err;

	/* Route the I2S ADC channels into the VUL memory interface. */
	ret = regmap_set_bits(afe->regmap, AFE_CONN3, AFE_CONN3_VUL_O9);
	if (ret)
		goto err;

	ret = regmap_set_bits(afe->regmap, AFE_CONN3, AFE_CONN3_VUL_O10);
	if (ret)
		goto err;

	/* Start the memif before the interrupt, as the DL1 path does. */
	ret = regmap_update_bits(afe->regmap, AFE_DAC_CON0,
				 AFE_DAC_CON0_VUL_ON, AFE_DAC_CON0_VUL_ON);
	if (ret)
		goto err;

	ret = regmap_set_bits(afe->regmap, AFE_IRQ_MCU_CON,
			      AFE_IRQ_MCU_CON_IRQ2_ON);
	if (ret)
		goto err;

	return regmap_set_bits(afe->regmap, AFE_DAC_CON0,
			       AFE_DAC_CON0_AFE_ON);

err:
	mt6589_afe_vul_stop(afe);
	return ret;
}

static int mt6589_afe_vul_stop(struct mt6589_afe *afe)
{
	int ret, first_err = 0;

	ret = regmap_clear_bits(afe->regmap, AFE_IRQ_MCU_CON,
				AFE_IRQ_MCU_CON_IRQ2_ON);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_CONN3, AFE_CONN3_VUL_O9);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_CONN3, AFE_CONN3_VUL_O10);
	if (ret && !first_err)
		first_err = ret;

	ret = regmap_clear_bits(afe->regmap, AFE_DAC_CON0,
				AFE_DAC_CON0_VUL_ON);
	if (ret && !first_err)
		first_err = ret;

	/*
	 * Ack a period that landed after IRQ2 was disabled, so it does not
	 * fire again on the next start.
	 */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR,
			   AFE_IRQ_MCU_CLR_NOSTATUS);
	if (ret && !first_err)
		first_err = ret;

	afe->vul_substream = NULL;
	return first_err;
}

static int mt6589_afe_vul_stop_substream(struct snd_soc_component *comp,
					 struct snd_pcm_substream *substream)
{
	struct mt6589_afe *afe = snd_soc_component_get_drvdata(comp);

	return mt6589_afe_vul_stop(afe);
}

/*
 * DAPM graph.
 *
 * Without these the codec's routes have nothing to attach to: a route
 * naming a widget that does not exist can never be walked, so the DAC is
 * never powered up and mt6320_dac_event() never runs.  That is what kept
 * the whole analog side dead.
 *
 * The memory interfaces feeding the interconnect are "DL1" and "VUL", and
 * the interconnect itself runs DL1 left/right into I05/I06 and out to
 * O03/O04 - the path the stock driver builds with SetinputConnection(I05,
 * O03) and SetinputConnection(I06, O04).
 */
static const struct snd_soc_dapm_widget mt6589_afe_widgets[] = {
	/*
	 * DL1 and VUL are the memory interfaces.  They carry an event
	 * handler that powers DAC_CON0 as DAPM walks the graph, mirroring
	 * what the stock driver does around SetMEMIFEnable().
	 *
	 * The stream endpoints themselves ("DL1 Playback", "VUL Capture")
	 * need no widget of their own: a DAI gets an auto-created one named
	 * after its stream_name, and dapm_connect_dai_pair() joins the codec
	 * to the AFE through exactly those.
	 */
	SND_SOC_DAPM_OUT_DRV_E("DL1", SND_SOC_NOPM, 0, 0, NULL, 0,
		      mt6589_afe_memif_event,
		      SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_OUT_DRV_E("VUL", SND_SOC_NOPM, 0, 0, NULL, 0,
			      mt6589_afe_memif_event,
			      SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_PRE_PMU),
	/*
	 * The Ixx/Oxx interconnect widgets the stock driver names in
	 * SetinputConnection() are deliberately absent here.  They are not
	 * endpoints, and nothing on the codec side routes to them, so
	 * dapm_generic_check_power() would see no sink and they would never
	 * power - dead weight that misleads a reader into thinking the
	 * interconnect is modelled.  The interconnect itself is hardware:
	 * AFE_CONN1 bit 21 (I05_O03_S) and AFE_CONN2 bit 6 (I06_O04_S) are
	 * programmed in prepare().
	 *
	 * The graph therefore has just the two memory interfaces, with the
	 * codec's DAC hanging off the DAI widget DAPM creates for us:
	 *
	 *	DL1 Playback -> DL1 -> (AFE_CONN1/CON2) -> I2S2 -> MT6320 DAC
	 */
};

static const struct snd_soc_dapm_route mt6589_afe_routes[] = {
	/* Playback: the DAI's own widget feeds the DL1 memory interface. */
	{ "DL1", NULL, "DL1 Playback" },
	/* Capture: the I2S ADC feeds the VUL memory interface. */
	{ "VUL", NULL, "VUL Capture" },
};

static const struct snd_soc_component_driver mt6589_afe_component = {
	.name = "mt6589-afe-pcm",
	.dapm_widgets = mt6589_afe_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6589_afe_widgets),
	.dapm_routes = mt6589_afe_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6589_afe_routes),
	.open = mt6589_afe_pcm_open,
	.hw_params = mt6589_afe_pcm_hw_params,
	.prepare = mt6589_afe_pcm_prepare,
	.trigger = mt6589_afe_pcm_trigger,
	.pointer = mt6589_afe_pcm_pointer,
	.pcm_new = mt6589_afe_pcm_new,
};

/* IRQ1 marks a DL1 period, IRQ2 a VUL one; hardirq, fast_io regmap,
 * atomic PCM.  Active-low.
 */
static irqreturn_t mt6589_afe_irq(int irq, void *dev_id)
{
	struct mt6589_afe *afe = dev_id;
	unsigned int status;
	int ret;

	ret = regmap_read(afe->regmap, AFE_IRQ_MCU_STATUS, &status);
	if (ret) {
		dev_err_ratelimited(afe->dev,
				    "failed to read AFE IRQ status: %d\n",
				    ret);
		return IRQ_HANDLED;
	}

	status &= AFE_IRQ_MCU_STATUS_MASK;
	if (!status) {
		ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR,
				   AFE_IRQ_MCU_CLR_NOSTATUS);
		if (ret)
			dev_err_ratelimited(afe->dev,
					    "failed to clear AFE IRQ: %d\n",
					    ret);
		return IRQ_HANDLED;
	}

	/*
	 * Ack before announcing the period.  The line is level triggered, so
	 * while the status is still set the handler runs again the moment it
	 * returns; snd_pcm_period_elapsed() takes the stream lock and can
	 * sleep, and doing that with the line still asserted is what wedged
	 * the whole system on the first period interrupt.
	 *
	 * Note the status is captured first, so clearing it here still reports
	 * the period that was just handled.
	 */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR, status);
	if (ret)
		dev_err_ratelimited(afe->dev,
				    "failed to clear AFE IRQ status: %d\n",
				    ret);

	if ((status & AFE_IRQ_MCU_STATUS_IRQ1) && afe->dl1_substream)
		snd_pcm_period_elapsed(afe->dl1_substream);

	if ((status & AFE_IRQ_MCU_STATUS_IRQ2) && afe->vul_substream)
		snd_pcm_period_elapsed(afe->vul_substream);

	return IRQ_HANDLED;
}

static int mt6589_afe_pcm_dev_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6589_afe *afe;
	struct resource res;
	void __iomem *base;
	int ret, irq;

	afe = devm_kzalloc(dev, sizeof(*afe), GFP_KERNEL);
	if (!afe)
		return -ENOMEM;
	afe->dev = dev;
	platform_set_drvdata(pdev, afe);

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	/* The AFE registers are in the parent audsys syscon window. */
	ret = of_address_to_resource(dev->parent->of_node, 0, &res);
	if (ret)
		return dev_err_probe(dev, ret, "no AFE reg in parent syscon\n");
	base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!base)
		return dev_err_probe(dev, -ENOMEM, "failed to map AFE registers\n");
	afe->regmap = devm_regmap_init_mmio(dev, base, &mt6589_afe_regmap_config);
	if (IS_ERR(afe->regmap))
		return dev_err_probe(dev, PTR_ERR(afe->regmap),
				     "failed to init AFE regmap\n");

	/*
	 * Release the AFE and I2S power-down bits before anything enables
	 * the clocks that live behind them, otherwise the register writes
	 * that follow land on a block that is still powered down.
	 *
	 * PDN_AFE resets to 0 (powered on) but PDN_I2S resets to 1 (powered
	 * down), so both are cleared explicitly.
	 */
	ret = regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
				 AUDIO_TOP_CON0_PDN_AFE |
				 AUDIO_TOP_CON0_PDN_I2S, 0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to power on AFE\n");

	afe->clk = devm_clk_get_enabled(dev, "afe");
	if (IS_ERR(afe->clk))
		return dev_err_probe(dev, PTR_ERR(afe->clk),
				     "failed to get/enable the audio clock\n");

	afe->clk_i2s = devm_clk_get_enabled(dev, "i2s");
	if (IS_ERR(afe->clk_i2s))
		return dev_err_probe(dev, PTR_ERR(afe->clk_i2s),
				     "failed to get/enable the I2S clock\n");

	/* mask all AFE IRQs + clear stale status before hooking the GIC */
	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CON, 0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to mask AFE IRQs\n");

	ret = regmap_write(afe->regmap, AFE_IRQ_MCU_CLR,
			   AFE_IRQ_MCU_CLR_NOSTATUS);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to clear AFE IRQ status\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mt6589_afe_irq, 0, "mt6589-afe", afe);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request AFE irq %d\n", irq);

	ret = devm_snd_soc_register_component(dev, &mt6589_afe_component,
					      mt6589_afe_dais,
					      ARRAY_SIZE(mt6589_afe_dais));
	if (ret)
		return dev_err_probe(dev, ret, "failed to register AFE component\n");

	return 0;
}

static const struct of_device_id mt6589_afe_pcm_dt_match[] = {
	{ .compatible = "mediatek,mt6589-audio" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6589_afe_pcm_dt_match);

static struct platform_driver mt6589_afe_pcm_driver = {
	.driver = {
		.name = "mt6589-afe-pcm",
		.of_match_table = mt6589_afe_pcm_dt_match,
	},
	.probe = mt6589_afe_pcm_dev_probe,
};
module_platform_driver(mt6589_afe_pcm_driver);

MODULE_DESCRIPTION("MediaTek mt6589 AFE platform driver");
MODULE_LICENSE("GPL");
