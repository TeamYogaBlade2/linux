// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6628 Bluetooth over shared STP transport
 */

#include <linux/mfd/mt6628.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci_sync.h>

struct mt6628_bt {
	struct mt6628_wmt *wmt;
	struct hci_dev *hdev;
};

static int mt6628_bt_vendor_cmd(struct hci_dev *hdev, u16 opcode,
				const void *param, u32 plen)
{
	int ret;

	ret = __hci_cmd_sync_status(hdev, opcode, plen, param, 1000);
	if (ret < 0)
		return ret;
	if (ret) {
		bt_dev_err(hdev, "vendor opcode %#.4x failed, status %#x",
			   opcode, ret);
		return -EIO;
	}

	return 0;
}

static int mt6628_bt_setup(struct hci_dev *hdev)
{
	static const u8 link_key_type[] = { 0x01 };
	static const u8 unit_key[] = {
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
	};
	static const u8 encryption[] = { 0x00, 0x02, 0x10 };
	static const u8 pin_code_type[] = { 0x00 };
	static const u8 voice[] = { 0x60, 0x00 };
	static const u8 pcm[] = { 0x63, 0x10, 0x00, 0x00 };
	static const u8 radio[] = {
		0x07, 0x80, 0x00, 0x06, 0x05, 0x07,
	};
	static const u8 tx_power_offset[] = {
		0xff, 0xff, 0xff,
	};
	static const u8 sleep_timeout[] = {
		0x03, 0x40, 0x1f, 0x40, 0x1f, 0x00, 0x04,
	};
	static const u8 bt_ftr[] = { 0x80, 0x00 };
	static const u8 osc_info[] = {
		0x01, 0x01, 0x14, 0x0a, 0x08,
	};
	static const u8 lpo_info[] = {
		0x01, 0xfa, 0x0a, 0x02, 0x00,
		0xa6, 0x0e, 0x00, 0x40, 0x00,
	};
	static const u8 pta[] = {
		0xc9, 0x8b, 0xbf, 0x00, 0x00,
		0x52, 0x0e, 0x0e, 0x1f, 0x1b,
	};
	static const u8 ble_pta[] = {
		0x16, 0x0e, 0x0e, 0x00, 0x07,
	};
	int ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc1b,
				   link_key_type, sizeof(link_key_type));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc75,
				   unit_key, sizeof(unit_key));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc76,
				   encryption, sizeof(encryption));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0x0c0a,
				   pin_code_type, sizeof(pin_code_type));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0x0c26,
				   voice, sizeof(voice));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc72,
				   pcm, sizeof(pcm));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc79,
				   radio, sizeof(radio));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc93,
				   tx_power_offset, sizeof(tx_power_offset));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc7a,
				   sleep_timeout, sizeof(sleep_timeout));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc7d,
				   bt_ftr, sizeof(bt_ftr));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc7b,
				   osc_info, sizeof(osc_info));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfc7c,
				   lpo_info, sizeof(lpo_info));
	if (ret)
		return ret;

	return mt6628_bt_vendor_cmd(hdev, 0xfc74,
				    pta, sizeof(pta));
}

static int mt6628_bt_post_init(struct hci_dev *hdev)
{
	static const u8 ble_pta[] = {
		0x16, 0x0e, 0x0e, 0x00, 0x07,
	};
	static const u8 internal_pta_1[] = {
		0x00, 0x01, 0x0f, 0x0f, 0x01,
		0x0f, 0x0f, 0x01, 0x0f, 0x0f,
		0x01, 0x0f, 0x0f, 0x02, 0x01,
	};
	static const u8 internal_pta_2[] = {
		0x01, 0x19, 0x19, 0x07, 0xd0, 0x00, 0x01,
	};
	static const u8 rf_reg_100[] = {
		0x64, 0x01, 0x02, 0x00, 0x00, 0x00,
	};
	int ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfcfc,
				   ble_pta, sizeof(ble_pta));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfcfb,
				   internal_pta_1, sizeof(internal_pta_1));
	if (ret)
		return ret;

	ret = mt6628_bt_vendor_cmd(hdev, 0xfcfb,
				   internal_pta_2, sizeof(internal_pta_2));
	if (ret)
		return ret;

	return mt6628_bt_vendor_cmd(hdev, 0xfcb0,
				    rf_reg_100, sizeof(rf_reg_100));
}

