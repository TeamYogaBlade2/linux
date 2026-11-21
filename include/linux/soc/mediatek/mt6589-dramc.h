/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6589 DRAMC / DDRPHY read-only interface.
 *
 * The DRAM controller configuration is fixed by the bootloader before Linux
 * starts, and this driver deliberately provides no way to change it: a bad
 * DRAM timing change is not recoverable by code running out of the DRAM it
 * just mis-timed.  Consumers get the current configuration and nothing else.
 */
#ifndef __SOC_MEDIATEK_MT6589_DRAMC_H
#define __SOC_MEDIATEK_MT6589_DRAMC_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/types.h>

struct mt6589_dramc;

/* The datasheet splits DRAMC across three windows; see the driver header. */
#define MT6589_DRAMC_WIN_AO	0	/* 0x1000_4000 */
#define MT6589_DRAMC_WIN_NAO	1	/* 0x1020_F000 */
#define MT6589_DRAMC_WIN_DDRPHY	2	/* 0x1001_1000 */
#define MT6589_DRAMC_NR_WINDOWS	3

struct mt6589_dramc_info {
	const char *type;
	u32 conf1;
	u32 conf2;
	u32 actim0;
	u32 lpddr2;
	u32 cona;

	unsigned int bus_width;		/* 32 or 64 bits */
	unsigned int ram_width;		/* 16 or 32 bits per part */
	unsigned int burst_length;
	unsigned int column_bits;
	unsigned int emi_col_bits;
	unsigned int emi_col2nd_bits;
	unsigned int row_bits;
	unsigned int row2nd_bits;
	unsigned int bank_bits;
	unsigned int bank2nd_bits;
	bool dual_rank;
	bool page_mode;
	bool cke;
	bool self_refresh;
	u64 size_bytes;

	/* DRAMC clock cycles, as the datasheet encodes them. */
	unsigned int trcd;
	unsigned int trp;
	unsigned int tfaw;
	unsigned int twr;
	unsigned int twtr;
	unsigned int trc;
	unsigned int tras;
	unsigned int trrd;
	unsigned int refcnt;
	unsigned int refthd;

	unsigned long clock_rate;
};

#if IS_ENABLED(CONFIG_MTK_MT6589_DRAMC)

struct mt6589_dramc *mt6589_dramc_get(struct device *dev);

int mt6589_dramc_get_info(struct mt6589_dramc *dc,
			  struct mt6589_dramc_info *info);

#else

static inline struct mt6589_dramc *mt6589_dramc_get(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}

static inline int mt6589_dramc_get_info(struct mt6589_dramc *dc,
					struct mt6589_dramc_info *info)
{
	return -ENODEV;
}

#endif /* CONFIG_MTK_MT6589_DRAMC */

#endif /* __SOC_MEDIATEK_MT6589_DRAMC_H */
