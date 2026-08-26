/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6628 WMT driver interface for the per-subsystem drivers.
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 */

#ifndef __MTK_WMT_H
#define __MTK_WMT_H

struct mtk_wmt;
struct device;

/* Returns the single WMT context once probed, or NULL. */
struct mtk_wmt *mtk_wmt_find(void);

int mtk_wmt_send(struct mtk_wmt *wmt, u8 type,
		 const u8 *payload, size_t len);
int mtk_wmt_recv(struct mtk_wmt *wmt, u8 type,
		 u8 *payload, size_t max_len);

#endif /* __MTK_WMT_H */
