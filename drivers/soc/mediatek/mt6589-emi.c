// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6589 EMI: bandwidth monitor, MPU and bandwidth limiter.
 *
 * REGISTER WINDOW
 *
 *  The EMI configuration window is at 0x1020_3000.  Downstream
 *  mt_reg_base.h:96 "EMI_BASE" is 0xF0203000, which maps back to
 *  0x1020_3000.  The datasheet's EMI chapter lists the registers as
 *  1020_3000 EMI_CONA through 1020_3400 EMI_BMEN and beyond; the
 *  bandwidth-filter block is 1020_3100..1020_3158, the protection unit
 *  1020_3160..1020_3220 and the bus monitor 1020_3400..1020_3570.
 *  Register offsets below are relative to 0x1020_3000, matching
 *  downstream emi_mpu.h / emi_bwl.h / mt_emi_bm.h, and each was checked
 *  against the datasheet address rather than copied.
 *
 * WHAT IS AND IS NOT PROGRAMMED
 *
 *  Nothing is written at probe time.  Three reasons, one per block:
 *
 *   - Bandwidth limiter: the reset value is what the bootloader left, and
 *     downstream's own "normal" concurrency scenario is a specific
 *     6-register setting (con_sce_lpddr2.h) that is board and DRAM-speed
 *     dependent.  Guessing it would change DRAM arbitration for every
 *     master on the bus.
 *
 *   - MPU: enabling a region with the wrong permissions on this SoC means
 *     the AP cannot read its own memory; downstream has no MT6589 region
 *     table in the device tree to copy, and the table it does have lives
 *     in the modem driver (see the comment above mt6589_emi_default_regions).
 *
 *   - Bus monitor: enabling it steals DRAM bandwidth and its counters
 *     overrun; it is turned on only while debugfs is being read.
 */
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>

#include <linux/soc/mediatek/mt6589-emi.h>

#define EMI_CONA		0x0000
#define EMI_CONM		0x0060

/* Bandwidth filter controls, datasheet p. 583-584. */
#define EMI_ARBA		0x0100	/* APMCU,          m0 */
#define EMI_ARBB		0x0108	/* not documented for MT6589; see note */
#define EMI_ARBC		0x0110	/* ARM9,           m2 */
#define EMI_ARBD		0x0118	/* MDMCU,          m3 */
#define EMI_ARBE		0x0120	/* modem HW,       m4 */
#define EMI_ARBF		0x0128	/* MM0,            m5 */
#define EMI_ARBG		0x0130	/* MM1,            m6 */
#define EMI_ARBI		0x0140	/* filter priority encode */
#define EMI_ARBI_2ND		0x0144	/* priority encode for MD */
#define EMI_ARBJ		0x0148	/* turnaround command number */
#define EMI_ARBJ_2ND		0x014c
#define EMI_ARBK		0x0150	/* page miss control */
#define EMI_ARBK_2ND		0x0154
#define EMI_SLCT		0x0158

/* Memory protection unit, datasheet p. 583-584. */
#define EMI_MPUA		0x0160	/* region 0 base/end,   stride 8 */
#define EMI_MPUI		0x01a0	/* region 0/1 permissions */
#define EMI_MPUJ		0x01a8	/* region 2/3 */
#define EMI_MPUK		0x01b0	/* region 4/5 */
#define EMI_MPUL		0x01b8	/* region 6/7 */
#define EMI_MPUP		0x01d8	/* domain 0 violation status */
#define EMI_MPUQ		0x01e0	/* domain 1 */
#define EMI_MPUR		0x01e8	/* domain 2 */
#define EMI_MPUS		0x01f0	/* violation debug info */
#define EMI_MPUT		0x01f8	/* violation address, [31:16] */
#define EMI_MPUU		0x0200
#define EMI_MPUY		0x0220	/* domain 3 */

#define EMI_MPU_ALIGNMENT	0x10000
/* Violation addresses are reported in physical memory space. */
#define EMI_PHY_OFFSET		0x80000000

/* MPUI..MPUL pack four permissions per 16-bit half. */
#define EMI_MPU_PERM(p)		((p) & 0x7)
#define EMI_MPU_SET_PERM(d3, d2, d1, d0) \
	((((d3) & 0x7) << 9) | (((d2) & 0x7) << 6) | \
	 (((d1) & 0x7) << 3) | ((d0) & 0x7))