static int mt6628_bt_open(struct hci_dev *hdev)
{
	struct mt6628_bt *bt = hci_get_drvdata(hdev);

	return mt6628_wmt_func_ctrl(bt->wmt, MT6628_WMT_FUNC_BT, true);
}

static int mt6628_bt_close(struct hci_dev *hdev)
{
	struct mt6628_bt *bt = hci_get_drvdata(hdev);

	return mt6628_wmt_func_ctrl(bt->wmt, MT6628_WMT_FUNC_BT, false);
}

static int mt6628_bt_flush(struct hci_dev *hdev)
{
	return 0;
}

static int mt6628_bt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct mt6628_bt *bt = hci_get_drvdata(hdev);
	u8 *buf;
	int ret;

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	case HCI_ISODATA_PKT:
		hdev->stat.iso_tx++;
		break;
	default:
		return -EILSEQ;
	}

	buf = kmalloc(skb->len + 1, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	buf[0] = hci_skb_pkt_type(skb);
	memcpy(buf + 1, skb->data, skb->len);

	ret = mt6628_stp_send(bt->wmt, MT6628_STP_TASK_BT,
			      buf, skb->len + 1);
	kfree(buf);
	if (ret < 0) {
		hdev->stat.err_tx++;
		return ret;
	}

	hdev->stat.byte_tx += skb->len;
	kfree_skb(skb);
	return 0;
}

static void mt6628_bt_rx(void *priv, const u8 *buf, size_t len)
{
	struct mt6628_bt *bt = priv;
	struct sk_buff *skb;
	u8 type;

	if (len < 1)
		return;

	type = buf[0];
	switch (type) {
	case HCI_EVENT_PKT:
	case HCI_ACLDATA_PKT:
	case HCI_SCODATA_PKT:
	case HCI_ISODATA_PKT:
		break;
	default:
		bt->hdev->stat.err_rx++;
		return;
	}

	skb = bt_skb_alloc(len - 1, GFP_KERNEL);
	if (!skb) {
		bt->hdev->stat.err_rx++;
		return;
	}

	skb_put_data(skb, buf + 1, len - 1);
	hci_skb_pkt_type(skb) = type;
	bt->hdev->stat.byte_rx += len - 1;

	if (hci_recv_frame(bt->hdev, skb) < 0)
		bt->hdev->stat.err_rx++;
}

static int mt6628_bt_probe(struct platform_device *pdev)
{
	struct mt6628_bt *bt;
	struct hci_dev *hdev;
	int ret;

	bt = devm_kzalloc(&pdev->dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;

	bt->wmt = dev_get_drvdata(pdev->dev.parent);
	if (!bt->wmt)
		return -EPROBE_DEFER;

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	bt->hdev = hdev;
	hdev->bus = HCI_SDIO;
	hdev->open = mt6628_bt_open;
	hdev->close = mt6628_bt_close;
	hdev->flush = mt6628_bt_flush;
	hdev->send = mt6628_bt_send_frame;
	hdev->setup = mt6628_bt_setup;
	hdev->post_init = mt6628_bt_post_init;
	SET_HCIDEV_DEV(hdev, &pdev->dev);
	hci_set_drvdata(hdev, bt);

	ret = mt6628_stp_register_rx(bt->wmt, MT6628_STP_TASK_BT,
				     mt6628_bt_rx, bt);
	if (ret)
		goto err_free_hdev;

	ret = hci_register_dev(hdev);
	if (ret)
		goto err_unregister_rx;

	platform_set_drvdata(pdev, bt);
	return 0;

err_unregister_rx:
	mt6628_stp_unregister_rx(bt->wmt, MT6628_STP_TASK_BT,
				 mt6628_bt_rx, bt);
err_free_hdev:
	hci_free_dev(hdev);
	return ret;
}

static void mt6628_bt_remove(struct platform_device *pdev)
{
	struct mt6628_bt *bt = platform_get_drvdata(pdev);

	if (!bt)
		return;

	mt6628_stp_unregister_rx(bt->wmt, MT6628_STP_TASK_BT,
				 mt6628_bt_rx, bt);
	hci_unregister_dev(bt->hdev);
	hci_free_dev(bt->hdev);
}

static struct platform_driver mt6628_bt_driver = {
	.probe = mt6628_bt_probe,
	.remove = mt6628_bt_remove,
	.driver = {
		.name = "mt6628-bt",
	},
};
module_platform_driver(mt6628_bt_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 Bluetooth over STP");
MODULE_LICENSE("GPL");
