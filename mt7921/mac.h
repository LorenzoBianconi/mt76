/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2020 MediaTek Inc. */

#ifndef __MT7921_MAC_H
#define __MT7921_MAC_H

#include "../mt76_connac_mac.h"

#define MT_SDIO_TXD_SIZE		(MT_TXD_SIZE + 8 * 4)
#define MT_SDIO_TAIL_SIZE		8
#define MT_SDIO_HDR_SIZE		4

#define MT_TX_RATE_IDX			GENMASK(3, 0)

struct mt7921_tx_free {
	__le16 rx_byte_cnt;
	__le16 ctrl;
	u8 txd_cnt;
	u8 rsv[3];
	__le32 info[];
} __packed __aligned(4);

#define MT_TX_FREE_MSDU_CNT		GENMASK(9, 0)
#define MT_TX_FREE_WLAN_ID		GENMASK(23, 14)
#define MT_TX_FREE_LATENCY		GENMASK(12, 0)
/* 0: success, others: dropped */
#define MT_TX_FREE_STATUS		GENMASK(14, 13)
#define MT_TX_FREE_MSDU_ID		GENMASK(30, 16)
#define MT_TX_FREE_PAIR			BIT(31)
/* will support this field in further revision */
#define MT_TX_FREE_RATE			GENMASK(13, 0)

#define MT_TXS0_BW			GENMASK(30, 29)
#define MT_TXS0_TXS_FORMAT		GENMASK(24, 23)
#define MT_TXS0_ACK_ERROR_MASK		GENMASK(18, 16)
#define MT_TXS0_TX_RATE			GENMASK(13, 0)

#define MT_TXS2_WCID			GENMASK(25, 16)

#define MT_TXS3_PID			GENMASK(31, 24)

static inline struct mt7921_txp_common *
mt7921_txwi_to_txp(struct mt76_dev *dev, struct mt76_txwi_cache *t)
{
	u8 *txwi;

	if (!t)
		return NULL;

	txwi = mt76_get_txwi_ptr(dev, t);

	return (struct mt7921_txp_common *)(txwi + MT_TXD_SIZE);
}

#define MT_HW_TXP_MAX_MSDU_NUM		4
#define MT_HW_TXP_MAX_BUF_NUM		4

#define MT_MSDU_ID_VALID		BIT(15)

#define MT_TXD_LEN_MASK			GENMASK(11, 0)
#define MT_TXD_LEN_MSDU_LAST		BIT(14)
#define MT_TXD_LEN_AMSDU_LAST		BIT(15)
#define MT_TXD_LEN_LAST			BIT(15)

struct mt7921_txp_ptr {
	__le32 buf0;
	__le16 len0;
	__le16 len1;
	__le32 buf1;
} __packed __aligned(4);

struct mt7921_hw_txp {
	__le16 msdu_id[MT_HW_TXP_MAX_MSDU_NUM];
	struct mt7921_txp_ptr ptr[MT_HW_TXP_MAX_BUF_NUM / 2];
} __packed __aligned(4);

struct mt7921_txp_common {
	union {
		struct mt7921_hw_txp hw;
	};
};

#define MT_WTBL_TXRX_CAP_RATE_OFFSET	7
#define MT_WTBL_TXRX_RATE_G2_HE		24
#define MT_WTBL_TXRX_RATE_G2		12

#define MT_WTBL_AC0_CTT_OFFSET		20

static inline u32 mt7921_mac_wtbl_lmac_addr(int idx, u8 offset)
{
	return MT_WTBL_LMAC_OFFS(idx, 0) + offset * 4;
}

#endif