/* EMI_MPUS (p. 621f): 31 CLR, 29 R_VIO, 28 W_VIO, 24 APB_ABORT, 23:16
 * REGION_ABORT, 13:12 DOMAIN_ID, 10:0 MASTER_ID.
 */
#define EMI_MPUS_CLR		BIT(31)
#define EMI_MPUS_R_VIO		BIT(29)
#define EMI_MPUS_W_VIO		BIT(28)
#define EMI_MPUS_APB_ABORT	BIT(24)
#define EMI_MPUS_REGION		GENMASK(23, 16)
#define EMI_MPUS_DOMAIN		GENMASK(13, 12)
#define EMI_MPUS_MASTER		GENMASK(10, 0)
#define EMI_MPUS_VIO_STATUS	GENMASK(9, 0)

/* EMI_MPUT (p. 622): MPU_ERROR_ADDR[31:16], a 64KB-granular address. */
#define EMI_MPUT_ADDR		GENMASK(31, 16)

/* EMI_MPUP/Q/R/Y status clear, downstream emi_mpu.c __clear_emi_mpu_vio(). */
#define EMI_MPU_VIO_CLEAR	GENMASK(9, 0)

/* Bus monitor, datasheet p. 584. */
#define EMI_BMEN		0x0400
#define EMI_BCNT		0x0408
#define EMI_TACT		0x0410
#define EMI_TSCT		0x0418
#define EMI_WACT		0x0420
#define EMI_WSCT		0x0428
#define EMI_BACT		0x0430
#define EMI_BSCT		0x0438
#define EMI_MSEL		0x0440
#define EMI_TSCT2		0x0448
#define EMI_TSCT3		0x0450
#define EMI_WSCT2		0x0458
#define EMI_WSCT3		0x0460
#define EMI_WSCT4		0x0464
#define EMI_MSEL2		0x0468
#define EMI_BMID0		0x04b0
#define EMI_BMEN1		0x04e0
#define EMI_BMEN2		0x04e8
#define EMI_TTYPE1		0x0500

/* EMI_BMEN bits (mt_emi_bm.h BUS_MON_EN / BUS_MON_PAUSE / BC_OVERRUN). */
#define EMI_BMEN_BUS_MON_EN	BIT(0)
#define EMI_BMEN_BUS_MON_PAUSE	BIT(1)
#define EMI_BMEN_BC_OVERRUN	BIT(8)
/* Master select for counter 1, EMI_BMEN[22:16]. */
#define EMI_BMEN_SEL_MASTER	GENMASK(22, 16)	/* one bit per master */
/* Read/write filter, EMI_BMEN[5:4]: 00/11 both, 01 read only, 10 write only. */
#define EMI_BMEN_RW_TYPE	GENMASK(5, 4)

#define EMI_BM_COUNTERS		4

/*
 * A master is named in three different encodings on this block, and
 * conflating them is the easy mistake.
 *
 * EMI_BMEN[22:16] SEL_MASTER is a BITMASK, one bit per master, not an ID
 * (datasheet p. 625): [16] APMCU, [18] ARM9, [19] MDMCU, [20] Modem,
 * [21] Multimedia 0, [22] Multimedia 1.  Bit [17] is unused, which matches
 * the absence of an EMI_ARBB register: the datasheet's list goes from ARBA
 * at 1020_3100 straight to ARBC at 1020_3110, and ARBA's fields are M0_*
 * while ARBC's are M2_*.  That is also why downstream's mt_emi_bm.h
 * BM_MASTER_ALL is 0x7D and not 0x7F -- those values are this bitmask.
 *
 * The EMI_ARBx bandwidth filters are one per master at 8-byte strides, and
 * the MPU reports a violation master as yet a third encoding, an
 * {AXI ID, Port ID} pair in EMI_MPUS[10:0].
 *
 * So "bm_bit" is the SEL_MASTER bit for the monitor and "arb_off" is the
 * filter register offset for the ARB dump; neither is an MPU master ID.
 */
