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

#include <linux/bits.h>
#include <linux/mmc/sdio_func.h>

#define MT6628_WLAN_SDIO_BLK_SIZE	512

size_t mt6628_sdio_xfer_len(size_t len);

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
#define MT6628_MCR_D2HRM0R		0x0040
#define MT6628_MCR_D2HRM1R		0x0044
#define MT6628_MCR_D2HRM2R		0x0048
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
#define MT6628_WHISR_D2H_SW_ASSERT_INFO	BIT(31)

/* WHIER */
#define MT6628_WHIER_TX_DONE		BIT(0)
#define MT6628_WHIER_RX0_DONE		BIT(1)
#define MT6628_WHIER_RX1_DONE		BIT(2)
#define MT6628_WHIER_ABNORMAL		BIT(3)
#define MT6628_WHIER_FW_OWN_BACK	BIT(4)
#define MT6628_WHIER_D2H_SW_ASSERT_INFO	BIT(31)

#define MT6628_WHIER_RUNTIME		(MT6628_WHIER_TX_DONE | \
					 MT6628_WHIER_RX0_DONE | \
					 MT6628_WHIER_RX1_DONE | \
					 MT6628_WHIER_ABNORMAL | \
					 MT6628_WHIER_D2H_SW_ASSERT_INFO)

/* Runtime command/event protocol. */
#define MT6628_WIFI_CMD_HEADER_LEN	8
#define MT6628_WIFI_EVENT_HEADER_LEN	8
#define MT6628_HIF_RX_HEADER_LEN	12
#define MT6628_HIF_TX_RESOURCE_OFFSET	2
#define MT6628_HIF_TX_PACKET_TYPE_OFFSET	6
#define MT6628_HIF_TX_PKT_TYPE_CMD	1
#define MT6628_HIF_TX_PKT_TYPE_MANAGEMENT	3
#define MT6628_TX_TC_CMD		4
#define MT6628_TX_TC_MGMT		4
#define MT6628_HIF_TX_80211_FORMAT	BIT(7)
#define MT6628_HIF_TX_1X_FRAME		BIT(6)
#define MT6628_HIF_TX_NEED_ACK	BIT(0)

#define MT6628_EVENT_ID_CMD_RESULT	1
#define MT6628_EVENT_ID_BASIC_CONFIG	9
#define MT6628_EVENT_ID_SCAN_DONE	0x15
#define MT6628_EVENT_ID_TX_DONE	0x17
#define MT6628_EVENT_ID_CH_PRIVILEGE	0x18

#define MT6628_CMD_ID_SCAN_REQ_V2	0x04
#define MT6628_CMD_ID_ADD_REMOVE_KEY	0x08
#define MT6628_CMD_ID_SCAN_CANCEL	0x1f
#define MT6628_CMD_ID_BSS_ACTIVATE_CTRL	0x15
#define MT6628_CMD_ID_SET_BSS_INFO	0x16
#define MT6628_CMD_ID_UPDATE_STA_RECORD	0x17
#define MT6628_CMD_ID_REMOVE_STA_RECORD	0x18
#define MT6628_CMD_ID_CH_PRIVILEGE	0x20
#define MT6628_CMD_ID_BASIC_CONFIG	0xc1

#define MT6628_BAND_2GHZ		0
#define MT6628_BAND_5GHZ		1

struct mt6628_wifi_cmd_hdr {
	__le16 tx_byte_count_user_priority;
	u8 ether_type_offset;
	u8 resource_pkt_type_csflags;
	u8 cid;
	u8 set_query;
	u8 seq_num;
	u8 reserved;
} __packed;

struct mt6628_wifi_event_hdr {
	__le16 packet_len;
	__le16 packet_type;
	u8 eid;
	u8 seq_num;
	u8 reserved[2];
} __packed;

static_assert(sizeof(struct mt6628_wifi_cmd_hdr) == MT6628_WIFI_CMD_HEADER_LEN);
static_assert(sizeof(struct mt6628_wifi_event_hdr) == MT6628_WIFI_EVENT_HEADER_LEN);

struct mt6628_hif_rx_hdr {
	__le16 packet_len;
	__le16 packet_type;
	u8 header_len_offset;
	u8 reorder_pal_tcl;
	__le16 seq_no_tid;
	u8 sta_rec_idx;
	u8 rcpi;
	u8 hw_channel_num;
	u8 reserved;
} __packed;

static_assert(sizeof(struct mt6628_hif_rx_hdr) == MT6628_HIF_RX_HEADER_LEN);

/* WCIR */
#define MT6628_WCIR_CHIP_ID		GENMASK(15, 0)
#define MT6628_WCIR_WLAN_READY		BIT(21)

/* WHCR */
#define MT6628_WHCR_W_INT_CLR_CTRL	BIT(1)

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
