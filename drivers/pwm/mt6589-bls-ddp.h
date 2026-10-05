/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DDP component interface for the MT6589 display BLS block.
 *
 * On MT6589 BLS is both the backlight PWM generator and a stage of the
 * display data path (OVL -> COLOR -> BLS -> RDMA0 -> DSI0), so the DRM
 * side drives it as a DDP component while the PWM driver owns the
 * mapping, the clock and the registers.  Every entry point below reads the
 * instance kept in pwm-mt6589-disp.c, not dev_get_drvdata(), which is NULL
 * on the component device.
 *
 * Copyright (c) 2026 MediaTek Inc.
 */

#ifndef _MT6589_BLS_DDP_H
#define _MT6589_BLS_DDP_H

#include <linux/device.h>

struct cmdq_pkt;

int mt6589_bls_ddp_clk_enable(struct device *dev);
void mt6589_bls_ddp_clk_disable(struct device *dev);
void mt6589_bls_ddp_config(struct device *dev, unsigned int w,
			   unsigned int h, unsigned int vrefresh,
			   unsigned int bpc, struct cmdq_pkt *cmdq_pkt);
void mt6589_bls_ddp_start(struct device *dev);
void mt6589_bls_ddp_stop(struct device *dev);

#endif /* _MT6589_BLS_DDP_H */
