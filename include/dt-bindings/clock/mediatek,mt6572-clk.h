// SPDX-License-Identifier: GPL-2.0+

#ifndef _DT_BINDINGS_CLK_MT6572_H
#define _DT_BINDINGS_CLK_MT6572_H

/* TOPCKGEN */
#define CLK_TOP_SC_26M_SEL		0
#define CLK_TOP_UNIV_48M_SEL	1

#define CLK_TOP_MPLL_D2		2
#define CLK_TOP_MPLL_D3		3
#define CLK_TOP_MPLL_D5		4
#define CLK_TOP_MPLL_D7		5
#define CLK_TOP_MPLL_D4		6
#define CLK_TOP_MPLL_D6		7
#define CLK_TOP_MPLL_D10	8
#define CLK_TOP_MPLL_D8		9
#define CLK_TOP_MPLL_D12	10
#define CLK_TOP_MPLL_D20	11
#define CLK_TOP_MPLL_D24	12
#define CLK_TOP_UPLL_D2		13
#define CLK_TOP_UPLL_D3		14
#define CLK_TOP_UPLL_D5		15
#define CLK_TOP_UPLL_D7		16
#define CLK_TOP_UPLL_D4		17
#define CLK_TOP_UPLL_D6		18
#define CLK_TOP_UPLL_D10	19
#define CLK_TOP_UPLL_D8		20
#define CLK_TOP_UPLL_D12	21
#define CLK_TOP_UPLL_D20	22
#define CLK_TOP_UPLL_D16	23
#define CLK_TOP_UPLL_D24	24

#define CLK_TOP_UART0_SEL				 	25
#define CLK_TOP_EMI2X_SEL				 	26
#define CLK_TOP_AXI_SEL					 	27
#define CLK_TOP_MFG_MUX_SEL			 	28
#define CLK_TOP_MSDC0_SEL				 	29
#define CLK_TOP_SPINFI_SEL			 	30
#define CLK_TOP_CAM_SEL					 	31
#define CLK_TOP_PWM_MM_SEL			 	32
#define CLK_TOP_UART1_SEL				 	34
#define CLK_TOP_MSDC1_SEL				 	35
#define CLK_TOP_SPM_52M_SEL			 	36
#define CLK_TOP_PMIC_SPI_SEL		 	37
#define CLK_TOP_AUDIO_INTBUS_SEL	38
#define CLK_TOP_SPINFI_PRE_SEL		39
#define CLK_TOP_MFG_SEL						40

/* CG0 */
#define CLK_TOP_PWM_MM				41
#define CLK_TOP_CAM_MM				42
#define CLK_TOP_MFG_MM				43
#define CLK_TOP_SPM_52M				44
#define CLK_TOP_MIPI_26M_DBG	45
#define CLK_TOP_DBI_BCLK			46
#define CLK_TOP_SC_26M				47
#define CLK_TOP_SC_MEM				48
#define CLK_TOP_DBI_PAD0			49
#define CLK_TOP_DBI_PAD1			50
#define CLK_TOP_DBI_PAD2			51
#define CLK_TOP_DBI_PAD3			52
#define CLK_TOP_MFG_PRE_491M	53
#define CLK_TOP_MFG_PRE_500M	54
#define CLK_TOP_ARMDCM				55

/* CG1 */
#define CLK_TOP_EFUSE							56
#define CLK_TOP_THERMAL						57
#define CLK_TOP_APDMA							58
#define CLK_TOP_I2C0							59
#define CLK_TOP_I2C1							60
#define CLK_TOP_NFI								61
#define CLK_TOP_NFI_ECC						62
#define CLK_TOP_DEBUGSYS					63
#define CLK_TOP_PWM								64
#define CLK_TOP_UART0							65
#define CLK_TOP_UART1							66
#define CLK_TOP_BTIF							67
#define CLK_TOP_USB								68
#define CLK_TOP_FHCTL							69
#define CLK_TOP_SPINFI						70
#define CLK_TOP_MSDC0							71
#define CLK_TOP_MSDC1							72
#define CLK_TOP_PMIC_WRAP					73
#define CLK_TOP_SEJ								74
#define CLK_TOP_MEMSLP_DLYER			75
#define CLK_TOP_APXGPT						76
#define CLK_TOP_AUDIO							77
#define CLK_TOP_SPM								78
#define CLK_TOP_PMIC_SPI					79
#define CLK_TOP_AUXADC						80
#define CLK_TOP_NR_CLK						81

#define CLK_APMIXED_ARMPLL		0
#define CLK_APMIXED_MAINPLL		1
//#define CLK_APMIXED_UNIVPLL 2
#define CLK_APMIXED_WHPLL			2

#define CLK_INFRA_CPUSEL	0

#define CLK_MM_SMI_COMMON 0
#define CLK_MM_SMI_LARB0 1
#define CLK_MM_CMDQ 2
#define CLK_MM_SMI_CMDQ 3
#define CLK_MM_DISP_COLOR 4
#define CLK_MM_DISP_BLS 5
#define CLK_MM_DISP_WDMA 6
#define CLK_MM_DISP_RDMA 7
#define CLK_MM_DISP_OVL 8
#define CLK_MM_DISP_MDP_TDSHP 9
#define CLK_MM_DISP_MDP_WROT 10
#define CLK_MM_DISP_MDP_WDMA 11
#define CLK_MM_DISP_MDP_RSZ1 12
#define CLK_MM_DISP_MDP_RSZ0 13
#define CLK_MM_DISP_MDP_RDMA 14
#define CLK_MM_DISP_MDP_BLS_26M 15
#define CLK_MM_CAM 16
#define CLK_MM_SENINF 17
#define CLK_MM_CAMTG 18
#define CLK_MM_CODEC 19
#define CLK_MM_DISP_FAKE_ENG 20
#define CLK_MM_MUTEX_SLOW_CLOCK 21
#define CLK_MM_DSI_ENGINE 22
#define CLK_MM_DSI_DIGITAL 23

#define CLK_MFG_BG3D 0

#endif /* _DT_BINDINGS_CLK_MT6572_H */