static const struct {
	const char *name;
	u16 arb_off;	/* offset of this master's EMI_ARBx from EMI_ARBA */
	u8 bm_bit;	/* its bit position in EMI_BMEN SEL_MASTER */
} mt6589_emi_masters[] = {
	{ "APMCU", 0x00, 16 },	/* EMI_ARBA */
	{ "ARM9",  0x10, 18 },	/* EMI_ARBC */
	{ "MDMCU", 0x18, 19 },	/* EMI_ARBD */
	{ "MDHW",  0x20, 20 },	/* EMI_ARBE */
	{ "MM0",   0x28, 21 },	/* EMI_ARBF */
	{ "MM1",   0x30, 22 },	/* EMI_ARBG */
};

/*
 * EMI_ARBA..ARBG field layout, datasheet p. 596-597.  All six registers are
 * documented identically, only the Mn_ prefix differs.
 */
#define EMI_ARB_WR_LATENCY	GENMASK(31, 24)	/* Mn_WR_LATENCY */
#define EMI_ARB_RD_LATENCY	GENMASK(23, 16)	/* Mn_RD_LATENCY */
#define EMI_ARB_BANDWIDTH_DIS	BIT(15)		/* Mn_BANDWIDTH_DISABLE */
#define EMI_ARB_MODE		BIT(14)		/* Mn_MODE: 0 hard, 1 soft */
#define EMI_ARB_HI_PRIO_DOM	BIT(13)		/* Mn_HI_PRIO_DOMAIN_BW */
#define EMI_ARB_BW_FILTER_EN	BIT(12)		/* Mn_BW_FILTER_EN */
#define EMI_ARB_BW_FILTER_LEN	GENMASK(11, 10)	/* 256/512/1024/2048 cyc */
#define EMI_ARB_DOUBLE_HALF_BW	GENMASK(9, 8)	/* channel arrangement */
#define EMI_ARB_BANDWIDTH	GENMASK(5, 0)	/* n/64 of total */

struct mt6589_emi_region {
	u32 start;
	u32 end;
	u32 perm[4];	/* per domain: AP, MD0, MD1, MM */
};

struct mt6589_emi {
	void __iomem *base;
	struct device *dev;
	struct mutex lock;	/* serialises MPU programming and BM sampling */
	int irq;
	u32 vio_count;
	u32 last_vio_mps;
	u32 last_vio_addr;
	struct dentry *debugfs;
};

static inline u32 mt6589_emi_read(struct mt6589_emi *emi, u32 off)
{
	return readl(emi->base + off);
}

static inline void mt6589_emi_write(struct mt6589_emi *emi, u32 off, u32 val)
{
	writel(val, emi->base + off);
}

/*
 * The MPU region table downstream actually installs for MT6589, reproduced
 * for reference only -- nothing below programs it.
 *
 * Source: kernel/drivers/dual_ccci/src/ccci_platform.c,
 * enable_mem_access_protection(), which contains the comment table
 *
 *            | Region |  D0(AP)  |  D1(MD0)  |  D2(MD1)  |  D3(MM)
 *   MD0 ROM  |    0   |RO(S/NS)  |RO(S/NS)   |Forbidden  |Forbidden
 *   MD0 R/W+ |    1   |Forbidden |No protect |Forbidden  |Forbidden
 *   MD1 ROM  |    2   |RO(S/NS)  |Forbidden  |RO(S/NS)   |Forbidden
 *   MD1 R/W+ |    3   |Forbidden |Forbidden  |No protect |Forbidden
 *   Secure OS|    4   |RW(S)     |Forbidden  |Forbidden  |Forbidden
 *   MD0 Share|    5   |No protect|No protect |Forbidden  |Forbidden
 *   MD1 Share|    6   |No protect|Forbidden  |No protect |Forbidden
 *   AP       |    7   |No protect|Forbidden  |Forbidden  |No protect
 *
 * The addresses are not constants: they come from the modem image layout
 * and from get_max_DRAM_size(), so a mainline driver cannot install them
 * without the modem firmware's memory map.  A board that wants MPU
 * protection provides its own regions through the device tree; with no
 * "mediatek,emi-mpu-regions" property, no region is written and the
 * hardware keeps whatever the bootloader set.
 */

