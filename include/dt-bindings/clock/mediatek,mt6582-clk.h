// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 
 * Author: Burst_Caster <swer15l23@gmail.com>
 */

#ifndef _DT_BINDINGS_CLK_MT6582_H
#define _DT_BINDINGS_CLK_MT6582_H

/* TOPCKGEN */

/*fixed factor*/
#define CLK_TOP_SYSPLL_D2			1
#define CLK_TOP_SYSPLL_D3			2
#define CLK_TOP_SYSPLL_D5			3
#define CLK_TOP_SYSPLL_D7			4

#define CLK_TOP_SYSPLL1_D2			5
#define CLK_TOP_SYSPLL1_D4			6
#define CLK_TOP_SYSPLL1_D8			7
#define CLK_TOP_SYSPLL1_D16			8
#define CLK_TOP_SYSPLL2_D2			9
#define CLK_TOP_SYSPLL2_D4			10
#define CLK_TOP_SYSPLL2_D8			11

#define CLK_TOP_SYSPLL3_D2			12
#define CLK_TOP_SYSPLL3_D4			13
#define CLK_TOP_SYSPLL4_D2			14

#define CLK_TOP_UNIVPLL_D2			15
#define CLK_TOP_UNIVPLL_D3			16
#define CLK_TOP_UNIVPLL_D5			17
#define CLK_TOP_UNIVPLL_D7			18

#define CLK_TOP_UNIVPLL1_D2			19
#define CLK_TOP_UNIVPLL1_D4			20
#define CLK_TOP_UNIVPLL1_D8			21

#define CLK_TOP_UNIVPLL2_D2			22
#define CLK_TOP_UNIVPLL2_D4			23
#define CLK_TOP_UNIVPLL2_D8			24
#define CLK_TOP_UNIVPLL2_D16			25
#define CLK_TOP_UNIVPLL2_D32			26

#define CLK_TOP_UNIVPLL3_D2			27
#define CLK_TOP_UNIVPLL3_D4			28

#define CLK_TOP_MSDCPLL_D2			29

#define CLK_TOP_MMPLL_D2			30

#define CLK_TOP_DMPLL				31
#define CLK_TOP_DMPLL_D2			32
#define CLK_TOP_DMPLL_D4			33
#define CLK_TOP_DMPLL_X2			34

/*muxes*/
#define CLK_TOP_AXI_SEL				35
#define CLK_TOP_MEM_SEL				36
#define CLK_TOP_DDRPHYCFG_SEL			37
#define CLK_TOP_MM_SEL				38
#define CLK_TOP_PWM_SEL				39
#define CLK_TOP_VDEC_SEL			40
#define CLK_TOP_MFG_SEL				41
#define CLK_TOP_CAMTG_SEL			42
#define CLK_TOP_UART_SEL			43
#define CLK_TOP_SPI_SEL			    	44
#define CLK_TOP_USB20_SEL			45
#define CLK_TOP_MSDC30_0_SEL	    		46
#define CLK_TOP_MSDC30_1_SEL			47
#define CLK_TOP_MSDC30_2_SEL			48
#define CLK_TOP_AUDIO_SEL			49
#define CLK_TOP_AUD_INTBUS_SEL			50
#define CLK_TOP_PMICSPI_SEL			51
#define CLK_TOP_SCP_SEL				52

#define CLK_TOP_NR_CLK			    	53


/* APMIXEDSYS */

#define CLK_APMIXED_ARMPLL			1
#define CLK_APMIXED_MAINPLL			2
#define CLK_APMIXED_UNIVPLL			3
#define CLK_APMIXED_MMPLL			4
#define CLK_APMIXED_MSDCPLL			5

#define CLK_APMIXED_UNIV48M         		6
#define CLK_APMIXED_USB48M          		7


#define CLK_APMIXED_NR_CLK			8


/* DDRPHY */

#define CLK_DDRPHY_VENCPLL			1
#define CLK_DDRPHY_NR_CLK			2


/* INFRACFG */

