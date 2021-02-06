// SPDX-License-Identifier: ISC
/* Copyright (C) 2021 Lorenzo Bianconi <lorenzo@kernel.org>  */

#include "mt7921.h"
#include "mcu.h"

enum {
	TM_CHANGED_TX_COUNT,
	TM_CHANGED_TX_LENGTH,
	TM_CHANGED_TXPOWER,
	TM_CHANGED_TX_RATE_SGI,
	TM_CHANGED_FREQ_OFFSET,

	/* must be last */
	NUM_TM_CHANGED
};

static const u8 tm_change_map[] = {
	[TM_CHANGED_TX_COUNT] = MT76_TM_ATTR_TX_COUNT,
	[TM_CHANGED_TX_LENGTH] = MT76_TM_ATTR_TX_LENGTH,
	[TM_CHANGED_TXPOWER] = MT76_TM_ATTR_TX_POWER,
	[TM_CHANGED_TX_RATE_SGI] = MT76_TM_ATTR_TX_RATE_SGI,
	[TM_CHANGED_FREQ_OFFSET] = MT76_TM_ATTR_FREQ_OFFSET,
};

static int
mt7921_tm_set_mode(struct mt7921_dev *dev, bool enable)
{
	struct mt76_testmode_data *td = &dev->mphy.test;
	struct mt7921_test_ctrl req = {
		.data0 = cpu_to_le32(enable), /* 0: disable, 1: enable */
	};
	int err;

	td->done = false;
	err = mt76_mcu_send_msg(&dev->mt76, MCU_CMD_TEST_CTRL, &req,
				sizeof(req), false);
	if (err < 0)
		return err;

	if (!wait_event_timeout(td->wait, td->done, HZ))
		return -ETIMEDOUT;

	return 0;
}

static int
mt7921_tm_set_param(struct mt7921_dev *dev, int index, int data)
{
	struct mt7921_test_ctrl req = {
		.action = 1,
		.data0 = cpu_to_le32(index),
		.data1 = cpu_to_le32(data),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CMD_TEST_CTRL, &req,
				 sizeof(req), false);
}

static int
mt7921_tm_set_tx_cont(struct mt7921_dev *dev, int tx_cont)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_PKTCNT, tx_cont);
}

static int
mt7921_tm_set_tx_len(struct mt7921_dev *dev, int tx_len)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_PKTLEN, tx_len);
}

static int
mt7921_tm_set_freq_offset(struct mt7921_dev *dev, int offset)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_FREQ_OFFSET, offset);
}

static int
mt7921_tm_set_channel(struct mt7921_dev *dev,
		      struct cfg80211_chan_def *chandef)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_CHAN,
				   chandef->center_freq1 * 1000);
}

static int
mt7921_tm_set_txgi(struct mt7921_dev *dev, int txgi)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_TXGI, txgi);
}

/* XXX rate encoding */
int mt7921_tm_set_rate(struct mt7921_dev *dev, int rate)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_RATE, rate);
}

static int
mt7921_tm_set_txpower0(struct mt7921_dev *dev, int power)
{
	if (power > 63)
		power += 128;

	return mt7921_tm_set_param(dev, MCU_TESTMODE_POWER, power);
}

static int
mt7921_tm_set_bw(struct mt7921_dev *dev, enum nl80211_chan_width width)
{
	int bw;

	switch (width) {
	case NL80211_CHAN_WIDTH_40:
		bw = CMD_CBW_40MHZ;
		break;
	case NL80211_CHAN_WIDTH_80:
		bw = CMD_CBW_80MHZ;
		break;
	case NL80211_CHAN_WIDTH_160:
		bw = CMD_CBW_160MHZ;
		break;
	case NL80211_CHAN_WIDTH_20:
	default:
		bw = CMD_CBW_20MHZ;
		break;
	}

	return mt7921_tm_set_param(dev, MCU_TESTMODE_BW, bw);
}

static int
mt7921_tm_reset_counters(struct mt7921_dev *dev)
{
	return mt7921_tm_set_param(dev, MCU_TESTMODE_RESET_COUNTERS, 0);
}

static int
mt7921_tm_get_rx_stats(struct mt7921_dev *dev, struct sk_buff *msg)
{
	/* XXX: need to parse the reply */
	struct {
		__le32 seq_num;
		__le32 total_num;
	} req = {
		.seq_num = cpu_to_le32(dev->mt76.mcu.msg_seq),
		.total_num = cpu_to_le32(72),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_CMD_GET_RX_STATS, &req,
				 sizeof(req), true);
}