/**
 * mt6589_emi_mpu_set_region() - program one MPU region
 * @emi: EMI context
 * @region: region index, 0..7
 * @start: start address in physical memory space
 * @end: end address in physical memory space
 * @perm: four 3-bit permissions, one per domain (AP, MD0, MD1, MM)
 *
 * Addresses are rounded down to the region granularity the hardware has,
 * 64 KB (downstream emi_mpu.h EMI_MPU_ALIGNMENT).  A region is disabled by
 * passing start == end == 0, which downstream also uses for that purpose.
 *
 * Returns 0, -EINVAL for a bad region or an empty/inverted range.
 */
int mt6589_emi_mpu_set_region(struct mt6589_emi *emi, int region,
			      u32 start, u32 end, const u32 perm[4])
{
	u32 perm_reg, perm_off, val;
	int ret = 0;

	if (region < 0 || region >= MT6589_EMI_NR_REGIONS)
		return -EINVAL;

	mutex_lock(&emi->lock);

	if (start || end) {
		if (start >= EMI_PHY_OFFSET)
			start -= EMI_PHY_OFFSET;
		if (end >= EMI_PHY_OFFSET)
			end -= EMI_PHY_OFFSET;

		start &= ~(EMI_MPU_ALIGNMENT - 1);
		end &= ~(EMI_MPU_ALIGNMENT - 1);

		if (end <= start) {
			ret = -EINVAL;
			goto out;
		}
	}

	/*
	 * The base/end register packs [31:16] = start >> 16 and
	 * [15:0] = end >> 16, both in 64KB units -- downstream
	 * emi_mpu.c emi_mpu_set_region_protection() writes
	 * (start << 16) | end after shifting both right by 16.
	 */
	if (start || end)
		val = ((start >> 16) << 16) | (end >> 16);
	else
		val = 0;

	mt6589_emi_write(emi, EMI_MPUA + region * 8, val);

	/*
	 * Permissions live in EMI_MPUI..EMI_MPUL, two regions per register:
	 * even region in [15:0], odd region in [31:16] (datasheet p. 615,
	 * EMI_MPUI holds R0 and R1).
	 */
	perm_reg = EMI_MPUI + (region / 2) * 8;
	perm_off = (region % 2) * 16;

	val = mt6589_emi_read(emi, perm_reg);
	val &= ~(GENMASK(11, 0) << perm_off);
	val |= EMI_MPU_SET_PERM(perm[3], perm[2], perm[1], perm[0]) << perm_off;
	mt6589_emi_write(emi, perm_reg, val);

out:
	mutex_unlock(&emi->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6589_emi_mpu_set_region);

/**
 * mt6589_emi_mpu_clear_violation() - clear latched MPU violation state
 * @emi: EMI context
 *
 * Must run after the status has been read and before the interrupt is
 * acknowledged, or the diagnostic the status exists for is lost.
 *
 * EMI_MPUS[31] CLR clears the debug latch (p. 621); the per-domain
 * violation status registers are write-1-to-clear in their low 10 bits,
 * which is what downstream __clear_emi_mpu_vio() does.  Downstream also
 * checks that MPUS/MPUT actually read back zero and complains if not; the
 * same check is done here because a sticky bit means the real violator is
 * still generating traffic.
 */
int mt6589_emi_mpu_clear_violation(struct mt6589_emi *emi)
{
	mt6589_emi_write(emi, EMI_MPUP, EMI_MPU_VIO_CLEAR);
	mt6589_emi_write(emi, EMI_MPUQ, EMI_MPU_VIO_CLEAR);
	mt6589_emi_write(emi, EMI_MPUR, EMI_MPU_VIO_CLEAR);
	mt6589_emi_write(emi, EMI_MPUY, EMI_MPU_VIO_CLEAR);

	mt6589_emi_write(emi, EMI_MPUS, EMI_MPUS_CLR);

	if (mt6589_emi_read(emi, EMI_MPUS) || mt6589_emi_read(emi, EMI_MPUT))
		return -EBUSY;

	return 0;
}
EXPORT_SYMBOL_GPL(mt6589_emi_mpu_clear_violation);

/*
 * Decode the MPU violation master field, EMI_MPUS[10:0]: a MASTER_ID of
 * {AXI ID[10:3], Port ID[2:0]} (datasheet p. 622, "Records the violation
 * master ID {AXI ID, Port ID}").
 *
 * This is NOT the bus-monitor SEL_MASTER bitmask and NOT the ARB register
 * index.  Only the Port ID can be named unambiguously: downstream's
 * mst_tbl[] in emi_mpu.c assigns ports to bus domains, and the AXI ID
 * within a port is further split by transaction type and byte lane.
 *
 * Port assignments below are downstream's, emi_mpu.c mst_tbl[] .port
 * field: 0 = APMCU/ARM, 2 = ARM9 (MD2), 3 = MDMCU, 4 = modem hardware,
 * 5 = MM0 plus peripherals, 6 = MM1.  Ports 1 and 7 do not appear in that
 * table at all, so they are reported as unknown instead of being given a
 * guessed name.
 */
static const char *mt6589_emi_vio_port_name(u32 master_id)
{
	switch (master_id & 0x7) {
	case 0:
		return "APMCU/ARM";
	case 2:
		return "ARM9/MD2";
	case 3:
		return "modem MCU";
	case 4:
		return "modem hardware";
	case 5:
		return "MM0/peripheral";
	case 6:
		return "MM1";
	default:
		return "unknown port";
	}
}

/* Name a bus-monitor SEL_MASTER bitmask, which is a different encoding. */
static const char *mt6589_emi_bm_sel_name(u32 sel)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6589_emi_masters); i++)
		if (sel & BIT(mt6589_emi_masters[i].bm_bit))
			return mt6589_emi_masters[i].name;

	return "none";
}