#define CLK_INFRA_DBG				1
#define CLK_INFRA_SMI				2
#define CLK_INFRA_AUDIO				5
#define CLK_INFRA_EFUSE				6
#define CLK_INFRA_L2C_SRAM			7
#define CLK_INFRA_M4U				8
#define CLK_INFRA_MD1_CR4_AXI       		9
#define CLK_INFRA_MD1_HWMIX_AXI     		10
#define CLK_INFRA_MD1_AHB           		11
#define CLK_INFRA_CONNMCU			12
#define CLK_INFRA_TRNG				13
#define CLK_INFRA_RAMBUFIF			14
#define CLK_INFRA_CPUM				15
#define CLK_INFRA_KP				16
#define CLK_INFRA_CCIF0_AP_CTRL 		17
#define CLK_INFRA_PMICWRAP			18

#define CLK_INFRA_CLK_13M           		19
#define CLK_INFRA_CPUSEL            		20

#define CLK_INFRA_NR_CLK			21

/* PERICFG */

#define CLK_PERI_SPI0				1
#define CLK_PERI_AUXADC				2
#define CLK_PERI_I2C2				3
#define CLK_PERI_I2C1				4
#define CLK_PERI_I2C0				5
#define CLK_PERI_BTIF				6
#define CLK_PERI_UART3				7
#define CLK_PERI_UART2				8
#define CLK_PERI_UART1				9
#define CLK_PERI_UART0				10
#define CLK_PERI_NLI				11
#define CLK_PERI_MSDC30_2			12
#define CLK_PERI_MSDC30_1			13
#define CLK_PERI_MSDC30_0			14
#define CLK_PERI_AP_DMA				15
#define CLK_PERI_USB0				16
#define CLK_PERI_PWM				17
#define CLK_PERI_PWM7				18
#define CLK_PERI_PWM6			    	19
#define CLK_PERI_PWM5				20
#define CLK_PERI_PWM4				21
#define CLK_PERI_PWM3				22
#define CLK_PERI_PWM2				23
#define CLK_PERI_PWM1				24
#define CLK_PERI_THERM				25
#define CLK_PERI_NFI				26
#define CLK_PERI_UART_SEL			27
#define CLK_PERI_NR_CLK			    	28


/* MMSYS */

#define CLK_MM_SMI_COMMON			0
#define CLK_MM_SMI_LARB0			1
#define CLK_MM_CMDQ				2
#define CLK_MM_SMI_CMDQ				3
#define CLK_MM_DISP_COLOR			4
#define CLK_MM_DISP_BLS				5
#define CLK_MM_DISP_WDMA			6
#define CLK_MM_DISP_RDMA			7
#define CLK_MM_DISP_OVL				8
#define CLK_MM_MDP_TDSHP			9
#define CLK_MM_MDP_WROT				10
#define CLK_MM_MDP_WDMA				11
#define CLK_MM_MDP_RSZ1				12
#define CLK_MM_MDP_RSZ0				13
#define CLK_MM_MDP_RDMA				14
#define CLK_MM_MDP_BLS_26M			15
#define CLK_MM_CAM_MDP				16
#define CLK_MM_FAKE_ENG				17
#define CLK_MM_MUTEX_32K			18

#define CLK_MM_DSI_ENGINE			19
#define CLK_MM_DSI_DIG				20
#define CLK_MM_DPI_DIGL				21
#define CLK_MM_DPI_ENGINE			22
#define CLK_MM_NR				23

/* MFGCFG */

#define CLK_MFG_BG3D				1
#define CLK_MFG_NR_CLK				2

/* IMG */

#define CLK_IMG_LARB2_SMI       		0
#define CLK_IMG_SMI             		5
#define CLK_IMG_CAM             		6
#define CLK_IMG_SEN_TG          		7
#define CLK_IMG_SEN_CAM         		8
#define CLK_IMG_VENC_JPEGENC    		9
#define CLK_IMG_NR_CLK          		10

/* VDEC */

#define CLK_VDEC_CKGEN				1
#define CLK_VDEC_LARB				2
#define CLK_VDEC_NR				3


#endif /* _DT_BINDINGS_CLK_MT6582_H */
