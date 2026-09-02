// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * MediaTek MT6628 WLAN driver -- SDIO HIF layer
 *
 * Copyright (c) 2026 Akari Tsuyukusa <akkun11.open@gmail.com>
 *
 * The MT6628 WLAN function lives on SDIO function 1.  Its HIF register
 * file is the same family as the MT7663 one but with different data
 * port offsets and interrupt bit positions:
 *
 *   WCIR     0x0000   chip status (WLAN_READY = BIT(21))
 *   WHLPCR   0x0004   ownership + interrupt enable (same bits as mt76)
 *   WHISR    0x0010   interrupt status (FW_OWN_BACK = BIT(4),
 *                      ABNORMAL = BIT(3), TX_DONE = BIT(0),
 *                      RX0DONE = BIT(1), RX1DONE = BIT(2))
 *   WTDR0/1  0x0028 / 0x002c   TX data ports
 *   WRDR0/1  0x0030 / 0x0034   RX data ports
 *   H2DSM0R  0x0038   host->chip SW mailbox
 *   WRPLR    0x0050   RX packet length (16bit x 2)
 *
 * Firmware download uses an "init command" protocol: a 4-byte HIF
 * header followed by a 4-byte command header, with
 * DOWNLOAD_BUF (0x01) chunks carrying address/len/crc32 and
 * WIFI_START (0x02) to boot.
 */

#include <linux/mmc/sdio_func.h>

/* register offsets */
#define MT6628_MCR_WCIR			0x0000
#define MT6628_MCR_WHLPCR		0x0004
#define MT6628_MCR_WSDIOCSR		0x0008
#define MT6628_MCR_WHCR			0x000c
#define MT6628_MCR_WHISR		0x0010
#define MT6628_MCR_WHIER		0x0014
#define MT6628_MCR_WASR			0x0018
#define MT6628_MCR_WSICR		0x001c
#define MT6628_MCR_WTSR0		0x0020
#define MT6628_MCR_WTSR1		0x0024
#define MT6628_MCR_WTDR0		0x0028
#define MT6628_MCR_WTDR1		0x002c
#define MT6628_MCR_WRDR0		0x0030
#define MT6628_MCR_WRDR1		0x0034
#define MT6628_MCR_H2DSM0R		0x0038
#define MT6628_MCR_H2DSM1R		0x003c
#define MT6628_MCR_WRPLR		0x0050

/* WHLPCR */
#define MT6628_FW_OWN_REQ_CLR		BIT(9)
#define MT6628_FW_OWN_REQ_SET		BIT(8)
#define MT6628_IS_DRIVER_OWN		BIT(8)
#define MT6628_INT_EN_CLR		BIT(1)
#define MT6628_INT_EN_SET		BIT(0)

/* WHISR / WHIER */
#define MT6628_WHISR_TX_DONE		BIT(0)
#define MT6628_WHISR_RX0_DONE		BIT(1)
#define MT6628_WHISR_RX1_DONE		BIT(2)
#define MT6628_WHISR_ABNORMAL		BIT(3)
#define MT6628_WHISR_FW_OWN_BACK	BIT(4)

/* WCIR */
#define MT6628_WCIR_WLAN_READY		BIT(21)

/* init command ids */
enum mt6628_init_cmd_id {
	MT6628_INIT_CMD_DOWNLOAD_BUF	= 1,
	MT6628_INIT_CMD_WIFI_START,
	MT6628_INIT_CMD_ACCESS_REG,
	MT6628_INIT_CMD_QUERY_PENDING_ERROR,
};

struct mt6628_init_hif_tx_hdr {
	__le16 tx_byte_count;
	u8 ether_type_offset;
	u8 cs_flags;
	u8 cid;
	u8 seq_num;
	__le16 reserved;
} __packed;

struct mt6628_init_cmd_download_buf {
	__le32 address;
	__le32 length;
	__le32 crc32;
	__le32 data_mode;
} __packed;
