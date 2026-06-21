/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __DTS_MT6320_PINFUNC_H
#define __DTS_MT6320_PINFUNC_H

#include <dt-bindings/pinctrl/mt65xx.h>


#define MT6320_PIN_0_GPIO0__FUNC_GPIO0                            (MTK_PIN_NO(0) | 0)
#define MT6320_PIN_0_GPIO0__FUNC_INT                              (MTK_PIN_NO(0) | 1)

#define MT6320_PIN_1_GPIO1__FUNC_GPIO1                            (MTK_PIN_NO(1) | 0)
#define MT6320_PIN_1_GPIO1__FUNC_SRCVOLTEN                        (MTK_PIN_NO(1) | 1)

#define MT6320_PIN_2_GPIO2__FUNC_GPIO2                            (MTK_PIN_NO(2) | 0)
#define MT6320_PIN_2_GPIO2__FUNC_SRCLKEN_PERI                     (MTK_PIN_NO(2) | 1)

#define MT6320_PIN_3_GPIO3__FUNC_GPIO3                            (MTK_PIN_NO(3) | 0)
#define MT6320_PIN_3_GPIO3__FUNC_SRCLKEN_MD2                      (MTK_PIN_NO(3) | 1)

#define MT6320_PIN_4_GPIO4__FUNC_GPIO4                            (MTK_PIN_NO(4) | 0)
#define MT6320_PIN_4_GPIO4__FUNC_RTC_32K1V8                       (MTK_PIN_NO(4) | 1)

#define MT6320_PIN_5_GPIO5__FUNC_GPIO5                            (MTK_PIN_NO(5) | 0)
#define MT6320_PIN_5_GPIO5__FUNC_WRAP_EVENT                       (MTK_PIN_NO(5) | 1)

#define MT6320_PIN_6_GPIO6__FUNC_GPIO6                            (MTK_PIN_NO(6) | 0)
#define MT6320_PIN_6_GPIO6__FUNC_SPI_CLK                          (MTK_PIN_NO(6) | 1)

#define MT6320_PIN_7_GPIO7__FUNC_GPIO7                            (MTK_PIN_NO(7) | 0)
#define MT6320_PIN_7_GPIO7__FUNC_SPI_CSN                          (MTK_PIN_NO(7) | 1)

#define MT6320_PIN_8_GPIO8__FUNC_GPIO8                            (MTK_PIN_NO(8) | 0)
#define MT6320_PIN_8_GPIO8__FUNC_SPI_MOSI                         (MTK_PIN_NO(8) | 1)

#define MT6320_PIN_9_GPIO9__FUNC_GPIO9                            (MTK_PIN_NO(9) | 0)
#define MT6320_PIN_9_GPIO9__FUNC_SPI_MISO                         (MTK_PIN_NO(9) | 1)

#define MT6320_PIN_10_GPIO10__FUNC_GPIO10                         (MTK_PIN_NO(10) | 0)
#define MT6320_PIN_10_GPIO10__FUNC_ADC_CK                         (MTK_PIN_NO(10) | 1)

#define MT6320_PIN_11_GPIO11__FUNC_GPIO11                         (MTK_PIN_NO(11) | 0)
#define MT6320_PIN_11_GPIO11__FUNC_ADC_WS                         (MTK_PIN_NO(11) | 1)

#define MT6320_PIN_12_GPIO12__FUNC_GPIO12                         (MTK_PIN_NO(12) | 0)
#define MT6320_PIN_12_GPIO12__FUNC_ADC_DAT                        (MTK_PIN_NO(12) | 1)

#define MT6320_PIN_13_GPIO13__FUNC_GPIO13                         (MTK_PIN_NO(13) | 0)
#define MT6320_PIN_13_GPIO13__FUNC_DAC_CK                         (MTK_PIN_NO(13) | 1)

#define MT6320_PIN_14_GPIO14__FUNC_GPIO14                         (MTK_PIN_NO(14) | 0)
#define MT6320_PIN_14_GPIO14__FUNC_DAC_WS                         (MTK_PIN_NO(14) | 1)

