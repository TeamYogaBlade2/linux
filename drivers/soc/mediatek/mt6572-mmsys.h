/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2026 rva3 <rva333@protonmail.com>
 */

#ifndef __SOC_MEDIATEK_MT6572_MMSYS_H
#define __SOC_MEDIATEK_MT6572_MMSYS_H

static const struct mtk_mmsys_routes mt6572_mmsys_routing_table[] = {
	/* ovl0 goes to rdma0 */
	MMSYS_ROUTE(OVL0, RDMA0, 0x030, BIT(0), BIT(0)),

	/* rdma0 goes to color0 */
	MMSYS_ROUTE(RDMA0, COLOR0, 0x050, BIT(0), 0),
	MMSYS_ROUTE(RDMA0, COLOR0, 0x054, BIT(0), 0),

	/* XXX: BLS, not aal... */
	MMSYS_ROUTE(AAL0, DSI0, 0x058, BIT(0), 0),
	MMSYS_ROUTE(AAL0, DSI0, 0x04C, 0x3, 0),

	/* ?????? */
	MMSYS_ROUTE(RDMA0, DSI0, 0x050, BIT(0), BIT(0)),
	MMSYS_ROUTE(RDMA0, DSI0, 0x058, BIT(0), BIT(0)),
	MMSYS_ROUTE(RDMA0, DSI0, 0x04C, 0x3, 0),

	/* ???????????????????????? */
	MMSYS_ROUTE(OVL0, COLOR0, 0x030, BIT(2), BIT(2)),
	MMSYS_ROUTE(OVL0, COLOR0, 0x054, BIT(0), BIT(0)),
};

#endif /* __SOC_MEDIATEK_MT6572_MMSYS_H */
