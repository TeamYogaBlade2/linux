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

struct mt6628_bt {
	struct mt6628_wmt *wmt;
	struct hci_dev *hdev;
};

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
	.remove_new = mt6628_bt_remove,
	.driver = {
		.name = "mt6628-bt",
	},
};
module_platform_driver(mt6628_bt_driver);

MODULE_AUTHOR("Akari Tsuyukusa <akkun11.open@gmail.com>");
MODULE_DESCRIPTION("MediaTek MT6628 Bluetooth over STP");
MODULE_LICENSE("GPL");