#define MT6320_PIN_15_GPIO15__FUNC_GPIO15                         (MTK_PIN_NO(15) | 0)
#define MT6320_PIN_15_GPIO15__FUNC_DAC_DAT                        (MTK_PIN_NO(15) | 1)

#define MT6320_PIN_16_GPIO16__FUNC_GPIO16                         (MTK_PIN_NO(16) | 0)
#define MT6320_PIN_16_GPIO16__FUNC_COL0_USBDL                     (MTK_PIN_NO(16) | 1)
#define MT6320_PIN_16_GPIO16__FUNC_EINT10                         (MTK_PIN_NO(16) | 2)
#define MT6320_PIN_16_GPIO16__FUNC_PWM1_3X                        (MTK_PIN_NO(16) | 3)

#define MT6320_PIN_17_GPIO17__FUNC_GPIO17                         (MTK_PIN_NO(17) | 0)
#define MT6320_PIN_17_GPIO17__FUNC_COL1                           (MTK_PIN_NO(17) | 1)
#define MT6320_PIN_17_GPIO17__FUNC_EINT11                         (MTK_PIN_NO(17) | 2)
#define MT6320_PIN_17_GPIO17__FUNC_SCL0_2X                        (MTK_PIN_NO(17) | 3)

#define MT6320_PIN_18_GPIO18__FUNC_GPIO18                         (MTK_PIN_NO(18) | 0)
#define MT6320_PIN_18_GPIO18__FUNC_COL2                           (MTK_PIN_NO(18) | 1)
#define MT6320_PIN_18_GPIO18__FUNC_EINT12                         (MTK_PIN_NO(18) | 2)
#define MT6320_PIN_18_GPIO18__FUNC_SDA0_2X                        (MTK_PIN_NO(18) | 3)

#define MT6320_PIN_19_GPIO19__FUNC_GPIO19                         (MTK_PIN_NO(19) | 0)
#define MT6320_PIN_19_GPIO19__FUNC_COL3                           (MTK_PIN_NO(19) | 1)
#define MT6320_PIN_19_GPIO19__FUNC_EINT13                         (MTK_PIN_NO(19) | 2)
#define MT6320_PIN_19_GPIO19__FUNC_SCL1_2X                        (MTK_PIN_NO(19) | 3)

#define MT6320_PIN_20_GPIO20__FUNC_GPIO20                         (MTK_PIN_NO(20) | 0)
#define MT6320_PIN_20_GPIO20__FUNC_COL4                           (MTK_PIN_NO(20) | 1)
#define MT6320_PIN_20_GPIO20__FUNC_EINT14                         (MTK_PIN_NO(20) | 2)
#define MT6320_PIN_20_GPIO20__FUNC_SDA1_2X                        (MTK_PIN_NO(20) | 3)

#define MT6320_PIN_21_GPIO21__FUNC_GPIO21                         (MTK_PIN_NO(21) | 0)
#define MT6320_PIN_21_GPIO21__FUNC_COL5                           (MTK_PIN_NO(21) | 1)
#define MT6320_PIN_21_GPIO21__FUNC_EINT15                         (MTK_PIN_NO(21) | 2)
#define MT6320_PIN_21_GPIO21__FUNC_SCL2_2X                        (MTK_PIN_NO(21) | 3)

#define MT6320_PIN_22_GPIO22__FUNC_GPIO22                         (MTK_PIN_NO(22) | 0)
#define MT6320_PIN_22_GPIO22__FUNC_COL6                           (MTK_PIN_NO(22) | 1)
#define MT6320_PIN_22_GPIO22__FUNC_EINT16                         (MTK_PIN_NO(22) | 2)
#define MT6320_PIN_22_GPIO22__FUNC_SDA2_2X                        (MTK_PIN_NO(22) | 3)
#define MT6320_PIN_22_GPIO22__FUNC_GPIO32K_0                      (MTK_PIN_NO(22) | 4)
#define MT6320_PIN_22_GPIO22__FUNC_GPIO26M_0                      (MTK_PIN_NO(22) | 5)