static irqreturn_t mt6589_emi_mpu_irq(int irq, void *data)
{
	struct mt6589_emi *emi = data;
	u32 mps, addr, region_mask;
	unsigned int region;

	mps = mt6589_emi_read(emi, EMI_MPUS);
	addr = mt6589_emi_read(emi, EMI_MPUT);

	/*
	 * LIABLE TO BE IRQF_SHARED WITH DEVAPC.  Downstream requests this
	 * line with IRQF_SHARED and documents why:
	 * emi_mpu.c: "Interrupts of violation (including SPC in SMI, or EMI
	 * MPU) are triggered by the device APC.  Need to share the interrupt
	 * with the SPC driver."  The line is GIC SPI 94 (downstream
	 * mt_irq.h MT_APARM_DOMAIN_IRQ_ID = GIC_PRIVATE_SIGNALS + 94), which
	 * mainline's drivers/soc/mediatek/mtk-devapc.c already owns for the
	 * MT6589 devapc node.  So a violation that did not come from the MPU
	 * must be handed back untouched.
	 *
	 * EMI_MPUS is the evidence: REGION_ABORT, DOMAIN_ID or a VIO bit set
	 * means the MPU saw it.  An MPU violation also latches MPUT, whose
	 * upper half is the faulting 64KB page, so an all-zero register pair
	 * with no status bits is devapc's or SMI's business.
	 */
	if (!(mps & (EMI_MPUS_VIO_STATUS | EMI_MPUS_REGION | EMI_MPUS_DOMAIN |
		     EMI_MPUS_R_VIO | EMI_MPUS_W_VIO | EMI_MPUS_APB_ABORT)))
		return IRQ_NONE;

	emi->vio_count++;
	emi->last_vio_mps = mps;
	emi->last_vio_addr = addr;

	region_mask = FIELD_GET(EMI_MPUS_REGION, mps);
	region = region_mask ? __ffs(region_mask) : -1;

	dev_err_ratelimited(emi->dev,
			    "EMI MPU violation: region %d, %s%s%s, master 0x%lx (%s), addr 0x%08x, MPUS 0x%08x MPUT 0x%08x\n",
			    region,
			    FIELD_GET(EMI_MPUS_W_VIO, mps) ? "write" : "read",
			    FIELD_GET(EMI_MPUS_R_VIO, mps) ? "/read-status" : "",
			    FIELD_GET(EMI_MPUS_APB_ABORT, mps) ? "/APB" : "",
			    FIELD_GET(EMI_MPUS_MASTER, mps),
			    mt6589_emi_vio_port_name(FIELD_GET(EMI_MPUS_MASTER, mps)),
			    (u32)FIELD_GET(EMI_MPUT_ADDR, addr),
			    mps, addr);

	if (mt6589_emi_mpu_clear_violation(emi))
		dev_err_ratelimited(emi->dev,
				    "violation status did not clear; the violator is still active\n");

	return IRQ_HANDLED;
}

