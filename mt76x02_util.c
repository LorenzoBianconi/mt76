/*
 * Copyright (C) 2018 Stanislaw Gruszka <stf_xl@wp.pl>
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
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

#include "mt76.h"
#include "mt76x02_regs.h"
#include "mt76x02_mac.h"

void mt76x02_configure_filter(struct ieee80211_hw *hw,
			     unsigned int changed_flags,
			     unsigned int *total_flags, u64 multicast)
{
	struct mt76_dev *dev = hw->priv;
	u32 flags = 0;

#define MT76_FILTER(_flag, _hw) do { \
		flags |= *total_flags & FIF_##_flag;			\
		dev->rxfilter &= ~(_hw);				\
		dev->rxfilter |= !(flags & FIF_##_flag) * (_hw);	\
	} while (0)

	mutex_lock(&dev->mutex);

	dev->rxfilter &= ~MT_RX_FILTR_CFG_OTHER_BSS;

	MT76_FILTER(FCSFAIL, MT_RX_FILTR_CFG_CRC_ERR);
	MT76_FILTER(PLCPFAIL, MT_RX_FILTR_CFG_PHY_ERR);
	MT76_FILTER(CONTROL, MT_RX_FILTR_CFG_ACK |
			     MT_RX_FILTR_CFG_CTS |
			     MT_RX_FILTR_CFG_CFEND |
			     MT_RX_FILTR_CFG_CFACK |
			     MT_RX_FILTR_CFG_BA |
			     MT_RX_FILTR_CFG_CTRL_RSV);
	MT76_FILTER(PSPOLL, MT_RX_FILTR_CFG_PSPOLL);

	*total_flags = flags;
	dev->bus->wr(dev, MT_RX_FILTR_CFG, dev->rxfilter);

	mutex_unlock(&dev->mutex);
}
EXPORT_SYMBOL_GPL(mt76x02_configure_filter);

int mt76x02_sta_add(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		   struct ieee80211_sta *sta)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76x02_sta *msta = (struct mt76x02_sta *) sta->drv_priv;
	struct mt76x02_vif *mvif = (struct mt76x02_vif *) vif->drv_priv;
	int ret = 0;
	int idx = 0;
	int i;

	mutex_lock(&dev->mutex);

	idx = mt76_wcid_alloc(dev->wcid_mask, ARRAY_SIZE(dev->wcid));
	if (idx < 0) {
		ret = -ENOSPC;
		goto out;
	}

	msta->vif = mvif;
	msta->wcid.sta = 1;
	msta->wcid.idx = idx;
	msta->wcid.hw_key_idx = -1;
	mt76x02_mac_wcid_setup(dev, idx, mvif->idx, sta->addr);
	mt76x02_mac_wcid_set_drop(dev, idx, false);
	for (i = 0; i < ARRAY_SIZE(sta->txq); i++)
		mt76x02_txq_init(dev, sta->txq[i]);

	if (vif->type == NL80211_IFTYPE_AP)
		set_bit(MT_WCID_FLAG_CHECK_PS, &msta->wcid.flags);

	ewma_signal_init(&msta->rssi);

	rcu_assign_pointer(dev->wcid[idx], &msta->wcid);

out:
	mutex_unlock(&dev->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mt76x02_sta_add);

int mt76x02_sta_remove(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		      struct ieee80211_sta *sta)
{
	struct mt76_dev *dev = hw->priv;
	struct mt76x02_sta *msta = (struct mt76x02_sta *) sta->drv_priv;
	int idx = msta->wcid.idx;
	int i;

	mutex_lock(&dev->mutex);
	rcu_assign_pointer(dev->wcid[idx], NULL);
	for (i = 0; i < ARRAY_SIZE(sta->txq); i++)
		mt76_txq_remove(dev, sta->txq[i]);
	mt76x02_mac_wcid_set_drop(dev, idx, true);
	mt76_wcid_free(dev->wcid_mask, idx);
	mt76x02_mac_wcid_setup(dev, idx, 0, NULL);
	mutex_unlock(&dev->mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_sta_remove);

MODULE_LICENSE("Dual BSD/GPL");