#define MT6320_PIN_23_GPIO23__FUNC_GPIO23                         (MTK_PIN_NO(23) | 0)
#define MT6320_PIN_23_GPIO23__FUNC_COL7                           (MTK_PIN_NO(23) | 1)
#define MT6320_PIN_23_GPIO23__FUNC_EINT17                         (MTK_PIN_NO(23) | 2)
#define MT6320_PIN_23_GPIO23__FUNC_PWM2_3X                        (MTK_PIN_NO(23) | 3)
#define MT6320_PIN_23_GPIO23__FUNC_GPIO32K_1                      (MTK_PIN_NO(23) | 4)
#define MT6320_PIN_23_GPIO23__FUNC_GPIO26M_1                      (MTK_PIN_NO(23) | 5)

#define MT6320_PIN_24_GPIO24__FUNC_GPIO24                         (MTK_PIN_NO(24) | 0)
#define MT6320_PIN_24_GPIO24__FUNC_ROW0                           (MTK_PIN_NO(24) | 1)
#define MT6320_PIN_24_GPIO24__FUNC_EINT18                         (MTK_PIN_NO(24) | 2)
#define MT6320_PIN_24_GPIO24__FUNC_SCL0_3X                        (MTK_PIN_NO(24) | 3)

#define MT6320_PIN_25_GPIO25__FUNC_GPIO25                         (MTK_PIN_NO(25) | 0)
#define MT6320_PIN_25_GPIO25__FUNC_ROW1                           (MTK_PIN_NO(25) | 1)
#define MT6320_PIN_25_GPIO25__FUNC_EINT19                         (MTK_PIN_NO(25) | 2)
#define MT6320_PIN_25_GPIO25__FUNC_SDA0_3X                        (MTK_PIN_NO(25) | 3)

#define MT6320_PIN_26_GPIO26__FUNC_GPIO26                         (MTK_PIN_NO(26) | 0)
#define MT6320_PIN_26_GPIO26__FUNC_ROW2                           (MTK_PIN_NO(26) | 1)
#define MT6320_PIN_26_GPIO26__FUNC_EINT20                         (MTK_PIN_NO(26) | 2)
#define MT6320_PIN_26_GPIO26__FUNC_SCL1_3X                        (MTK_PIN_NO(26) | 3)

#define MT6320_PIN_27_GPIO27__FUNC_GPIO27                         (MTK_PIN_NO(27) | 0)
#define MT6320_PIN_27_GPIO27__FUNC_ROW3                           (MTK_PIN_NO(27) | 1)
#define MT6320_PIN_27_GPIO27__FUNC_EINT21                         (MTK_PIN_NO(27) | 2)
#define MT6320_PIN_27_GPIO27__FUNC_SDA1_3X                        (MTK_PIN_NO(27) | 3)

#define MT6320_PIN_28_GPIO28__FUNC_GPIO28                         (MTK_PIN_NO(28) | 0)
#define MT6320_PIN_28_GPIO28__FUNC_ROW4                           (MTK_PIN_NO(28) | 1)
#define MT6320_PIN_28_GPIO28__FUNC_EINT22                         (MTK_PIN_NO(28) | 2)
#define MT6320_PIN_28_GPIO28__FUNC_SCL2_3X                        (MTK_PIN_NO(28) | 3)

#define MT6320_PIN_29_GPIO29__FUNC_GPIO29                         (MTK_PIN_NO(29) | 0)
#define MT6320_PIN_29_GPIO29__FUNC_ROW5                           (MTK_PIN_NO(29) | 1)
#define MT6320_PIN_29_GPIO29__FUNC_EINT23                         (MTK_PIN_NO(29) | 2)
#define MT6320_PIN_29_GPIO29__FUNC_SDA2_3X                        (MTK_PIN_NO(29) | 3)

