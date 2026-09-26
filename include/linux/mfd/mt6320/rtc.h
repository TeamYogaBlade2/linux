/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6320 RTC register definitions for poweroff driver
 *
 * Register offsets are relative to the RTC block base (0xe000).
 * Values derived from the MT6589/MT6320 downstream kernel
 * (mediatek/platform/mt6589/kernel/core/include/mach/mt_rtc_hw.h)
 * and confirmed against the Aquaris 5 (BQ) kernel source.
 *
 * Copyright (C) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef _LINUX_MFD_MT6320_RTC_H_
#define _LINUX_MFD_MT6320_RTC_H_

#include <linux/jiffies.h>

/* RTC register offsets (relative to block base) */
#define MT6320_RTC_BBPU		0x0000
#define MT6320_RTC_PDN1		0x002c
#define MT6320_RTC_PROT		0x0036
#define MT6320_RTC_WRTGR	0x003c
#define MT6320_RTC_CON		0x003e

/* RTC_BBPU bits */
#define MT6320_RTC_BBPU_PWREN	BIT(0)	/* BBPU=1 when alarm fires */
#define MT6320_RTC_BBPU_AUTO	BIT(3)	/* BBPU=0 when xreset_rstb goes low */
#define MT6320_RTC_BBPU_CBUSY	BIT(6)	/* write-trigger busy flag */
#define MT6320_RTC_BBPU_KEY	(0x43 << 8)

/* RTC_PDN1 bits relevant to 32K GPIO users (bits 8..12) */
#define MT6320_RTC_GPIO_USER_MASK	GENMASK(12, 8)

/* RTC_PROT unlock sequence */
#define MT6320_RTC_PROT_UNLOCK1	0x586a
#define MT6320_RTC_PROT_UNLOCK2	0x9136

/* RTC_CON bits */
#define MT6320_RTC_CON_F32KOB	BIT(5)	/* 0: RTC_GPIO exports 32K */

/* Poll parameters for CBUSY */
#define MT6320_RTC_POLL_DELAY_US	10
#define MT6320_RTC_POLL_TIMEOUT		(jiffies_to_usecs(HZ))

/* MT6320 PMIC STRUP_CON9 for poweroff sequence enable */
#define MT6320_STRUP_CON9		0x0510
#define MT6320_STRUP_PWROFF_SEQ_EN	BIT(0)
#define MT6320_STRUP_PWROFF_PREOFF_EN	BIT(1)

#endif /* _LINUX_MFD_MT6320_RTC_H_ */