/*
 * Bus monitor sampling.
 *
 * The counters are free-running once EMI_BMEN[0] BUS_MON_EN is set and
 * overrun quickly at DRAM rates, so a sample is: pause, read, restart.
 * EMI_BMEN[8] BC_OVERRUN tells us the sample was too long to be valid;
 * downstream mt_emi_bm.c treats that as an error rather than reporting a
 * wrapped count, which is the only defensible thing to do with a counter
 * that has no overflow interrupt.
 */
struct mt6589_emi_bm_sample {
	u32 bus_cycles;
	u32 trans_all;
	u32 trans_sel;		/* counter 1 */
	u32 word_all;
	u32 word_sel;		/* counter 1 */
	u32 bw_words;
	u32 overhead_words;
	bool overrun;
};

static void mt6589_emi_bm_sample(struct mt6589_emi *emi,
				 struct mt6589_emi_bm_sample *s)
{
	u32 bmen;

	bmen = mt6589_emi_read(emi, EMI_BMEN);
	if (!(bmen & EMI_BMEN_BUS_MON_EN))
		mt6589_emi_write(emi, EMI_BMEN, bmen | EMI_BMEN_BUS_MON_EN);

	/* Pause the counters so the set of readings is coherent. */
	mt6589_emi_write(emi, EMI_BMEN, bmen | EMI_BMEN_BUS_MON_EN |
					 EMI_BMEN_BUS_MON_PAUSE);

	s->bus_cycles = mt6589_emi_read(emi, EMI_BCNT);
	s->trans_all = mt6589_emi_read(emi, EMI_TACT);
	s->trans_sel = mt6589_emi_read(emi, EMI_TSCT);
	s->word_all = mt6589_emi_read(emi, EMI_WACT);
	s->word_sel = mt6589_emi_read(emi, EMI_WSCT);
	s->bw_words = mt6589_emi_read(emi, EMI_BACT);
	s->overhead_words = mt6589_emi_read(emi, EMI_BSCT);

	/*
	 * Re-read BMEN for the overrun flag: it is set by hardware while the
	 * counters run, so it is only meaningful once they are stopped.
	 */
	s->overrun = !!(mt6589_emi_read(emi, EMI_BMEN) & EMI_BMEN_BC_OVERRUN);

	/* Clearing the counters is done by pulsing BUS_MON_EN low. */
	mt6589_emi_write(emi, EMI_BMEN, bmen & ~EMI_BMEN_BUS_MON_EN);
	mt6589_emi_write(emi, EMI_BMEN, bmen);
}