#define MT6320_PIN_30_GPIO30__FUNC_GPIO30                         (MTK_PIN_NO(30) | 0)
#define MT6320_PIN_30_GPIO30__FUNC_ROW6                           (MTK_PIN_NO(30) | 1)
#define MT6320_PIN_30_GPIO30__FUNC_EINT24                         (MTK_PIN_NO(30) | 2)
#define MT6320_PIN_30_GPIO30__FUNC_PWM3_3X                        (MTK_PIN_NO(30) | 3)
#define MT6320_PIN_30_GPIO30__FUNC_GPIO32K_2                      (MTK_PIN_NO(30) | 4)
#define MT6320_PIN_30_GPIO30__FUNC_GPIO26M_2                      (MTK_PIN_NO(30) | 5)

#define MT6320_PIN_31_GPIO31__FUNC_GPIO31                         (MTK_PIN_NO(31) | 0)
#define MT6320_PIN_31_GPIO31__FUNC_ROW7                           (MTK_PIN_NO(31) | 1)
#define MT6320_PIN_31_GPIO31__FUNC_EINT3                          (MTK_PIN_NO(31) | 2)
#define MT6320_PIN_31_GPIO31__FUNC_GPIO32K_3                      (MTK_PIN_NO(31) | 4)
#define MT6320_PIN_31_GPIO31__FUNC_GPIO26M_3                      (MTK_PIN_NO(31) | 5)

#define MT6320_PIN_32_GPIO32__FUNC_GPIO32                         (MTK_PIN_NO(32) | 0)
#define MT6320_PIN_32_GPIO32__FUNC_PWM1                           (MTK_PIN_NO(32) | 1)
#define MT6320_PIN_32_GPIO32__FUNC_EINT4                          (MTK_PIN_NO(32) | 2)
#define MT6320_PIN_32_GPIO32__FUNC_GPIO32K_4                      (MTK_PIN_NO(32) | 4)
#define MT6320_PIN_32_GPIO32__FUNC_GPIO26M_4                      (MTK_PIN_NO(32) | 5)

#define MT6320_PIN_33_GPIO33__FUNC_GPIO33                         (MTK_PIN_NO(33) | 0)
#define MT6320_PIN_33_GPIO33__FUNC_PWM2                           (MTK_PIN_NO(33) | 1)
#define MT6320_PIN_33_GPIO33__FUNC_EINT5                          (MTK_PIN_NO(33) | 2)
#define MT6320_PIN_33_GPIO33__FUNC_GPIO32K_5                      (MTK_PIN_NO(33) | 4)
#define MT6320_PIN_33_GPIO33__FUNC_GPIO26M_5                      (MTK_PIN_NO(33) | 5)

#define MT6320_PIN_34_GPIO34__FUNC_GPIO34                         (MTK_PIN_NO(34) | 0)
#define MT6320_PIN_34_GPIO34__FUNC_PWM3                           (MTK_PIN_NO(34) | 1)
#define MT6320_PIN_34_GPIO34__FUNC_EINT6                          (MTK_PIN_NO(34) | 2)
#define MT6320_PIN_34_GPIO34__FUNC_COL0                           (MTK_PIN_NO(34) | 3)
#define MT6320_PIN_34_GPIO34__FUNC_GPIO32K_6                      (MTK_PIN_NO(34) | 4)
#define MT6320_PIN_34_GPIO34__FUNC_GPIO26M_6                      (MTK_PIN_NO(34) | 5)

#define MT6320_PIN_35_GPIO35__FUNC_GPIO35                         (MTK_PIN_NO(35) | 0)
#define MT6320_PIN_35_GPIO35__FUNC_SCL0                           (MTK_PIN_NO(35) | 1)
#define MT6320_PIN_35_GPIO35__FUNC_EINT7                          (MTK_PIN_NO(35) | 2)
#define MT6320_PIN_35_GPIO35__FUNC_PWM1_2X                        (MTK_PIN_NO(35) | 3)

#define MT6320_PIN_36_GPIO36__FUNC_GPIO36                         (MTK_PIN_NO(36) | 0)
#define MT6320_PIN_36_GPIO36__FUNC_SDA0                           (MTK_PIN_NO(36) | 1)
#define MT6320_PIN_36_GPIO36__FUNC_EINT8                          (MTK_PIN_NO(36) | 2)

