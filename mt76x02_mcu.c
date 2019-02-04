/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 * Copyright (C) 2018 Lorenzo Bianconi <lorenzo.bianconi83@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/firmware.h>
#include <linux/delay.h>

#include "mt76x02_mcu.h"

static int
mt76x02_tx_queue_mcu(struct mt76x02_dev *dev, enum mt76_txq_id qid,
		     struct sk_buff *skb, int cmd, int seq)
{
	struct mt76_queue *q = &dev->mt76.q_tx[qid];
	struct mt76_queue_buf buf;
	dma_addr_t addr;
	u32 tx_info;

	tx_info = MT_MCU_MSG_TYPE_CMD |
		  FIELD_PREP(MT_MCU_MSG_CMD_TYPE, cmd) |
		  FIELD_PREP(MT_MCU_MSG_CMD_SEQ, seq) |
		  FIELD_PREP(MT_MCU_MSG_PORT, CPU_TX_PORT) |
		  FIELD_PREP(MT_MCU_MSG_LEN, skb->len);

	addr = dma_map_single(dev->mt76.dev, skb->data, skb->len,
			      DMA_TO_DEVICE);
	if (dma_mapping_error(dev->mt76.dev, addr))
		return -ENOMEM;

	buf.addr = addr;
	buf.len = skb->len;

	spin_lock_bh(&q->lock);
	mt76_queue_add_buf(dev, q, &buf, 1, tx_info, skb, NULL);
	mt76_queue_kick(dev, q);
	spin_unlock_bh(&q->lock);

	return 0;
}

int mt76x02_mcu_msg_send(struct mt76_dev *mdev, int cmd, const void *data,
			 int len, bool wait_resp)
{
	struct mt76x02_dev *dev = container_of(mdev, struct mt76x02_dev, mt76);
	unsigned long expires = jiffies + HZ;
	struct sk_buff *skb;
	int ret;
	u8 seq;

	skb = mt76x02_mcu_msg_alloc(data, len);
	if (!skb)
		return -ENOMEM;

	mutex_lock(&mdev->mmio.mcu.mutex);

	seq = ++mdev->mmio.mcu.msg_seq & 0xf;
	if (!seq)
		seq = ++mdev->mmio.mcu.msg_seq & 0xf;

	ret = mt76x02_tx_queue_mcu(dev, MT_TXQ_MCU, skb, cmd, seq);
	if (ret)
		goto out;

	while (wait_resp) {
		u32 *rxfce;
		bool check_seq = false;

		skb = mt76_mcu_get_response(&dev->mt76, expires);
		if (!skb) {
			dev_err(mdev->dev,
				"MCU message %d (seq %d) timed out\n", cmd,
				seq);
			ret = -ETIMEDOUT;
			break;
		}

		rxfce = (u32 *) skb->cb;

		if (seq == FIELD_GET(MT_RX_FCE_INFO_CMD_SEQ, *rxfce))
			check_seq = true;

		dev_kfree_skb(skb);
		if (check_seq)
			break;
	}

out:
	mutex_unlock(&mdev->mmio.mcu.mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mt76x02_mcu_msg_send);

int mt76x02_mcu_function_select(struct mt76x02_dev *dev, enum mcu_function func,
				u32 val)
{
	struct {
	    __le32 id;
	    __le32 value;
	} __packed __aligned(4) msg = {
	    .id = cpu_to_le32(func),
	    .value = cpu_to_le32(val),
	};
	bool wait = false;

	if (func != Q_SELECT)
		wait = true;

	return mt76_mcu_send_msg(dev, CMD_FUN_SET_OP, &msg, sizeof(msg), wait);
}
EXPORT_SYMBOL_GPL(mt76x02_mcu_function_select);

int mt76x02_mcu_set_radio_state(struct mt76x02_dev *dev, bool on)
{
	struct {
		__le32 mode;
		__le32 level;
	} __packed __aligned(4) msg = {
		.mode = cpu_to_le32(on ? RADIO_ON : RADIO_OFF),
		.level = cpu_to_le32(0),
	};

	return mt76_mcu_send_msg(dev, CMD_POWER_SAVING_OP, &msg, sizeof(msg), false);
}
EXPORT_SYMBOL_GPL(mt76x02_mcu_set_radio_state);

int mt76x02_mcu_calibrate(struct mt76x02_dev *dev, int type, u32 param)
{
	struct {
		__le32 id;
		__le32 value;
	} __packed __aligned(4) msg = {
		.id = cpu_to_le32(type),
		.value = cpu_to_le32(param),
	};
	bool is_mt76x2e = mt76_is_mmio(dev) && is_mt76x2(dev);
	int ret;

	if (is_mt76x2e)
		mt76_rmw(dev, MT_MCU_COM_REG0, BIT(31), 0);

	ret = mt76_mcu_send_msg(dev, CMD_CALIBRATION_OP, &msg, sizeof(msg),
				true);
	if (ret)
		return ret;

	if (is_mt76x2e &&
	    WARN_ON(!mt76_poll_msec(dev, MT_MCU_COM_REG0,
				    BIT(31), BIT(31), 100)))
		return -ETIMEDOUT;

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_mcu_calibrate);

int mt76x02_mcu_cleanup(struct mt76x02_dev *dev)
{
	struct sk_buff *skb;

	mt76_wr(dev, MT_MCU_INT_LEVEL, 1);
	usleep_range(20000, 30000);

	while ((skb = skb_dequeue(&dev->mt76.mmio.mcu.res_q)) != NULL)
		dev_kfree_skb(skb);

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_mcu_cleanup);

void mt76x02_set_ethtool_fwver(struct mt76x02_dev *dev,
			       const struct mt76x02_fw_header *h)
{
	u16 bld = le16_to_cpu(h->build_ver);
	u16 ver = le16_to_cpu(h->fw_ver);

	snprintf(dev->mt76.hw->wiphy->fw_version,
		 sizeof(dev->mt76.hw->wiphy->fw_version),
		 "%d.%d.%02d-b%x",
		 (ver >> 12) & 0xf, (ver >> 8) & 0xf, ver & 0xf, bld);
}
EXPORT_SYMBOL_GPL(mt76x02_set_ethtool_fwver);
