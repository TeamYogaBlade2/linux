/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6589 EMI memory protection unit API.
 *
 * The EMI MPU blocks accesses from each bus master to address ranges.  A
 * board that wants it enabled obtains the EMI device and programs regions
 * with mt6589_emi_mpu_set_region(); the driver programs nothing by
 * default, because the wrong permission on this hardware stops the CPU
 * reading its own memory and there is no way back from that at runtime.
 */
#ifndef __SOC_MEDIATEK_MT6589_EMI_H
#define __SOC_MEDIATEK_MT6589_EMI_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/types.h>

struct mt6589_emi;

#if IS_ENABLED(CONFIG_MTK_MT6589_EMI)

/*
 * There is exactly one EMI block on the SoC, so these are singletons.  Both
 * return NULL until the EMI driver has probed.
 */
struct mt6589_emi *mt6589_emi_get(void);

int mt6589_emi_mpu_set_region(struct mt6589_emi *emi, int region,
			      u32 start, u32 end, const u32 perm[4]);

int mt6589_emi_mpu_clear_violation(struct mt6589_emi *emi);

/*
 * Raw access to the EMI window, for a driver that needs one register out of
 * it (mt6589-dramc wants EMI_CONA for the address mapping) without taking
 * ownership of the whole window.  Returns NULL if the EMI driver is not
 * probed.
 */
void __iomem *mt6589_emi_iomem(void);

#else

static inline struct mt6589_emi *mt6589_emi_get(void)
{
	return NULL;
}

static inline int mt6589_emi_mpu_set_region(struct mt6589_emi *emi,
					    int region, u32 start, u32 end,
					    const u32 perm[4])
{
	return -ENODEV;
}

static inline int mt6589_emi_mpu_clear_violation(struct mt6589_emi *emi)
{
	return -ENODEV;
}

static inline void __iomem *mt6589_emi_iomem(void)
{
	return NULL;
}

#endif /* CONFIG_MTK_MT6589_EMI */

/* MPU permission values, MT6589 datasheet p. 615, R0D0_APC. */
#define MT6589_EMI_NO_PROTECTION	0
#define MT6589_EMI_SEC_RW		1
#define MT6589_EMI_SEC_RW_NSEC_R	2
#define MT6589_EMI_SEC_RW_NSEC_W	3
#define MT6589_EMI_SEC_R_NSEC_R		4
#define MT6589_EMI_FORBIDDEN		5

/* Domain indices for the perm[] array: AP, modem 0, modem 1, multimedia. */
#define MT6589_EMI_DOMAIN_AP	0
#define MT6589_EMI_DOMAIN_MD0	1
#define MT6589_EMI_DOMAIN_MD1	2
#define MT6589_EMI_DOMAIN_MM	3

#define MT6589_EMI_NR_REGIONS	8

#endif /* __SOC_MEDIATEK_MT6589_EMI_H */