#define MT6320_PIN_37_GPIO37__FUNC_GPIO37                         (MTK_PIN_NO(37) | 0)
#define MT6320_PIN_37_GPIO37__FUNC_SCL1                           (MTK_PIN_NO(37) | 1)
#define MT6320_PIN_37_GPIO37__FUNC_EINT9                          (MTK_PIN_NO(37) | 2)
#define MT6320_PIN_37_GPIO37__FUNC_PWM2_2X                        (MTK_PIN_NO(37) | 3)

#define MT6320_PIN_38_GPIO38__FUNC_GPIO38                         (MTK_PIN_NO(38) | 0)
#define MT6320_PIN_38_GPIO38__FUNC_SDA1                           (MTK_PIN_NO(38) | 1)
#define MT6320_PIN_38_GPIO38__FUNC_EINT0                          (MTK_PIN_NO(38) | 2)

#define MT6320_PIN_39_GPIO39__FUNC_GPIO39                         (MTK_PIN_NO(39) | 0)
#define MT6320_PIN_39_GPIO39__FUNC_SCL2                           (MTK_PIN_NO(39) | 1)
#define MT6320_PIN_39_GPIO39__FUNC_EINT1                          (MTK_PIN_NO(39) | 2)
#define MT6320_PIN_39_GPIO39__FUNC_PWM3_2X                        (MTK_PIN_NO(39) | 3)

#define MT6320_PIN_40_GPIO40__FUNC_GPIO40                         (MTK_PIN_NO(40) | 0)
#define MT6320_PIN_40_GPIO40__FUNC_SDA2                           (MTK_PIN_NO(40) | 1)
#define MT6320_PIN_40_GPIO40__FUNC_EINT2                          (MTK_PIN_NO(40) | 2)

#define MT6320_PIN_41_GPIO41__FUNC_GPIO41                         (MTK_PIN_NO(41) | 0)
#define MT6320_PIN_41_GPIO41__FUNC_SIM1_AP_SCLK                   (MTK_PIN_NO(41) | 1)

#define MT6320_PIN_42_GPIO42__FUNC_GPIO42                         (MTK_PIN_NO(42) | 0)
#define MT6320_PIN_42_GPIO42__FUNC_SIM1_AP_SRST                   (MTK_PIN_NO(42) | 1)

#define MT6320_PIN_43_GPIO43__FUNC_GPIO43                         (MTK_PIN_NO(43) | 0)
#define MT6320_PIN_43_GPIO43__FUNC_SIM2_AP_SCLK                   (MTK_PIN_NO(43) | 1)

#define MT6320_PIN_44_GPIO44__FUNC_GPIO44                         (MTK_PIN_NO(44) | 0)
#define MT6320_PIN_44_GPIO44__FUNC_SIM2_AP_SRST                   (MTK_PIN_NO(44) | 1)

#define MT6320_PIN_45_GPIO45__FUNC_GPIO45                         (MTK_PIN_NO(45) | 0)
#define MT6320_PIN_45_GPIO45__FUNC_SIMLS1_SCLK                    (MTK_PIN_NO(45) | 1)

#define MT6320_PIN_46_GPIO46__FUNC_GPIO46                         (MTK_PIN_NO(46) | 0)
#define MT6320_PIN_46_GPIO46__FUNC_SIMLS1_SRST                    (MTK_PIN_NO(46) | 1)

#define MT6320_PIN_47_GPIO47__FUNC_GPIO47                         (MTK_PIN_NO(47) | 0)
#define MT6320_PIN_47_GPIO47__FUNC_SIMLS2_SCLK                    (MTK_PIN_NO(47) | 1)
#define MT6320_PIN_47_GPIO47__FUNC_EINT10                         (MTK_PIN_NO(47) | 5)

#define MT6320_PIN_48_GPIO48__FUNC_GPIO48                         (MTK_PIN_NO(48) | 0)
#define MT6320_PIN_48_GPIO48__FUNC_SIMLS2_SRST                    (MTK_PIN_NO(48) | 1)

#endif /* __DTS_MT6320_PINFUNC_H */