static int
mt7921_tm_set_txrx_enable(struct mt7921_dev *dev, bool tx, bool enable)
{
	int cmd;

	if (enable)
		cmd = tx ? MCU_TESTMODE_START_TX : MCU_TESTMODE_START_RX;
	else
		cmd = MCU_TESTMODE_STOP;

	return mt7921_tm_set_param(dev, MCU_TESTMODE_CMD, cmd);
}

static int
mt7921_tm_update_params(struct mt7921_phy *phy, int changed)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt7921_dev *dev = phy->dev;
	int err;

	if (changed & BIT(TM_CHANGED_TX_COUNT)) {
		err = mt7921_tm_set_tx_cont(dev, td->tx_count);
		if (err < 0)
			return err;
	}

	if (changed & BIT(TM_CHANGED_TX_LENGTH)) {
		err = mt7921_tm_set_tx_len(dev, td->tx_msdu_len);
		if (err < 0)
			return err;
	}

	if (changed & BIT(TM_CHANGED_TXPOWER)) {
		err = mt7921_tm_set_txpower0(dev, td->tx_power[0]);
		if (err < 0)
			return err;
	}

	if (changed & BIT(TM_CHANGED_TX_RATE_SGI)) {
		err = mt7921_tm_set_txgi(dev, td->tx_rate_sgi);
		if (err < 0)
			return err;
	}

	if (changed & BIT(TM_CHANGED_FREQ_OFFSET)) {
		err = mt7921_tm_set_freq_offset(dev, td->freq_offset);
		if (err < 0)
			return err;
	}

	return 0;
}

static int
mt7921_tm_set_params(struct mt76_phy *mphy, struct nlattr **tb,
		     enum mt76_testmode_state new_state)
{
	struct mt7921_phy *phy = mphy->priv;
	int i, changed = 0;

	BUILD_BUG_ON(NUM_TM_CHANGED >= 32);

	for (i = 0; i < ARRAY_SIZE(tm_change_map); i++) {
		if (tb[tm_change_map[i]])
			changed |= BIT(i);
	}

	return mt7921_tm_update_params(phy, changed);
}

static int
mt7921_tm_set_state(struct mt76_phy *mphy, enum mt76_testmode_state state)
{
	struct mt76_testmode_data *td = &mphy->test;
	struct mt7921_phy *phy = mphy->priv;
	enum mt76_testmode_state prev_state = td->state;
	bool tx = state == MT76_TM_STATE_TX_FRAMES;
	int err;

	mphy->test.state = state;

	if (prev_state == MT76_TM_STATE_TX_FRAMES ||
	    prev_state == MT76_TM_STATE_RX_FRAMES) {
		err = mt7921_tm_set_txrx_enable(phy->dev, true, false);
		if (err < 0)
			return err;
	}

	if (state == MT76_TM_STATE_IDLE &&
	    prev_state == MT76_TM_STATE_OFF) {
		struct ieee80211_hw *hw = mphy->hw;
		struct cfg80211_chan_def *chandef = &hw->conf.chandef;

		mt76_worker_disable(&mphy->dev->tx_worker);

		err = mt7921_tm_set_mode(phy->dev, true);
		if (err < 0)
			return err;

		err = mt7921_tm_reset_counters(phy->dev);
		if (err < 0)
			return err;

		err = mt7921_tm_set_channel(phy->dev, chandef);
		if (err < 0)
			return err;

		err = mt7921_tm_set_bw(phy->dev, chandef->width);
		if (err < 0)
			return err;
	}

	if (state == MT76_TM_STATE_OFF) {
		err = mt7921_tm_set_mode(phy->dev, false);
		if (err < 0)
			return err;

		mt76_worker_enable(&mphy->dev->tx_worker);
		return 0;
	}

	return mt7921_tm_set_txrx_enable(phy->dev, tx, true);
}

static int
mt7921_tm_dump_stats(struct mt76_phy *mphy, struct sk_buff *msg)
{
	struct mt7921_phy *phy = mphy->priv;

	return mt7921_tm_get_rx_stats(phy->dev, msg);
}

const struct mt76_testmode_ops mt7921_testmode_ops = {
	.set_state = mt7921_tm_set_state,
	.set_params = mt7921_tm_set_params,
	.dump_stats = mt7921_tm_dump_stats,
};
