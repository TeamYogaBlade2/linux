// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */

#ifndef _DT_BINDINGS_RESET_CONTROLLER_MT6582
#define _DT_BINDINGS_RESET_CONTROLLER_MT6582


/* INFRACFG resets */
/* INFRA_RST_0 */
#define INFRA_RST0_EMI_REG_RST		    0
#define INFRA_RST0_DRAMC0_AO_RST	        1
#define INFRA_RST0_FHCTL_RST		    2
#define INFRA_RST0_AP_CIRQ_EINT_RST		3
#define INFRA_RST0_APXGPT_RST		    4
#define INFRA_RST0_SCPSYS_RST		    5
#define INFRA_RST0_KP_RST		        6
#define INFRA_RST0_PMIC_WRAP_RST		7
#define INFRA_RST0_MIPI_CONFIG_RST		8
/* INFRA_RST_1 */
#define INFRA_RST1_EMI_RST		        32
#define INFRA_RST1_DRAMC0_RST		    34
#define INFRA_RST1_APMIXEDSYS_RST		35
#define INFRA_RST1_TRNG_RST		        36
#define INFRA_RST1_SYS_CIRQ_RST		    37

/* PERICFG resets */
/* PERI_GLOBALCON_RST0*/
#define PERI_UART0_SW_RST 	     0 
#define PERI_UART1_SW_RST 	     1 
#define PERI_UART2_SW_RST 	     2 
#define PERI_UART3_SW_RST 	     3 
#define PERI_BTIF_SW_RST		     6 
#define PERI_PWM_SW_RST		     8 
#define PERI_AUXADC_SW_RST	     10 
#define PERI_DMA_SW_RST		     11 
#define PERI_NFI_SW_RST		     14 
#define PERI_NLI_SW_RST		     15 
#define PERI_THERM_SW_RST		     16 
#define PERI_MSDC2_SW_RST		     17 
#define PERI_MSDC0_SW_RST		     19 
#define PERI_MSDC1_SW_RST		     20 
#define PERI_I2C0_SW_RST		     22 
#define PERI_I2C1_SW_RST		     23 
#define PERI_I2C2_SW_RST		     24 
#define PERI_USB_SW_RST		     28 
/* PERI_GLOBALCON_RST0*/
#define PERI_SPI0_SW_RST		     32 

/* MFG resets */
#define MFG_G3D_RESET               0
#define MFG_AXI_RESET               1

/* Watchdog */
#define MT6582_TOPRGU_INFRA_SW_RST				0
#define MT6582_TOPRGU_MM_SW_RST					1
#define MT6582_TOPRGU_MFG_SW_RST				2
#define MT6582_TOPRGU_VDEC_SW_RST				4
#define MT6582_TOPRGU_VENC_IMG_SW_RST				5
#define MT6582_TOPRGU_DDRPHY_SW_RST				6
#define MT6582_TOPRGU_MD_SW_RST					7
#define MT6582_TOPRGU_USB_SW_RST				8
#define MT6582_TOPRGU_INFRA_AO_SW_RST				9
#define MT6582_TOPRGU_CONNSYS_SW_RST				10
#define MT6582_TOPRGU_APMIXED_SW_RST				11
#define MT6582_TOPRGU_CONN_MCU_SW_RST				12

#define MT6582_TOPRGU_SW_RST_NUM				13

#endif  /* _DT_BINDINGS_RESET_CONTROLLER_MT6582 */
