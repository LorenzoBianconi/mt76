/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2020 MediaTek Inc. */

#ifndef __MT7915_MAC_H
#define __MT7915_MAC_H

#include "../mt76_connac_mac.h"

#define MT_RXV_HDR_BAND_IDX		BIT(24)

#define MT_PRXV_DCM			BIT(17)
#define MT_PRXV_NUM_RX			BIT(20, 18)

/* VHT/HE only use bits 0-3 */
#define MT_TX_RATE_IDX			GENMASK(5, 0)

#define MT_TXP_MAX_BUF_NUM		6
struct mt7915_txp {
	__le16 flags;
	__le16 token;
	u8 bss_idx;
	__le16 rept_wds_wcid;
	u8 nbuf;
	__le32 buf[MT_TXP_MAX_BUF_NUM];
	__le16 len[MT_TXP_MAX_BUF_NUM];
} __packed __aligned(4);

struct mt7915_tx_free {
	__le16 rx_byte_cnt;
	__le16 ctrl;
	__le32 txd;
	__le32 info[];
} __packed __aligned(4);

#define MT_TX_FREE_VER			GENMASK(18, 16)
#define MT_TX_FREE_MSDU_CNT		GENMASK(9, 0)
#define MT_TX_FREE_WLAN_ID		GENMASK(23, 14)
#define MT_TX_FREE_LATENCY		GENMASK(12, 0)
/* 0: success, others: dropped */
#define MT_TX_FREE_MSDU_ID		GENMASK(30, 16)
#define MT_TX_FREE_PAIR			BIT(31)
#define MT_TX_FREE_MPDU_HEADER		BIT(30)
#define MT_TX_FREE_MSDU_ID_V3		GENMASK(14, 0)

/* will support this field in further revision */
#define MT_TX_FREE_RATE			GENMASK(13, 0)

#define MT_TXS0_FIXED_RATE		BIT(31)
#define MT_TXS0_BW			GENMASK(30, 29)
#define MT_TXS0_TID			GENMASK(28, 26)
#define MT_TXS0_AMPDU			BIT(25)
#define MT_TXS0_TXS_FORMAT		GENMASK(24, 23)
#define MT_TXS0_BA_ERROR		BIT(22)
#define MT_TXS0_PS_FLAG			BIT(21)
#define MT_TXS0_TXOP_TIMEOUT		BIT(20)
#define MT_TXS0_BIP_ERROR		BIT(19)

#define MT_TXS0_QUEUE_TIMEOUT		BIT(18)
#define MT_TXS0_RTS_TIMEOUT		BIT(17)
#define MT_TXS0_ACK_TIMEOUT		BIT(16)
#define MT_TXS0_ACK_ERROR_MASK		GENMASK(18, 16)

#define MT_TXS0_TX_STATUS_HOST		BIT(15)
#define MT_TXS0_TX_STATUS_MCU		BIT(14)
#define MT_TXS0_TX_RATE			GENMASK(13, 0)

#define MT_TXS1_SEQNO			GENMASK(31, 20)
#define MT_TXS1_RESP_RATE		GENMASK(19, 16)
#define MT_TXS1_RXV_SEQNO		GENMASK(15, 8)
#define MT_TXS1_TX_POWER_DBM		GENMASK(7, 0)

#define MT_TXS2_BF_STATUS		GENMASK(31, 30)
#define MT_TXS2_LAST_TX_RATE		GENMASK(29, 27)
#define MT_TXS2_SHARED_ANTENNA		BIT(26)
#define MT_TXS2_WCID			GENMASK(25, 16)
#define MT_TXS2_TX_DELAY		GENMASK(15, 0)

#define MT_TXS3_PID			GENMASK(31, 24)
#define MT_TXS3_ANT_ID			GENMASK(23, 0)

#define MT_TXS4_TIMESTAMP		GENMASK(31, 0)

#define MT_TXS5_F0_FINAL_MPDU		BIT(31)
#define MT_TXS5_F0_QOS			BIT(30)
#define MT_TXS5_F0_TX_COUNT		GENMASK(29, 25)
#define MT_TXS5_F0_FRONT_TIME		GENMASK(24, 0)
#define MT_TXS5_F1_MPDU_TX_COUNT	GENMASK(31, 24)
#define MT_TXS5_F1_MPDU_TX_BYTES	GENMASK(23, 0)

#define MT_TXS6_F0_NOISE_3		GENMASK(31, 24)
#define MT_TXS6_F0_NOISE_2		GENMASK(23, 16)
#define MT_TXS6_F0_NOISE_1		GENMASK(15, 8)
#define MT_TXS6_F0_NOISE_0		GENMASK(7, 0)
#define MT_TXS6_F1_MPDU_FAIL_COUNT	GENMASK(31, 24)
#define MT_TXS6_F1_MPDU_FAIL_BYTES	GENMASK(23, 0)

#define MT_TXS7_F0_RCPI_3		GENMASK(31, 24)
#define MT_TXS7_F0_RCPI_2		GENMASK(23, 16)
#define MT_TXS7_F0_RCPI_1		GENMASK(15, 8)
#define MT_TXS7_F0_RCPI_0		GENMASK(7, 0)
#define MT_TXS7_F1_MPDU_RETRY_COUNT	GENMASK(31, 24)
#define MT_TXS7_F1_MPDU_RETRY_BYTES	GENMASK(23, 0)

struct mt7915_dfs_pulse {
	u32 max_width;		/* us */
	int max_pwr;		/* dbm */
	int min_pwr;		/* dbm */
	u32 min_stgr_pri;	/* us */
	u32 max_stgr_pri;	/* us */
	u32 min_cr_pri;		/* us */
	u32 max_cr_pri;		/* us */
};

struct mt7915_dfs_pattern {
	u8 enb;
	u8 stgr;
	u8 min_crpn;
	u8 max_crpn;
	u8 min_crpr;
	u8 min_pw;
	u32 min_pri;
	u32 max_pri;
	u8 max_pw;
	u8 min_crbn;
	u8 max_crbn;
	u8 min_stgpn;
	u8 max_stgpn;
	u8 min_stgpr;
	u8 rsv[2];
	u32 min_stgpr_diff;
} __packed;

struct mt7915_dfs_radar_spec {
	struct mt7915_dfs_pulse pulse_th;
	struct mt7915_dfs_pattern radar_pattern[16];
};

static inline struct mt7915_txp *
mt7915_txwi_to_txp(struct mt76_dev *dev, struct mt76_txwi_cache *t)
{
	u8 *txwi;

	if (!t)
		return NULL;

	txwi = mt76_get_txwi_ptr(dev, t);

	return (struct mt7915_txp *)(txwi + MT_TXD_SIZE);
}

#endif
