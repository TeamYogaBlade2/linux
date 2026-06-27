// SPDX-License-Identifier: GPL-2.0-only

#if !defined(__SOCCONFIG_H__)
#define __SOCCONFIG_H__

/*#if defined(CONFIG_ARCH_MT6577)
#define VS_PRODUCT_NAME	"MT6577"
#else
#define VS_PRODUCT_NAME	"MT6575"
#endif*/

#define VS_PRODUCT_NAME "MT6589"

#define SYS_SGX_CLOCK_SPEED     286000000

#define SYS_SGX_HWRECOVERY_TIMEOUT_FREQ		(100)
#define SYS_SGX_PDS_TIMER_FREQ				(1000)

#if !defined(SYS_SGX_ACTIVE_POWER_LATENCY_MS)
#define SYS_SGX_ACTIVE_POWER_LATENCY_MS		(2)
#endif

#define SYS_MTK_SGX_REGS_SYS_PHYS_BASE  0x13000000 // MFG_AXI_BASE

#define SYS_MTK_SGX_REGS_SIZE           0xFFFF

#define SYS_MTK_SGX_IRQ				 220 //(188+32) // MT6589_MFG_IRQ_ID

#define DEVICE_SGX_INTERRUPT		(1<<0)

#if defined(__linux__)
#if defined(PVR_LDM_PLATFORM_PRE_REGISTERED_DEV)
#define	SYS_SGX_DEV_NAME	PVR_LDM_PLATFORM_PRE_REGISTERED_DEV
#else
#define	SYS_SGX_DEV_NAME	"mt6589_gpu"
#endif
#endif


#endif