static int mt6589_emi_bm_show(struct seq_file *s, void *unused)
{
	struct mt6589_emi *emi = s->private;
	struct mt6589_emi_bm_sample bm;
	u32 bmen;
	unsigned int i;

	mutex_lock(&emi->lock);
	bmen = mt6589_emi_read(emi, EMI_BMEN);
	mt6589_emi_bm_sample(emi, &bm);
	mutex_unlock(&emi->lock);

	seq_printf(s, "EMI_BMEN       : 0x%08x\n", bmen);
	seq_printf(s, "BUS_MON_EN     : %u\n", !!(bmen & EMI_BMEN_BUS_MON_EN));
	seq_printf(s, "selected masters: 0x%02lx (%s)\n",
		   FIELD_GET(EMI_BMEN_SEL_MASTER, bmen),
		   mt6589_emi_bm_sel_name(FIELD_GET(EMI_BMEN_SEL_MASTER, bmen)));
	seq_printf(s, "read/write filter: %lu (%s)\n",
		   FIELD_GET(EMI_BMEN_RW_TYPE, bmen),
		   (FIELD_GET(EMI_BMEN_RW_TYPE, bmen) == 1) ? "read only" :
		   (FIELD_GET(EMI_BMEN_RW_TYPE, bmen) == 2) ? "write only" :
							      "both");

	if (bm.overrun)
		seq_puts(s, "OVERRUN: sample invalid, read again more often\n");

	seq_printf(s, "bus cycles     : %u\n", bm.bus_cycles);
	seq_printf(s, "transactions   : %u total, %u selected master\n",
		   bm.trans_all, bm.trans_sel);
	seq_printf(s, "words          : %u total, %u selected master\n",
		   bm.word_all, bm.word_sel);
	seq_printf(s, "bandwidth words: %u\n", bm.bw_words);
	seq_printf(s, "overhead words : %u\n", bm.overhead_words);

	/*
	 * Load is bandwidth words over bus cycles, both in the same sample
	 * window, so it is a ratio and needs no clock rate.  EMI_BACT counts
	 * 64-bit words (mt_emi_bm.h calls it "bandwidth word count"), which
	 * is the DRAM bus width here -- but rather than assert that, the raw
	 * counts are printed above and this is a plain ratio.
	 */
	if (!bm.overrun && bm.bus_cycles)
		seq_printf(s, "load           : %u%%\n",
			   (u32)div_u64((u64)bm.bw_words * 100, bm.bus_cycles));

	seq_puts(s, "\nper-master bandwidth filter controls\n");
	for (i = 0; i < ARRAY_SIZE(mt6589_emi_masters); i++) {
		u32 off = EMI_ARBA + mt6589_emi_masters[i].arb_off;
		u32 arb = mt6589_emi_read(emi, off);

		/*
		 * ARBA..ARBG are 8 bytes apart and hold one master each, but
		 * the set is not contiguous by master ID: there is no ARBB
		 * master on this SoC (see mt6589_emi_masters[]), so each
		 * entry carries its own offset.
		 */
		seq_printf(s, "  %-6s ARB 0x%08x bandwidth %2lu/64 filter %u len %lu hard %s\n",
			   mt6589_emi_masters[i].name, arb,
			   FIELD_GET(EMI_ARB_BANDWIDTH, arb),
			   !!(arb & EMI_ARB_BW_FILTER_EN),
			   FIELD_GET(EMI_ARB_BW_FILTER_LEN, arb),
			   (arb & EMI_ARB_MODE) ? "soft" : "hard");
		seq_printf(s, "         rd latency %lu wr latency %lu hi-prio dominates %u\n",
			   FIELD_GET(EMI_ARB_RD_LATENCY, arb),
			   FIELD_GET(EMI_ARB_WR_LATENCY, arb),
			   !!(arb & EMI_ARB_HI_PRIO_DOM));
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6589_emi_bm);

static int mt6589_emi_mpu_show(struct seq_file *s, void *unused)
{
	struct mt6589_emi *emi = s->private;
	static const char * const perm_name[] = {
		"no-protection", "secure-rw", "secure-rw+nsec-read",
		"secure-rw+nsec-write", "read-only", "forbidden",
	};
	u32 mps, addr, perm_reg, val;
	unsigned int i, d;

	mps = mt6589_emi_read(emi, EMI_MPUS);
	addr = mt6589_emi_read(emi, EMI_MPUT);

	seq_printf(s, "EMI_MPUS       : 0x%08x\n", mps);
	seq_printf(s, "EMI_MPUT       : 0x%08x\n", addr);
	seq_printf(s, "  region abort : 0x%02lx\n", FIELD_GET(EMI_MPUS_REGION, mps));
	seq_printf(s, "  domain       : %lu\n", FIELD_GET(EMI_MPUS_DOMAIN, mps));
	seq_printf(s, "  master id    : 0x%lx\n", FIELD_GET(EMI_MPUS_MASTER, mps));
	seq_printf(s, "  read / write : %u / %u\n",
		   !!(mps & EMI_MPUS_R_VIO), !!(mps & EMI_MPUS_W_VIO));
	seq_printf(s, "  address      : 0x%08lx\n", FIELD_GET(EMI_MPUT_ADDR, addr));
	seq_printf(s, "violations     : %u\n", emi->vio_count);

	seq_puts(s, "\nregions (unprogrammed unless the board supplied them)\n");
	for (i = 0; i < MT6589_EMI_NR_REGIONS; i++) {
		u32 base = mt6589_emi_read(emi, EMI_MPUA + i * 8);

		perm_reg = EMI_MPUI + (i / 2) * 8;
		val = mt6589_emi_read(emi, perm_reg);
		val >>= (i % 2) * 16;

		/*
		 * Print the two fields exactly as they are stored, without
		 * adding the 64KB granularity to the end field: what the
		 * hardware compares is these numbers, and inventing an
		 * inclusive end here would hide an off-by-one in whoever
		 * programmed the region.
		 */
		seq_printf(s, "  region %u: start 0x%08x end 0x%08x (%s)",
			   i,
			   base ? (u32)(base >> 16) << 16 : 0,
			   base ? (u32)(base & 0xffff) << 16 : 0,
			   base ? "" : "disabled");
		for (d = 0; d < 4; d++)
			seq_printf(s, " d%u=%s", d, perm_name[val >> (3 * d) & 0x7]);
		seq_puts(s, "\n");
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mt6589_emi_mpu);

/*
 * Provider lookup for other drivers.  There is exactly one EMI block on the
 * SoC, so it is a singleton rather than a phandle: a phandle would make every
 * consumer carry a "mediatek,emi" property for a block that is never optional
 * per board.
 */
static struct mt6589_emi *mt6589_emi_instance;

struct mt6589_emi *mt6589_emi_get(void)
{
	return mt6589_emi_instance;
}
EXPORT_SYMBOL_GPL(mt6589_emi_get);

void __iomem *mt6589_emi_iomem(void)
{
	return mt6589_emi_instance ? mt6589_emi_instance->base : NULL;
}
EXPORT_SYMBOL_GPL(mt6589_emi_iomem);

static int mt6589_emi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6589_emi *emi;
	int ret;

	emi = devm_kzalloc(dev, sizeof(*emi), GFP_KERNEL);
	if (!emi)
		return -ENOMEM;

	emi->dev = dev;
	mutex_init(&emi->lock);

	emi->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(emi->base))
		return dev_err_probe(dev, PTR_ERR(emi->base),
				     "failed to map EMI window\n");

	/*
	 * The violation line is shared with DEVAPC (see the IRQ handler).
	 * It is optional: the bandwidth monitor and the MPU region API work
	 * without it, so a board that does not route the line still gets a
	 * usable driver instead of a failed probe.
	 */
	emi->irq = platform_get_irq_optional(pdev, 0);
	if (emi->irq == -EPROBE_DEFER)
		return -EPROBE_DEFER;

	if (emi->irq > 0) {
		ret = devm_request_irq(dev, emi->irq, mt6589_emi_mpu_irq,
				       IRQF_SHARED | IRQF_TRIGGER_LOW,
				       dev_name(dev), emi);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request violation IRQ\n");
	}

	platform_set_drvdata(pdev, emi);
	mt6589_emi_instance = emi;

	emi->debugfs = debugfs_create_dir("mt6589-emi", NULL);
	if (!IS_ERR_OR_NULL(emi->debugfs)) {
		debugfs_create_file("bandwidth", 0444, emi->debugfs, emi,
				    &mt6589_emi_bm_fops);
		debugfs_create_file("mpu", 0444, emi->debugfs, emi,
				    &mt6589_emi_mpu_fops);
	}

	dev_info(dev, "EMI mapped, violation IRQ %d, no region programmed\n",
		 emi->irq);

	return 0;
}

static void mt6589_emi_remove(struct platform_device *pdev)
{
	struct mt6589_emi *emi = platform_get_drvdata(pdev);

	debugfs_remove_recursive(emi->debugfs);
	mt6589_emi_instance = NULL;
}

static const struct of_device_id mt6589_emi_of_match[] = {
	{ .compatible = "mediatek,mt6589-emi" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6589_emi_of_match);

static struct platform_driver mt6589_emi_driver = {
	.probe	= mt6589_emi_probe,
	.remove	= mt6589_emi_remove,
	.driver	= {
		.name		= "mt6589-emi",
		.of_match_table	= mt6589_emi_of_match,
	},
};
module_platform_driver(mt6589_emi_driver);

MODULE_DESCRIPTION("MediaTek MT6589 EMI bandwidth monitor, MPU and limiter");
MODULE_LICENSE("GPL");
