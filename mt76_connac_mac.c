// SPDX-License-Identifier: ISC
/* Copyright (C) 2020 MediaTek Inc. */

#include "mt76_connac_mac.h"
#include "mt76_connac.h"

int mt76_connac_pm_wake(struct mt76_phy *phy, struct mt76_connac_pm *pm)
{
	struct mt76_dev *dev = phy->dev;

	if (mt76_is_usb(dev))
		return 0;

	cancel_delayed_work_sync(&pm->ps_work);
	if (!test_bit(MT76_STATE_PM, &phy->state))
		return 0;

	if (pm->suspended)
		return 0;

	queue_work(dev->wq, &pm->wake_work);
	if (!wait_event_timeout(pm->wait,
				!test_bit(MT76_STATE_PM, &phy->state),
				3 * HZ)) {
		ieee80211_wake_queues(phy->hw);
		return -ETIMEDOUT;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt76_connac_pm_wake);

void mt76_connac_power_save_sched(struct mt76_phy *phy,
				  struct mt76_connac_pm *pm)
{
	struct mt76_dev *dev = phy->dev;

	if (mt76_is_usb(dev))
		return;

	if (!pm->enable)
		return;

	if (pm->suspended)
		return;

	pm->last_activity = jiffies;

	if (!test_bit(MT76_STATE_PM, &phy->state)) {
		cancel_delayed_work(&phy->mac_work);
		queue_delayed_work(dev->wq, &pm->ps_work, pm->idle_timeout);
	}
}
EXPORT_SYMBOL_GPL(mt76_connac_power_save_sched);

void mt76_connac_free_pending_tx_skbs(struct mt76_connac_pm *pm,
				      struct mt76_wcid *wcid)
{
	int i;

	spin_lock_bh(&pm->txq_lock);
	for (i = 0; i < IEEE80211_NUM_ACS; i++) {
		if (wcid && pm->tx_q[i].wcid != wcid)
			continue;

		dev_kfree_skb(pm->tx_q[i].skb);
		pm->tx_q[i].skb = NULL;
	}
	spin_unlock_bh(&pm->txq_lock);
}
EXPORT_SYMBOL_GPL(mt76_connac_free_pending_tx_skbs);

void mt76_connac_pm_queue_skb(struct ieee80211_hw *hw,
			      struct mt76_connac_pm *pm,
			      struct mt76_wcid *wcid,
			      struct sk_buff *skb)
{
	int qid = skb_get_queue_mapping(skb);
	struct mt76_phy *phy = hw->priv;

	spin_lock_bh(&pm->txq_lock);
	if (!pm->tx_q[qid].skb) {
		ieee80211_stop_queues(hw);
		pm->tx_q[qid].wcid = wcid;
		pm->tx_q[qid].skb = skb;
		queue_work(phy->dev->wq, &pm->wake_work);
	} else {
		dev_kfree_skb(skb);
	}
	spin_unlock_bh(&pm->txq_lock);
}
EXPORT_SYMBOL_GPL(mt76_connac_pm_queue_skb);

void mt76_connac_pm_dequeue_skbs(struct mt76_phy *phy,
				 struct mt76_connac_pm *pm)
{
	int i;

	spin_lock_bh(&pm->txq_lock);
	for (i = 0; i < IEEE80211_NUM_ACS; i++) {
		struct mt76_wcid *wcid = pm->tx_q[i].wcid;
		struct ieee80211_sta *sta = NULL;

		if (!pm->tx_q[i].skb)
			continue;

		if (wcid && wcid->sta)
			sta = container_of((void *)wcid, struct ieee80211_sta,
					   drv_priv);

		mt76_tx(phy, sta, wcid, pm->tx_q[i].skb);
		pm->tx_q[i].skb = NULL;
	}
	spin_unlock_bh(&pm->txq_lock);

	mt76_worker_schedule(&phy->dev->tx_worker);
}
EXPORT_SYMBOL_GPL(mt76_connac_pm_dequeue_skbs);

/* The HW does not translate the mac header to 802.3 for mesh point */
int mt76_connac_reverse_frag0_hdr_trans(struct sk_buff *skb, u16 hdr_offset,
					struct ieee80211_sta *sta,
					struct ieee80211_vif *vif)
{
	struct ethhdr *eth_hdr = (struct ethhdr *)(skb->data + hdr_offset);
	__le32 *rxd = (__le32 *)skb->data;
	struct ieee80211_hdr hdr;
	__le32 qos_ctrl, ht_ctrl;

	if (FIELD_GET(MT_RXD3_NORMAL_ADDR_TYPE, le32_to_cpu(rxd[3])) !=
	    MT_RXD3_NORMAL_U2M)
		return -EINVAL;

	if (!(le32_to_cpu(rxd[1]) & MT_RXD1_NORMAL_GROUP_4))
		return -EINVAL;

	/* store the info from RXD and ethhdr to avoid being overridden */
	hdr.frame_control = FIELD_GET(MT_RXD6_FRAME_CONTROL, rxd[6]);
	hdr.seq_ctrl = FIELD_GET(MT_RXD8_SEQ_CTRL, rxd[8]);
	qos_ctrl = FIELD_GET(MT_RXD8_QOS_CTL, rxd[8]);
	ht_ctrl = FIELD_GET(MT_RXD9_HT_CONTROL, rxd[9]);

	hdr.duration_id = 0;
	ether_addr_copy(hdr.addr1, vif->addr);
	ether_addr_copy(hdr.addr2, sta->addr);
	switch (le16_to_cpu(hdr.frame_control) &
		(IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) {
	case 0:
		ether_addr_copy(hdr.addr3, vif->bss_conf.bssid);
		break;
	case IEEE80211_FCTL_FROMDS:
		ether_addr_copy(hdr.addr3, eth_hdr->h_source);
		break;
	case IEEE80211_FCTL_TODS:
		ether_addr_copy(hdr.addr3, eth_hdr->h_dest);
		break;
	case IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS:
		ether_addr_copy(hdr.addr3, eth_hdr->h_dest);
		ether_addr_copy(hdr.addr4, eth_hdr->h_source);
		break;
	default:
		break;
	}

	skb_pull(skb, hdr_offset + sizeof(struct ethhdr) - 2);
	if (eth_hdr->h_proto == cpu_to_be16(ETH_P_AARP) ||
	    eth_hdr->h_proto == cpu_to_be16(ETH_P_IPX))
		ether_addr_copy(skb_push(skb, ETH_ALEN), bridge_tunnel_header);
	else if (eth_hdr->h_proto >= cpu_to_be16(ETH_P_802_3_MIN))
		ether_addr_copy(skb_push(skb, ETH_ALEN), rfc1042_header);
	else
		skb_pull(skb, 2);

	if (ieee80211_has_order(hdr.frame_control))
		memcpy(skb_push(skb, 2), &ht_ctrl, 2);
	if (ieee80211_is_data_qos(hdr.frame_control))
		memcpy(skb_push(skb, 2), &qos_ctrl, 2);
	if (ieee80211_has_a4(hdr.frame_control))
		memcpy(skb_push(skb, sizeof(hdr)), &hdr, sizeof(hdr));
	else
		memcpy(skb_push(skb, sizeof(hdr) - 6), &hdr, sizeof(hdr) - 6);

	return 0;
}
EXPORT_SYMBOL_GPL(mt76_connac_reverse_frag0_hdr_trans);

static void
mt76_connac_mac_decode_he_radiotap_ru(struct mt76_rx_status *status,
				      struct ieee80211_radiotap_he *he,
				      __le32 *rxv)
{
	u32 ru_h, ru_l;
	u8 ru, offs = 0;

	ru_l = FIELD_GET(MT_PRXV_HE_RU_ALLOC_L, le32_to_cpu(rxv[0]));
	ru_h = FIELD_GET(MT_PRXV_HE_RU_ALLOC_H, le32_to_cpu(rxv[1]));
	ru = (u8)(ru_l | ru_h << 4);

	status->bw = RATE_INFO_BW_HE_RU;

	switch (ru) {
	case 0 ... 36:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_26;
		offs = ru;
		break;
	case 37 ... 52:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_52;
		offs = ru - 37;
		break;
	case 53 ... 60:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_106;
		offs = ru - 53;
		break;
	case 61 ... 64:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_242;
		offs = ru - 61;
		break;
	case 65 ... 66:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_484;
		offs = ru - 65;
		break;
	case 67:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_996;
		break;
	case 68:
		status->he_ru = NL80211_RATE_INFO_HE_RU_ALLOC_2x996;
		break;
	}

	he->data1 |= HE_BITS(DATA1_BW_RU_ALLOC_KNOWN);
	he->data2 |= HE_BITS(DATA2_RU_OFFSET_KNOWN) |
		     le16_encode_bits(offs,
				      IEEE80211_RADIOTAP_HE_DATA2_RU_OFFSET);
}

#define MU_PREP(f, v)	le16_encode_bits(v, IEEE80211_RADIOTAP_HE_MU_##f)
static void
mt76_connac_mac_decode_he_mu_radiotap(struct sk_buff *skb, __le32 *rxv)
{
	struct mt76_rx_status *status = (struct mt76_rx_status *)skb->cb;
	static const struct ieee80211_radiotap_he_mu mu_known = {
		.flags1 = HE_BITS(MU_FLAGS1_SIG_B_MCS_KNOWN) |
			  HE_BITS(MU_FLAGS1_SIG_B_DCM_KNOWN) |
			  HE_BITS(MU_FLAGS1_CH1_RU_KNOWN) |
			  HE_BITS(MU_FLAGS1_SIG_B_SYMS_USERS_KNOWN) |
			  HE_BITS(MU_FLAGS1_SIG_B_COMP_KNOWN),
		.flags2 = HE_BITS(MU_FLAGS2_BW_FROM_SIG_A_BW_KNOWN) |
			  HE_BITS(MU_FLAGS2_PUNC_FROM_SIG_A_BW_KNOWN),
	};
	struct ieee80211_radiotap_he_mu *he_mu;

	status->flag |= RX_FLAG_RADIOTAP_HE_MU;

	he_mu = skb_push(skb, sizeof(mu_known));
	memcpy(he_mu, &mu_known, sizeof(mu_known));

	he_mu->flags1 |= MU_PREP(FLAGS1_SIG_B_MCS, status->rate_idx);
	if (status->he_dcm)
		he_mu->flags1 |= MU_PREP(FLAGS1_SIG_B_DCM, status->he_dcm);

	he_mu->flags2 |= MU_PREP(FLAGS2_BW_FROM_SIG_A_BW, status->bw) |
			 MU_PREP(FLAGS2_SIG_B_SYMS_USERS,
				 le32_get_bits(rxv[2], MT_CRXV_HE_NUM_USER));

	he_mu->ru_ch1[0] = le32_get_bits(rxv[3], MT_CRXV_HE_RU0);

	if (status->bw >= RATE_INFO_BW_40) {
		he_mu->flags1 |= HE_BITS(MU_FLAGS1_CH2_RU_KNOWN);
		he_mu->ru_ch2[0] = le32_get_bits(rxv[3], MT_CRXV_HE_RU1);
	}

	if (status->bw >= RATE_INFO_BW_80) {
		he_mu->ru_ch1[1] = le32_get_bits(rxv[3], MT_CRXV_HE_RU2);
		he_mu->ru_ch2[1] = le32_get_bits(rxv[3], MT_CRXV_HE_RU3);
	}
}

void mt76_connac_mac_decode_he_radiotap(struct sk_buff *skb,
					__le32 *rxv, u32 mode)
{
	struct mt76_rx_status *status = (struct mt76_rx_status *)skb->cb;
	static const struct ieee80211_radiotap_he known = {
		.data1 = HE_BITS(DATA1_DATA_MCS_KNOWN) |
			 HE_BITS(DATA1_DATA_DCM_KNOWN) |
			 HE_BITS(DATA1_STBC_KNOWN) |
			 HE_BITS(DATA1_CODING_KNOWN) |
			 HE_BITS(DATA1_LDPC_XSYMSEG_KNOWN) |
			 HE_BITS(DATA1_DOPPLER_KNOWN) |
			 HE_BITS(DATA1_SPTL_REUSE_KNOWN) |
			 HE_BITS(DATA1_BSS_COLOR_KNOWN),
		.data2 = HE_BITS(DATA2_GI_KNOWN) |
			 HE_BITS(DATA2_TXBF_KNOWN) |
			 HE_BITS(DATA2_PE_DISAMBIG_KNOWN) |
			 HE_BITS(DATA2_TXOP_KNOWN),
	};
	u32 ltf_size = le32_get_bits(rxv[2], MT_CRXV_HE_LTF_SIZE) + 1;
	struct ieee80211_radiotap_he *he;

	status->flag |= RX_FLAG_RADIOTAP_HE;

	he = skb_push(skb, sizeof(known));
	memcpy(he, &known, sizeof(known));

	he->data3 = HE_PREP(DATA3_BSS_COLOR, BSS_COLOR, rxv[14]) |
		    HE_PREP(DATA3_LDPC_XSYMSEG, LDPC_EXT_SYM, rxv[2]);
	he->data4 = HE_PREP(DATA4_SU_MU_SPTL_REUSE, SR_MASK, rxv[11]);
	he->data5 = HE_PREP(DATA5_PE_DISAMBIG, PE_DISAMBIG, rxv[2]) |
		    le16_encode_bits(ltf_size,
				     IEEE80211_RADIOTAP_HE_DATA5_LTF_SIZE);
	if (le32_to_cpu(rxv[0]) & MT_PRXV_TXBF)
		he->data5 |= HE_BITS(DATA5_TXBF);
	he->data6 = HE_PREP(DATA6_TXOP, TXOP_DUR, rxv[14]) |
		    HE_PREP(DATA6_DOPPLER, DOPPLER, rxv[14]);

	switch (mode) {
	case MT_PHY_TYPE_HE_SU:
		he->data1 |= HE_BITS(DATA1_FORMAT_SU) |
			     HE_BITS(DATA1_UL_DL_KNOWN) |
			     HE_BITS(DATA1_BEAM_CHANGE_KNOWN);
		he->data3 |= HE_PREP(DATA3_BEAM_CHANGE, BEAM_CHNG, rxv[14]) |
			     HE_PREP(DATA3_UL_DL, UPLINK, rxv[2]);
		break;
	case MT_PHY_TYPE_HE_EXT_SU:
		he->data1 |= HE_BITS(DATA1_FORMAT_EXT_SU) |
			     HE_BITS(DATA1_UL_DL_KNOWN);
		he->data3 |= HE_PREP(DATA3_UL_DL, UPLINK, rxv[2]);
		break;
	case MT_PHY_TYPE_HE_MU:
		he->data1 |= HE_BITS(DATA1_FORMAT_MU) |
			     HE_BITS(DATA1_UL_DL_KNOWN);
		he->data3 |= HE_PREP(DATA3_UL_DL, UPLINK, rxv[2]);
		he->data4 |= HE_PREP(DATA4_MU_STA_ID, MU_AID, rxv[7]);

		mt76_connac_mac_decode_he_radiotap_ru(status, he, rxv);
		mt76_connac_mac_decode_he_mu_radiotap(skb, rxv);
		break;
	case MT_PHY_TYPE_HE_TB:
		he->data1 |= HE_BITS(DATA1_FORMAT_TRIG) |
			     HE_BITS(DATA1_SPTL_REUSE2_KNOWN) |
			     HE_BITS(DATA1_SPTL_REUSE3_KNOWN) |
			     HE_BITS(DATA1_SPTL_REUSE4_KNOWN);
		he->data4 |= HE_PREP(DATA4_TB_SPTL_REUSE1, SR_MASK, rxv[11]) |
			     HE_PREP(DATA4_TB_SPTL_REUSE2, SR1_MASK, rxv[11]) |
			     HE_PREP(DATA4_TB_SPTL_REUSE3, SR2_MASK, rxv[11]) |
			     HE_PREP(DATA4_TB_SPTL_REUSE4, SR3_MASK, rxv[11]);

		mt76_connac_mac_decode_he_radiotap_ru(status, he, rxv);
		break;
	default:
		break;
	}
}
EXPORT_SYMBOL_GPL(mt76_connac_mac_decode_he_radiotap);
