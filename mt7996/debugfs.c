// SPDX-License-Identifier: BSD-3-Clause-Clear
/*
 * Copyright (C) 2022 MediaTek Inc.
 */

#include <linux/relay.h>
#include "mt7996.h"
#include "eeprom.h"
#include "mcu.h"
#include "mac.h"
#include "mtk_debug.h"

#define FW_BIN_LOG_MAGIC	0x44d9c99a

/** global debugfs **/

struct hw_queue_map {
	const char *name;
	u8 index;
	u8 pid;
	u8 qid;
};

static int
mt7996_implicit_txbf_set(void *data, u64 val)
{
	struct mt7996_dev *dev = data;

	/* The existing connected stations shall reconnect to apply
	 * new implicit txbf configuration.
	 */
	dev->ibf = !!val;

	return mt7996_mcu_set_txbf(dev, BF_HW_EN_UPDATE);
}

static int
mt7996_implicit_txbf_get(void *data, u64 *val)
{
	struct mt7996_dev *dev = data;

	*val = dev->ibf;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_implicit_txbf, mt7996_implicit_txbf_get,
			 mt7996_implicit_txbf_set, "%lld\n");

/* test knob of system error recovery */
static ssize_t
mt7996_sys_recovery_set(struct file *file, const char __user *user_buf,
			size_t count, loff_t *ppos)
{
	struct mt7996_dev *dev = file->private_data;
	char buf[16], *sep;
	int ret = 0;
	u16 band, val;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	sep = strchr(buf, ',');
	if (!sep)
		return -EINVAL;

	*sep = 0;
	if (kstrtou16(buf, 0, &band) || kstrtou16(sep + 1, 0, &val))
		return -EINVAL;

	switch (val) {
	/*
	 * <band>,0: grab firmware current SER state.
	 * <band>,1: trigger & enable system error L1 recovery.
	 * <band>,2: trigger & enable system error L2 recovery.
	 * <band>,3: trigger & enable system error L3 rx abort.
	 * <band>,4: trigger & enable system error L3 tx abort
	 * <band>,5: trigger & enable system error L3 tx disable.
	 * <band>,6: trigger & enable system error L3 bf recovery.
	 * <band>,7: trigger & enable system error L4 mdp recovery.
	 * <band>,8: trigger & enable system error full recovery.
	 * <band>,9: trigger firmware crash.
	 */
	case UNI_CMD_SER_QUERY:
		ret = mt7996_mcu_set_ser(dev, UNI_CMD_SER_QUERY, 0, band);
		break;
	case UNI_CMD_SER_SET_RECOVER_L1:
	case UNI_CMD_SER_SET_RECOVER_L2:
	case UNI_CMD_SER_SET_RECOVER_L3_RX_ABORT:
	case UNI_CMD_SER_SET_RECOVER_L3_TX_ABORT:
	case UNI_CMD_SER_SET_RECOVER_L3_TX_DISABLE:
	case UNI_CMD_SER_SET_RECOVER_L3_BF:
	case UNI_CMD_SER_SET_RECOVER_L4_MDP:
		ret = mt7996_mcu_set_ser(dev, UNI_CMD_SER_SET, BIT(val), band);
		if (ret)
			return ret;

		ret = mt7996_mcu_set_ser(dev, UNI_CMD_SER_TRIGGER, val, band);
		break;

	/* enable full chip reset */
	case UNI_CMD_SER_SET_RECOVER_FULL:
		mt76_set(dev, MT_WFDMA0_MCU_HOST_INT_ENA, MT_MCU_CMD_WDT_MASK);
		dev->recovery.state |= MT_MCU_CMD_WDT_MASK;
		mt7996_reset(dev);
		break;

	/* WARNING: trigger firmware crash */
	case UNI_CMD_SER_SET_SYSTEM_ASSERT:
		ret = mt7996_mcu_trigger_assert(dev);
		if (ret)
			return ret;
		break;
	default:
		break;
	}

	return ret ? ret : count;
}

static ssize_t
mt7996_sys_recovery_get(struct file *file, char __user *user_buf,
			size_t count, loff_t *ppos)
{
	struct mt7996_dev *dev = file->private_data;
	char *buff;
	int desc = 0;
	ssize_t ret;
	static const size_t bufsz = 1024;

	buff = kmalloc(bufsz, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	/* HELP */
	desc += scnprintf(buff + desc, bufsz - desc,
			  "Please echo the correct value ...\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,0: grab firmware transient SER state\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,1: trigger system error L1 recovery\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,2: trigger system error L2 recovery\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,3: trigger system error L3 rx abort\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,4: trigger system error L3 tx abort\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,5: trigger system error L3 tx disable\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,6: trigger system error L3 bf recovery\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,7: trigger system error L4 mdp recovery\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,8: trigger system error full recovery\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "<band>,9: trigger firmware crash\n");

	/* SER statistics */
	desc += scnprintf(buff + desc, bufsz - desc,
			  "\nlet's dump firmware SER statistics...\n");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_STATUS        = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_SER_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PLE_ERR       = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PLE_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PLE_ERR_1     = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PLE1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PLE_ERR_AMSDU = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PLE_AMSDU_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PSE_ERR       = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PSE_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PSE_ERR_1     = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PSE1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR6_B0 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR6_BN0_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR6_B1 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR6_BN1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR6_B2 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR6_BN2_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR7_B0 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR7_BN0_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR7_B1 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR7_BN1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR7_B2 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR7_BN2_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "\nSYS_RESET_COUNT: WM %d, WA %d\n",
			  dev->recovery.wm_reset_count,
			  dev->recovery.wa_reset_count);

	ret = simple_read_from_buffer(user_buf, count, ppos, buff, desc);
	kfree(buff);
	return ret;
}

static const struct file_operations mt7996_sys_recovery_ops = {
	.write = mt7996_sys_recovery_set,
	.read = mt7996_sys_recovery_get,
	.open = simple_open,
	.llseek = default_llseek,
};

static int
mt7996_radar_trigger(void *data, u64 val)
{
#define RADAR_MAIN_CHAIN	1
#define RADAR_BACKGROUND	2
	struct mt7996_dev *dev = data;
	struct mt7996_phy *phy = mt7996_band_phy(dev, NL80211_BAND_5GHZ);
	int rdd_idx;

	if (!phy || !val || val > RADAR_BACKGROUND)
		return -EINVAL;

	if (val == RADAR_BACKGROUND && !dev->rdd2_phy) {
		dev_err(dev->mt76.dev, "Background radar is not enabled\n");
		return -EINVAL;
	}

	rdd_idx = mt7996_get_rdd_idx(phy, val == RADAR_BACKGROUND);
	if (rdd_idx < 0) {
		dev_err(dev->mt76.dev, "No RDD found\n");
		return -EINVAL;
	}

	return mt7996_mcu_rdd_cmd(dev, RDD_RADAR_EMULATE, rdd_idx, 0);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_radar_trigger, NULL,
			 mt7996_radar_trigger, "%lld\n");

static int
mt7996_rdd_monitor(struct seq_file *s, void *data)
{
	struct mt7996_dev *dev = dev_get_drvdata(s->private);
	struct cfg80211_chan_def *chandef = &dev->rdd2_chandef;
	const char *bw;
	int ret = 0;

	mutex_lock(&dev->mt76.mutex);

	if (!cfg80211_chandef_valid(chandef)) {
		ret = -EINVAL;
		goto out;
	}

	if (!dev->rdd2_phy) {
		seq_puts(s, "not running\n");
		goto out;
	}

	switch (chandef->width) {
	case NL80211_CHAN_WIDTH_40:
		bw = "40";
		break;
	case NL80211_CHAN_WIDTH_80:
		bw = "80";
		break;
	case NL80211_CHAN_WIDTH_160:
		bw = "160";
		break;
	case NL80211_CHAN_WIDTH_80P80:
		bw = "80P80";
		break;
	default:
		bw = "20";
		break;
	}

	seq_printf(s, "channel %d (%d MHz) width %s MHz center1: %d MHz\n",
		   chandef->chan->hw_value, chandef->chan->center_freq,
		   bw, chandef->center_freq1);
out:
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static int
mt7996_fw_debug_wm_set(void *data, u64 val)
{
	struct mt7996_dev *dev = data;
	enum {
		DEBUG_TXCMD = 62,
		DEBUG_CMD_RPT_TX,
		DEBUG_CMD_RPT_TRIG,
		DEBUG_SPL,
		DEBUG_RPT_RX,
		DEBUG_RPT_RA = 68,
	} debug;
	bool tx, rx, en;
	int ret;

	dev->fw_debug_wm = val ? MCU_FW_LOG_TO_HOST : 0;

	if (dev->fw_debug_bin)
		val = MCU_FW_LOG_RELAY;
	else
		val = dev->fw_debug_wm;

	tx = dev->fw_debug_wm || (dev->fw_debug_bin & BIT(1));
	rx = dev->fw_debug_wm || (dev->fw_debug_bin & BIT(2));
	en = dev->fw_debug_wm || (dev->fw_debug_bin & BIT(0));

	ret = mt7996_mcu_fw_log_2_host(dev, MCU_FW_LOG_WM, val);
	if (ret)
		return ret;

	for (debug = DEBUG_TXCMD; debug <= DEBUG_RPT_RA; debug++) {
		if (debug == 67)
			continue;

		if (debug == DEBUG_RPT_RX)
			val = en && rx;
		else
			val = en && tx;

		ret = mt7996_mcu_fw_dbg_ctrl(dev, debug, val);
		if (ret)
			return ret;
	}

	return 0;
}

static int
mt7996_fw_debug_wm_get(void *data, u64 *val)
{
	struct mt7996_dev *dev = data;

	*val = dev->fw_debug_wm;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug_wm, mt7996_fw_debug_wm_get,
			 mt7996_fw_debug_wm_set, "%lld\n");

static int
mt7996_fw_debug_wa_set(void *data, u64 val)
{
	struct mt7996_dev *dev = data;
	int ret;

	dev->fw_debug_wa = val ? MCU_FW_LOG_TO_HOST : 0;

	ret = mt7996_mcu_fw_log_2_host(dev, MCU_FW_LOG_WA, dev->fw_debug_wa);
	if (ret)
		return ret;

	return mt7996_mcu_wa_cmd(dev, MCU_WA_PARAM_CMD(SET), MCU_WA_PARAM_PDMA_RX,
				 !!dev->fw_debug_wa, 0);
}

static int
mt7996_fw_debug_wa_get(void *data, u64 *val)
{
	struct mt7996_dev *dev = data;

	*val = dev->fw_debug_wa;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug_wa, mt7996_fw_debug_wa_get,
			 mt7996_fw_debug_wa_set, "%lld\n");

static struct dentry *
create_buf_file_cb(const char *filename, struct dentry *parent, umode_t mode,
		   struct rchan_buf *buf, int *is_global)
{
	struct dentry *f;

	f = debugfs_create_file("fwlog_data", mode, parent, buf,
				&relay_file_operations);
	if (IS_ERR(f))
		return NULL;

	*is_global = 1;

	return f;
}

static int
remove_buf_file_cb(struct dentry *f)
{
	debugfs_remove(f);

	return 0;
}

static int
mt7996_fw_debug_bin_set(void *data, u64 val)
{
	static struct rchan_callbacks relay_cb = {
		.create_buf_file = create_buf_file_cb,
		.remove_buf_file = remove_buf_file_cb,
	};
	struct mt7996_dev *dev = data;

	if (!dev->relay_fwlog)
		dev->relay_fwlog = relay_open("fwlog_data", dev->debugfs_dir,
					      1500, 512, &relay_cb, NULL);
	if (!dev->relay_fwlog)
		return -ENOMEM;

	dev->fw_debug_bin = val;

	relay_reset(dev->relay_fwlog);

	return mt7996_fw_debug_wm_set(dev, dev->fw_debug_wm);
}

static int
mt7996_fw_debug_bin_get(void *data, u64 *val)
{
	struct mt7996_dev *dev = data;

	*val = dev->fw_debug_bin;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug_bin, mt7996_fw_debug_bin_get,
			 mt7996_fw_debug_bin_set, "%lld\n");

static int
mt7996_fw_util_wa_show(struct seq_file *file, void *data)
{
	struct mt7996_dev *dev = file->private;

	if (dev->fw_debug_wa)
		return mt7996_mcu_wa_cmd(dev, MCU_WA_PARAM_CMD(QUERY),
					 MCU_WA_PARAM_CPU_UTIL, 0, 0);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7996_fw_util_wa);

static void
mt7996_ampdu_stat_read_phy(struct mt7996_phy *phy, struct seq_file *file)
{
	struct mt7996_dev *dev = phy->dev;
	int bound[15], range[8], i;
	u8 band_idx = phy->mt76->band_idx;

	/* Tx ampdu stat */
	for (i = 0; i < ARRAY_SIZE(range); i++)
		range[i] = mt76_rr(dev, MT_MIB_ARNG(band_idx, i));

	for (i = 0; i < ARRAY_SIZE(bound); i++)
		bound[i] = MT_MIB_ARNCR_RANGE(range[i / 2], i % 2) + 1;

	seq_printf(file, "\nPhy %s, Phy band %d\n",
		   wiphy_name(phy->mt76->hw->wiphy), band_idx);

	seq_printf(file, "Length: %8d | ", bound[0]);
	for (i = 0; i < ARRAY_SIZE(bound) - 1; i++)
		seq_printf(file, "%3d -%3d | ",
			   bound[i] + 1, bound[i + 1]);

	seq_puts(file, "\nCount:  ");
	for (i = 0; i < ARRAY_SIZE(bound); i++)
		seq_printf(file, "%8d | ", phy->mt76->aggr_stats[i]);
	seq_puts(file, "\n");

	seq_printf(file, "BA miss count: %d\n", phy->mib.ba_miss_cnt);
}

static void
mt7996_txbf_stat_read_phy(struct mt7996_phy *phy, struct seq_file *s)
{
	struct mt76_mib_stats *mib = &phy->mib;
	static const char * const bw[] = {
		"BW20", "BW40", "BW80", "BW160", "BW320"
	};

	/* Tx Beamformer monitor */
	seq_puts(s, "\nTx Beamformer applied PPDU counts: ");

	seq_printf(s, "iBF: %d, eBF: %d\n",
		   mib->tx_bf_ibf_ppdu_cnt,
		   mib->tx_bf_ebf_ppdu_cnt);

	/* Tx Beamformer Rx feedback monitor */
	seq_puts(s, "Tx Beamformer Rx feedback statistics: ");

	seq_printf(s, "All: %d, EHT: %d, HE: %d, VHT: %d, HT: %d, ",
		   mib->tx_bf_rx_fb_all_cnt,
		   mib->tx_bf_rx_fb_eht_cnt,
		   mib->tx_bf_rx_fb_he_cnt,
		   mib->tx_bf_rx_fb_vht_cnt,
		   mib->tx_bf_rx_fb_ht_cnt);

	seq_printf(s, "%s, NC: %d, NR: %d\n",
		   bw[mib->tx_bf_rx_fb_bw],
		   mib->tx_bf_rx_fb_nc_cnt,
		   mib->tx_bf_rx_fb_nr_cnt);

	/* Tx Beamformee Rx NDPA & Tx feedback report */
	seq_printf(s, "Tx Beamformee successful feedback frames: %d\n",
		   mib->tx_bf_fb_cpl_cnt);
	seq_printf(s, "Tx Beamformee feedback triggered counts: %d\n",
		   mib->tx_bf_fb_trig_cnt);

	/* Tx SU & MU counters */
	seq_printf(s, "Tx multi-user Beamforming counts: %d\n",
		   mib->tx_mu_bf_cnt);
	seq_printf(s, "Tx multi-user MPDU counts: %d\n", mib->tx_mu_mpdu_cnt);
	seq_printf(s, "Tx multi-user successful MPDU counts: %d\n",
		   mib->tx_mu_acked_mpdu_cnt);
	seq_printf(s, "Tx single-user successful MPDU counts: %d\n",
		   mib->tx_su_acked_mpdu_cnt);

	seq_puts(s, "\n");
}

static void
mt7996_tx_stats_show_phy(struct seq_file *file, struct mt7996_phy *phy)
{
	struct mt76_mib_stats *mib = &phy->mib;
	u32 attempts, success, per;
	int i;

	mt7996_mac_update_stats(phy);
	mt7996_ampdu_stat_read_phy(phy, file);

	attempts = mib->tx_mpdu_attempts_cnt;
	success = mib->tx_mpdu_success_cnt;
	per = attempts ? 100 - success * 100 / attempts : 100;
	seq_printf(file, "Tx attempts: %8u (MPDUs)\n", attempts);
	seq_printf(file, "Tx success: %8u (MPDUs)\n", success);
	seq_printf(file, "Tx PER: %u%%\n", per);

	mt7996_txbf_stat_read_phy(phy, file);

	/* Tx amsdu info */
	seq_puts(file, "Tx MSDU statistics:\n");
	for (i = 0; i < ARRAY_SIZE(mib->tx_amsdu); i++) {
		seq_printf(file, "AMSDU pack count of %d MSDU in TXD: %8d ",
			   i + 1, mib->tx_amsdu[i]);
		if (mib->tx_amsdu_cnt)
			seq_printf(file, "(%3d%%)\n",
				   mib->tx_amsdu[i] * 100 / mib->tx_amsdu_cnt);
		else
			seq_puts(file, "\n");
	}
}

static int
mt7996_tx_stats_show(struct seq_file *file, void *data)
{
	struct mt7996_dev *dev = file->private;
	struct mt7996_phy *phy = &dev->phy;

	mutex_lock(&dev->mt76.mutex);

	mt7996_tx_stats_show_phy(file, phy);
	phy = mt7996_phy2(dev);
	if (phy)
		mt7996_tx_stats_show_phy(file, phy);
	phy = mt7996_phy3(dev);
	if (phy)
		mt7996_tx_stats_show_phy(file, phy);

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7996_tx_stats);

static void
mt7996_hw_queue_read(struct seq_file *s, u32 size,
		     const struct hw_queue_map *map)
{
	struct mt7996_phy *phy = s->private;
	struct mt7996_dev *dev = phy->dev;
	u32 i, val;

	val = mt76_rr(dev, MT_FL_Q_EMPTY);
	for (i = 0; i < size; i++) {
		u32 ctrl, head, tail, queued;

		if (val & BIT(map[i].index))
			continue;

		ctrl = BIT(31) | (map[i].pid << 10) | ((u32)map[i].qid << 24);
		mt76_wr(dev, MT_FL_Q0_CTRL, ctrl);

		head = mt76_get_field(dev, MT_FL_Q2_CTRL,
				      GENMASK(11, 0));
		tail = mt76_get_field(dev, MT_FL_Q2_CTRL,
				      GENMASK(27, 16));
		queued = mt76_get_field(dev, MT_FL_Q3_CTRL,
					GENMASK(11, 0));

		seq_printf(s, "\t%s: ", map[i].name);
		seq_printf(s, "queued:0x%03x head:0x%03x tail:0x%03x\n",
			   queued, head, tail);
	}
}

static void
mt7996_sta_hw_queue_read(void *data, struct ieee80211_sta *sta)
{
	struct mt7996_sta *msta = (struct mt7996_sta *)sta->drv_priv;
	struct mt7996_vif *mvif = msta->vif;
	struct mt7996_dev *dev = mvif->deflink.phy->dev;
	struct ieee80211_link_sta *link_sta;
	struct seq_file *s = data;
	struct ieee80211_vif *vif;
	unsigned int link_id;

	vif = container_of((void *)mvif, struct ieee80211_vif, drv_priv);

	rcu_read_lock();

	for_each_sta_active_link(vif, sta, link_sta, link_id) {
		struct mt7996_sta_link *msta_link;
		struct mt76_vif_link *mlink;
		u8 ac;

		mlink = rcu_dereference(mvif->mt76.link[link_id]);
		if (!mlink)
			continue;

		msta_link = rcu_dereference(msta->link[link_id]);
		if (!msta_link)
			continue;

		for (ac = 0; ac < 4; ac++) {
			u32 idx = msta_link->wcid.idx >> 5, qlen, ctrl, val;
			u8 offs = msta_link->wcid.idx & GENMASK(4, 0);

			ctrl = BIT(31) | BIT(11) | (ac << 24);
			val = mt76_rr(dev, MT_PLE_AC_QEMPTY(ac, idx));

			if (val & BIT(offs))
				continue;

			mt76_wr(dev,
				MT_FL_Q0_CTRL, ctrl | msta_link->wcid.idx);
			qlen = mt76_get_field(dev, MT_FL_Q3_CTRL,
					      GENMASK(11, 0));
			seq_printf(s, "\tSTA %pM wcid %d: AC%d%d queued:%d\n",
				   sta->addr, msta_link->wcid.idx,
				   mlink->wmm_idx, ac, qlen);
		}
	}

	rcu_read_unlock();
}

static int
mt7996_hw_queues_show(struct seq_file *file, void *data)
{
	struct mt7996_dev *dev = file->private;
	struct mt7996_phy *phy = &dev->phy;
	static const struct hw_queue_map ple_queue_map[] = {
		{ "CPU_Q0",  0,  1, MT_CTX0	      },
		{ "CPU_Q1",  1,  1, MT_CTX0 + 1	      },
		{ "CPU_Q2",  2,  1, MT_CTX0 + 2	      },
		{ "CPU_Q3",  3,  1, MT_CTX0 + 3	      },
		{ "ALTX_Q0", 8,  2, MT_LMAC_ALTX0     },
		{ "BMC_Q0",  9,  2, MT_LMAC_BMC0      },
		{ "BCN_Q0",  10, 2, MT_LMAC_BCN0      },
		{ "PSMP_Q0", 11, 2, MT_LMAC_PSMP0     },
		{ "ALTX_Q1", 12, 2, MT_LMAC_ALTX0 + 4 },
		{ "BMC_Q1",  13, 2, MT_LMAC_BMC0  + 4 },
		{ "BCN_Q1",  14, 2, MT_LMAC_BCN0  + 4 },
		{ "PSMP_Q1", 15, 2, MT_LMAC_PSMP0 + 4 },
	};
	static const struct hw_queue_map pse_queue_map[] = {
		{ "CPU Q0",  0,  1, MT_CTX0	      },
		{ "CPU Q1",  1,  1, MT_CTX0 + 1	      },
		{ "CPU Q2",  2,  1, MT_CTX0 + 2	      },
		{ "CPU Q3",  3,  1, MT_CTX0 + 3	      },
		{ "HIF_Q0",  8,  0, MT_HIF0	      },
		{ "HIF_Q1",  9,  0, MT_HIF0 + 1	      },
		{ "HIF_Q2",  10, 0, MT_HIF0 + 2	      },
		{ "HIF_Q3",  11, 0, MT_HIF0 + 3	      },
		{ "HIF_Q4",  12, 0, MT_HIF0 + 4	      },
		{ "HIF_Q5",  13, 0, MT_HIF0 + 5	      },
		{ "LMAC_Q",  16, 2, 0		      },
		{ "MDP_TXQ", 17, 2, 1		      },
		{ "MDP_RXQ", 18, 2, 2		      },
		{ "SEC_TXQ", 19, 2, 3		      },
		{ "SEC_RXQ", 20, 2, 4		      },
	};
	u32 val, head, tail;

	/* ple queue */
	val = mt76_rr(dev, MT_PLE_FREEPG_CNT);
	head = mt76_get_field(dev, MT_PLE_FREEPG_HEAD_TAIL, GENMASK(11, 0));
	tail = mt76_get_field(dev, MT_PLE_FREEPG_HEAD_TAIL, GENMASK(27, 16));
	seq_puts(file, "PLE page info:\n");
	seq_printf(file,
		   "\tTotal free page: 0x%08x head: 0x%03x tail: 0x%03x\n",
		   val, head, tail);

	val = mt76_rr(dev, MT_PLE_PG_HIF_GROUP);
	head = mt76_get_field(dev, MT_PLE_HIF_PG_INFO, GENMASK(11, 0));
	tail = mt76_get_field(dev, MT_PLE_HIF_PG_INFO, GENMASK(27, 16));
	seq_printf(file, "\tHIF free page: 0x%03x res: 0x%03x used: 0x%03x\n",
		   val, head, tail);

	seq_puts(file, "PLE non-empty queue info:\n");
	mt7996_hw_queue_read(file, ARRAY_SIZE(ple_queue_map),
			     &ple_queue_map[0]);

	/* iterate per-sta ple queue */
	ieee80211_iterate_stations_atomic(phy->mt76->hw,
					  mt7996_sta_hw_queue_read, file);
	phy = mt7996_phy2(dev);
	if (phy)
		ieee80211_iterate_stations_atomic(phy->mt76->hw,
						  mt7996_sta_hw_queue_read, file);
	phy = mt7996_phy3(dev);
	if (phy)
		ieee80211_iterate_stations_atomic(phy->mt76->hw,
						  mt7996_sta_hw_queue_read, file);

	/* pse queue */
	seq_puts(file, "PSE non-empty queue info:\n");
	mt7996_hw_queue_read(file, ARRAY_SIZE(pse_queue_map),
			     &pse_queue_map[0]);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7996_hw_queues);

static int
mt7996_xmit_queues_show(struct seq_file *file, void *data)
{
	struct mt7996_dev *dev = file->private;
	struct mt7996_phy *phy;
	struct {
		struct mt76_queue *q;
		char *queue;
	} queue_map[] = {
		{ dev->mphy.q_tx[MT_TXQ_BE],	 "  MAIN0"  },
		{ NULL,				 "  MAIN1"  },
		{ NULL,				 "  MAIN2"  },
		{ dev->mt76.q_mcu[MT_MCUQ_WM],	 "  MCUWM"  },
		{ dev->mt76.q_mcu[MT_MCUQ_WA],	 "  MCUWA"  },
		{ dev->mt76.q_mcu[MT_MCUQ_FWDL], "MCUFWDL" },
	};
	int i;

	phy = mt7996_phy2(dev);
	if (phy)
		queue_map[1].q = phy->mt76->q_tx[MT_TXQ_BE];

	phy = mt7996_phy3(dev);
	if (phy)
		queue_map[2].q = phy->mt76->q_tx[MT_TXQ_BE];

	seq_puts(file, "     queue | hw-queued |      head |      tail |\n");
	for (i = 0; i < ARRAY_SIZE(queue_map); i++) {
		struct mt76_queue *q = queue_map[i].q;

		if (!q)
			continue;

		seq_printf(file, "   %s | %9d | %9d | %9d |\n",
			   queue_map[i].queue, q->queued, q->head,
			   q->tail);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7996_xmit_queues);

static int
mt7996_twt_stats(struct seq_file *s, void *data)
{
	struct mt7996_dev *dev = dev_get_drvdata(s->private);
	struct mt7996_twt_flow *iter;

	rcu_read_lock();

	seq_puts(s, "     wcid |       id |    flags |      exp | mantissa");
	seq_puts(s, " | duration |            tsf |\n");
	list_for_each_entry_rcu(iter, &dev->twt_list, list)
		seq_printf(s,
			   "%9d | %8d | %5c%c%c%c | %8d | %8d | %8d | %14lld |\n",
			   iter->wcid, iter->id,
			   iter->sched ? 's' : 'u',
			   iter->protection ? 'p' : '-',
			   iter->trigger ? 't' : '-',
			   iter->flowtype ? '-' : 'a',
			   iter->exp, iter->mantissa,
			   iter->duration, iter->tsf);

	rcu_read_unlock();

	return 0;
}

/* The index of RF registers use the generic regidx, combined with two parts:
 * WF selection [31:24] and offset [23:0].
 */
static int
mt7996_rf_regval_get(void *data, u64 *val)
{
	struct mt7996_dev *dev = data;
	u32 regval;
	int ret;

	ret = mt7996_mcu_rf_regval(dev, dev->mt76.debugfs_reg, &regval, false);
	if (ret)
		return ret;

	*val = regval;

	return 0;
}

static int
mt7996_rf_regval_set(void *data, u64 val)
{
	struct mt7996_dev *dev = data;
	u32 val32 = val;

	return mt7996_mcu_rf_regval(dev, dev->mt76.debugfs_reg, &val32, true);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_rf_regval, mt7996_rf_regval_get,
			 mt7996_rf_regval_set, "0x%08llx\n");

static void
dump_dma_tx_ring_info(struct seq_file *s, struct mt7996_dev *dev,  char *str1, char *str2, u32 ring_base)
{
	u32 base, cnt, cidx, didx, queue_cnt;

	base= mt76_rr(dev, ring_base);
	cnt = mt76_rr(dev, ring_base + 4);
	cidx = mt76_rr(dev, ring_base + 8);
	didx = mt76_rr(dev, ring_base + 12);
	queue_cnt = (cidx >= didx) ? (cidx - didx) : (cidx - didx + cnt);

	seq_printf(s, "%20s %6s %10x %15x %10x %10x %10x\n", str1, str2, base, cnt, cidx, didx, queue_cnt);
}

static void
dump_dma_rx_ring_info(struct seq_file *s, struct mt7996_dev *dev,  char *str1, char *str2, u32 ring_base)
{
	u32 base, ctrl1, cnt, cidx, didx, queue_cnt;

	base= mt76_rr(dev, ring_base);
	ctrl1 = mt76_rr(dev, ring_base + 4);
	cidx = mt76_rr(dev, ring_base + 8) & 0xfff;
	didx = mt76_rr(dev, ring_base + 12) & 0xfff;
	cnt = ctrl1 & 0xfff;
	queue_cnt = (didx > cidx) ? (didx - cidx - 1) : (didx - cidx + cnt - 1);

	seq_printf(s, "%20s %6s %10x %10x(%3x) %10x %10x %10x\n",
		   str1, str2, base, ctrl1, cnt, cidx, didx, queue_cnt);
}

static void
mt7996_show_dma_info(struct seq_file *s, struct mt7996_dev *dev)
{
	u32 sys_ctrl[10];

	/* HOST DMA0 information */
	sys_ctrl[0] = mt76_rr(dev, WF_WFDMA_HOST_DMA0_HOST_INT_STA_ADDR);
	sys_ctrl[1] = mt76_rr(dev, WF_WFDMA_HOST_DMA0_HOST_INT_ENA_ADDR);
	sys_ctrl[2] = mt76_rr(dev, WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_ADDR);

	seq_printf(s, "HOST_DMA Configuration\n");
	seq_printf(s, "%10s %10s %10s %10s %10s %10s\n",
		"DMA", "IntCSR", "IntMask", "Glocfg", "Tx/RxEn", "Tx/RxBusy");
	seq_printf(s, "%10s %10x %10x %10x %4x/%5x %4x/%5x\n",
		"DMA0", sys_ctrl[0], sys_ctrl[1], sys_ctrl[2],
		(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_TX_DMA_EN_MASK)
			>> WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_TX_DMA_EN_SHFT,
		(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_RX_DMA_EN_MASK)
			>> WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_RX_DMA_EN_SHFT,
		(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_TX_DMA_BUSY_MASK)
			>> WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_TX_DMA_BUSY_SHFT,
		(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_RX_DMA_BUSY_MASK)
			>> WF_WFDMA_HOST_DMA0_WPDMA_GLO_CFG_RX_DMA_BUSY_SHFT);

	if (dev->hif2) {
		/* HOST DMA1 information */
		sys_ctrl[0] = mt76_rr(dev, WF_WFDMA_HOST_DMA0_PCIE1_HOST_INT_STA_ADDR);
		sys_ctrl[1] = mt76_rr(dev, WF_WFDMA_HOST_DMA0_PCIE1_HOST_INT_ENA_ADDR);
		sys_ctrl[2] = mt76_rr(dev, WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_ADDR);

		seq_printf(s, "%10s %10x %10x %10x %4x/%5x %4x/%5x\n",
			"DMA0P1", sys_ctrl[0], sys_ctrl[1], sys_ctrl[2],
			(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_TX_DMA_EN_MASK)
				>> WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_TX_DMA_EN_SHFT,
			(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_RX_DMA_EN_MASK)
				>> WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_RX_DMA_EN_SHFT,
			(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_TX_DMA_BUSY_MASK)
				>> WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_TX_DMA_BUSY_SHFT,
			(sys_ctrl[2] & WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_RX_DMA_BUSY_MASK)
				>> WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_GLO_CFG_RX_DMA_BUSY_SHFT);
	}

	seq_printf(s, "HOST_DMA0 Ring Configuration\n");
	seq_printf(s, "%20s %6s %10s %15s %10s %10s %10s\n",
		"Name", "Used", "Base", "Ctrl1(Cnt)", "CIDX", "DIDX", "QCnt");
	dump_dma_tx_ring_info(s, dev, "T0:TXD0(H2MAC)", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING0_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T1:TXD1(H2MAC)", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING1_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T2:TXD2(H2MAC)", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING2_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T3:", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING3_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T4:", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING4_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T5:", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING5_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T6:", "STA",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING6_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T16:FWDL", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING16_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T17:Cmd(H2WM)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_TX_RING17_CTRL0_ADDR);
	if (mt7996_has_wa(dev)) {
		dump_dma_tx_ring_info(s, dev, "T18:TXD0(H2WA)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING18_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T19:TXD1(H2WA)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING19_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T20:Cmd(H2WA)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING20_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T21:TXD2(H2WA)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING21_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T22:TXD3(H2WA)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING22_CTRL0_ADDR);
	} else {
		dump_dma_tx_ring_info(s, dev, "T18:TXD0(H2SDO)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING18_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T19:TXD1(H2SDO)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING19_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T20:Reserved", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING20_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T21:TXD2(H2SDO)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING21_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T22:TXD3(H2SDO)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_TX_RING22_CTRL0_ADDR);
	}


	dump_dma_rx_ring_info(s, dev, "R0:Event(WM2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING0_CTRL0_ADDR);
	if (mt7996_has_wa(dev)) {
		dump_dma_rx_ring_info(s, dev, "R1:Event(WA2H)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING1_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R2:TxDone0(WA2H)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING2_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R3:TxDone1(WA2H)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING3_CTRL0_ADDR);
	} else {
		dump_dma_rx_ring_info(s, dev, "R1:Event(SDO2H)", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING1_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R2:Reserved", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING2_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R3:Reserved", "AP",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING3_CTRL0_ADDR);
	}
	dump_dma_rx_ring_info(s, dev, "R4:Data0(MAC2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING4_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R5:Data1(MAC2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING5_CTRL0_ADDR);
	if (is_mt7996(&dev->mt76))
		dump_dma_rx_ring_info(s, dev, "R6:BUF1(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING6_CTRL0_ADDR);
	else
		dump_dma_rx_ring_info(s, dev, "R6:TxDone0(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING6_CTRL0_ADDR);
	if (is_mt7990(&dev->mt76))
		dump_dma_rx_ring_info(s, dev, "R7:Reserved)", "Both",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING7_CTRL0_ADDR);
	else
		dump_dma_rx_ring_info(s, dev, "R7:TxDone1(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING7_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R8:BUF0(MAC2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING8_CTRL0_ADDR);
	if (is_mt7996(&dev->mt76))
		dump_dma_rx_ring_info(s, dev, "R9:TxDone0(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING9_CTRL0_ADDR);
	else
		dump_dma_rx_ring_info(s, dev, "R9:BUF0(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_WPDMA_RX_RING9_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R10:MSDU_PG0(MAC2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING10_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R11:MSDU_PG1(MAC2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING11_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R12:MSDU_PG2(MAC2H)", "Both",
		WF_WFDMA_HOST_DMA0_WPDMA_RX_RING12_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "IND:IND_CMD(MAC2H)", "Both",
		WF_RRO_TOP_IND_CMD_0_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "RRO:Data0(MAC2H)", "Both",
		WF_RRO_TOP_RX_RING_AP_0_CTRL0_ADDR);

	if (dev->hif2) {
		seq_printf(s, "HOST_DMA0 PCIe1 Ring Configuration\n");
		seq_printf(s, "%20s %6s %10s %15s %10s %10s %10s\n",
			"Name", "Used", "Base", "Ctrl1(Cnt)", "CIDX", "DIDX", "QCnt");
		if (mt7996_has_wa(dev)) {
			dump_dma_tx_ring_info(s, dev, "T21:TXD2(H2WA)", "AP",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_TX_RING21_CTRL0_ADDR);
			dump_dma_tx_ring_info(s, dev, "T22:TXD?(H2WA)", "AP",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_TX_RING22_CTRL0_ADDR);
			dump_dma_rx_ring_info(s, dev, "R3:TxDone1(WA2H)", "AP",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_RX_RING3_CTRL0_ADDR);
		} else {
			dump_dma_tx_ring_info(s, dev, "T21:TXD2(H2SDO)", "AP",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_TX_RING21_CTRL0_ADDR);
			dump_dma_tx_ring_info(s, dev, "T22:TXD?(H2SDO)", "AP",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_TX_RING22_CTRL0_ADDR);
			dump_dma_rx_ring_info(s, dev, "R3:Reserved", "AP",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_RX_RING3_CTRL0_ADDR);
		}
		dump_dma_rx_ring_info(s, dev, "R5:Data1(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_RX_RING5_CTRL0_ADDR);
		if (is_mt7996(&dev->mt76))
			dump_dma_rx_ring_info(s, dev, "R6:BUF1(MAC2H)", "Both",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_RX_RING6_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R7:TxDone1(MAC2H)", "Both",
			WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_RX_RING7_CTRL0_ADDR);
		if (is_mt7992(&dev->mt76) || is_mt7990(&dev->mt76))
			dump_dma_rx_ring_info(s, dev, "R9:BUF1(MAC2H)", "Both",
				WF_WFDMA_HOST_DMA0_PCIE1_WPDMA_RX_RING9_CTRL0_ADDR);
	}

	/* MCU DMA information */
	sys_ctrl[0] = mt76_rr(dev, WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_ADDR);
	sys_ctrl[1] = mt76_rr(dev, WF_WFDMA_MCU_DMA0_HOST_INT_STA_ADDR);
	sys_ctrl[2] = mt76_rr(dev, WF_WFDMA_MCU_DMA0_HOST_INT_ENA_ADDR);

	seq_printf(s, "MCU_DMA Configuration\n");
	seq_printf(s, "%10s %10s %10s %10s %10s %10s\n",
		"DMA", "IntCSR", "IntMask", "Glocfg", "Tx/RxEn", "Tx/RxBusy");
	seq_printf(s, "%10s %10x %10x %10x %4x/%5x %4x/%5x\n",
		"DMA0", sys_ctrl[1], sys_ctrl[2], sys_ctrl[0],
		(sys_ctrl[0] & WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_TX_DMA_EN_MASK)
			>> WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_TX_DMA_EN_SHFT,
		(sys_ctrl[0] & WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_RX_DMA_EN_MASK)
			>> WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_RX_DMA_EN_SHFT,
		(sys_ctrl[0] & WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_TX_DMA_BUSY_MASK)
			>> WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_TX_DMA_BUSY_SHFT,
		(sys_ctrl[0] & WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_RX_DMA_BUSY_MASK)
			>> WF_WFDMA_MCU_DMA0_WPDMA_GLO_CFG_RX_DMA_BUSY_SHFT);

	seq_printf(s, "MCU_DMA0 Ring Configuration\n");
	seq_printf(s, "%20s %6s %10s %15s %10s %10s %10s\n",
		"Name", "Used", "Base", "Cnt", "CIDX", "DIDX", "QCnt");
	dump_dma_tx_ring_info(s, dev, "T0:Event(WM2H)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_TX_RING0_CTRL0_ADDR);
	if (mt7996_has_wa(dev)) {
		dump_dma_tx_ring_info(s, dev, "T1:Event(WA2H)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING1_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T2:TxDone0(WA2H)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING2_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T3:TxDone1(WA2H)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING3_CTRL0_ADDR);
	} else {
		dump_dma_tx_ring_info(s, dev, "T1:Event(SDO2H)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING1_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T2:Reserved", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING2_CTRL0_ADDR);
		dump_dma_tx_ring_info(s, dev, "T3:Reserved", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING3_CTRL0_ADDR);
	}
	dump_dma_tx_ring_info(s, dev, "T4:TXD(WM2MAC)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_TX_RING4_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T5:TXCMD(WM2MAC)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_TX_RING5_CTRL0_ADDR);
	if (mt7996_has_wa(dev))
		dump_dma_tx_ring_info(s, dev, "T6:TXD(WA2MAC)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING6_CTRL0_ADDR);
	else
		dump_dma_tx_ring_info(s, dev, "T6:TXD(SDO2MAC)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_TX_RING6_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R0:FWDL", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING0_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R1:Cmd(H2WM)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING1_CTRL0_ADDR);
	if (mt7996_has_wa(dev)) {
		dump_dma_rx_ring_info(s, dev, "R2:TXD0(H2WA)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING2_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R3:TXD1(H2WA)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING3_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R4:Cmd(H2WA)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING4_CTRL0_ADDR);
	} else {
		dump_dma_rx_ring_info(s, dev, "R2:TXD0(H2SDO)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING2_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R3:TXD1(H2SDO)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING3_CTRL0_ADDR);
		dump_dma_rx_ring_info(s, dev, "R4:Reserved", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING4_CTRL0_ADDR);
	}
	dump_dma_rx_ring_info(s, dev, "R5:Data0(MAC2WM)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING5_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R6:TxDone(MAC2WM)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING6_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R7:SPL/RPT(MAC2WM)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING7_CTRL0_ADDR);
	if (mt7996_has_wa(dev))
		dump_dma_rx_ring_info(s, dev, "R8:TxDone(MAC2WA)", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING8_CTRL0_ADDR);
	else
		dump_dma_rx_ring_info(s, dev, "R8:Reserved", "AP",
			WF_WFDMA_MCU_DMA0_WPDMA_RX_RING8_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R9:Data1(MAC2WM)", "Both",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING9_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R10:TXD2(H2WA)", "AP",
		WF_WFDMA_MCU_DMA0_WPDMA_RX_RING10_CTRL0_ADDR);

	/* MEM DMA information */
	sys_ctrl[0] = mt76_rr(dev, WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_ADDR);
	sys_ctrl[1] = mt76_rr(dev, WF_WFDMA_MEM_DMA_HOST_INT_STA_ADDR);
	sys_ctrl[2] = mt76_rr(dev, WF_WFDMA_MEM_DMA_HOST_INT_ENA_ADDR);

	seq_printf(s, "MEM_DMA Configuration\n");
	seq_printf(s, "%10s %10s %10s %10s %10s %10s\n",
		"DMA", "IntCSR", "IntMask", "Glocfg", "Tx/RxEn", "Tx/RxBusy");
	seq_printf(s, "%10s %10x %10x %10x %4x/%5x %4x/%5x\n",
		"MEM", sys_ctrl[1], sys_ctrl[2], sys_ctrl[0],
		(sys_ctrl[0] & WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_TX_DMA_EN_MASK)
			>> WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_TX_DMA_EN_SHFT,
		(sys_ctrl[0] & WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_RX_DMA_EN_MASK)
			>> WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_RX_DMA_EN_SHFT,
		(sys_ctrl[0] & WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_TX_DMA_BUSY_MASK)
			>> WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_TX_DMA_BUSY_SHFT,
		(sys_ctrl[0] & WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_RX_DMA_BUSY_MASK)
			>> WF_WFDMA_MEM_DMA_WPDMA_GLO_CFG_RX_DMA_BUSY_SHFT);

	seq_printf(s, "MEM_DMA Ring Configuration\n");
	seq_printf(s, "%20s %6s %10s %10s %10s %10s %10s\n",
		"Name", "Used", "Base", "Cnt", "CIDX", "DIDX", "QCnt");
	dump_dma_tx_ring_info(s, dev, "T0:CmdEvent(WM2WA)", "AP",
		WF_WFDMA_MEM_DMA_WPDMA_TX_RING0_CTRL0_ADDR);
	dump_dma_tx_ring_info(s, dev, "T1:CmdEvent(WA2WM)", "AP",
		WF_WFDMA_MEM_DMA_WPDMA_TX_RING1_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R0:CmdEvent(WM2WA)", "AP",
		WF_WFDMA_MEM_DMA_WPDMA_RX_RING0_CTRL0_ADDR);
	dump_dma_rx_ring_info(s, dev, "R1:CmdEvent(WA2WM)", "AP",
		WF_WFDMA_MEM_DMA_WPDMA_RX_RING1_CTRL0_ADDR);
}

static int mt7996_trinfo_read(struct seq_file *s, void *data)
{
	struct mt7996_dev *dev = dev_get_drvdata(s->private);
	mt7996_show_dma_info(s, dev);
	return 0;
}

unsigned long int counter_base[BAND_NUM]={0};
static void npu_wifi_offload_get_dbg_counter_address(struct mt7996_dev *dev)
{
    int i=0;
	struct airoha_npu *npu;
    //unsigned long int counter_base[band_num]={0};
	u32 val;
	npu = rcu_dereference(dev->mt76.mmio.npu);
	if(!npu)
	{
		printk("%s:%d npu load fail !!!!!\n",__func__,__LINE__);
		return;
	}
    for(i = 0; i< (BAND_NUM-1); i++)
    {
		if( mt76_npu_get_msg(npu, i, WLAN_FUNC_GET_WAIT_DBG_COUNTER, &val, GFP_ATOMIC)){
			printk("%s:%d band:%d get dbg counter addr error \n",__func__,__LINE__,i);
			return;
		} 
        if(i == 2)
        {
            
            counter_base[i] = (unsigned long int)ioremap((phys_addr_t)val, (UCOUNTER_BOTTOM * sizeof(unsigned int)));
        }
        else
        {
            counter_base[i] = (unsigned long int)ioremap((phys_addr_t)val, (COUNTER_BOTTOM * sizeof(unsigned int)));
        }
		
        printk("counter_base[%d]=%lx(%x) \n", i,counter_base[i],val);
    }
    return;
}

int npu_debug_band;
static ssize_t
npu_wifioffload_dbg_set(struct file *file, const char __user *user_buf,
			size_t count, loff_t *ppos)
{
	struct mt7996_phy *phy = file->private_data;
	struct mt7996_dev *dev = phy->dev;
	char buf[16];
	int ret = 0;
	u16 val;
	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	if (kstrtou16(buf, 0, &val))
		return -EINVAL;

	switch (val) {
	case 0:
	case 1:
		printk("[%s debug counter]\n",(val==0)?"2.4G/5G":"6G");
		//print_npu_wifi_offload_dbg_counter(dev, val);
		npu_debug_band = val;
		if(counter_base[2] == 0)
		{
		    npu_wifi_offload_get_dbg_counter_address(dev);
		}				
		break;
	default:
		printk("plz echo 0(2.4G) or 1(5G).\n");
		break;
	}

	return ret ? ret : count;
}

static ssize_t
npu_wifioffload_dbg_get(struct file *file, char __user *user_buf,
			size_t count, loff_t *ppos)
{
	char *buff;
	int desc = 0;
	ssize_t ret;
	unsigned long int *Counter_Base;
	static const size_t bufsz = 3072;

	buff = kmalloc(bufsz, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	if(counter_base[npu_debug_band]==0)
		return 0;
	else 
		Counter_Base = (unsigned long int *)counter_base[npu_debug_band];	
	desc += scnprintf(buff + desc, bufsz - desc,
			  "[%s debug counter ]\n", (npu_debug_band==0)?"2.4G/5G":"6G");
	desc += scnprintf(buff + desc, bufsz - desc,
			  "get packet while count:\t\t%u\n", NPU_COUNTER(Counter_Base,WHILE_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "RX_DESC_DDONE\t\t\t%u\n", NPU_COUNTER(Counter_Base,RX_DESC_DDONE));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "ALL_GET_PKT_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,ALL_GET_PKT_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "DROP_PACKETS\t\t\t%u\n", NPU_COUNTER(Counter_Base,DROP_PACKETS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "TO_TDMA_COUNT\t\t\t%u\n", NPU_COUNTER(Counter_Base,TO_QDMA_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"GET_BUFID_FAIL\t\t\t%u\n", NPU_COUNTER(Counter_Base,GET_BUFID_FAIL));
				//printk("NO_BUFID\t\t\t%lu\n", NPU_COUNTER(Counter_Base,NO_BUFID));
	desc += scnprintf(buff + desc, bufsz - desc,"SCATTER_CNT_MORE1\t\t%u\n", NPU_COUNTER(Counter_Base, SCATTER_CNT_MORE1));
	desc += scnprintf(buff + desc, bufsz - desc,"BIGGER_PACKET\t\t\t%u\n", NPU_COUNTER(Counter_Base,BIGGER_PACKET));
	desc += scnprintf(buff + desc, bufsz - desc,"SCATTER_PKT_ONE_BUFID\t\t%u\n", NPU_COUNTER(Counter_Base, SCATTER_PKT_ONE_BUFID));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_BIGPKT_TOSRAM_FAIL_COUNT\t%u\n", NPU_COUNTER(Counter_Base, 
	ENQ_BIGPKT_TOSRAM_FAIL_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"------Enq & Deq Counter----------\n");
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_DEQ_WHILE_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,ENQ_DEQ_WHILE_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_SRAM_COUNT:\t\t\t%u\n",NPU_COUNTER(Counter_Base,ENQ_SRAM_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_SRAM_FULL_COUNT:\t\t%u\n",NPU_COUNTER(Counter_Base,ENQ_SRAM_FULL_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_SRAM_FAIL_COUNT:\t\t%u\n",NPU_COUNTER(Counter_Base,ENQ_SRAM_FAIL_COUNT));	
	desc += scnprintf(buff + desc, bufsz - desc,"DEQ_SRAM_COUNT:\t\t\t%u\nDEQ_SRAM_FAIL_BUFID_COUNT:\t%u\nDEQ_SRAM_FAIL_LEN_COUNT:\t\t%u\n",NPU_COUNTER(Counter_Base,DEQ_SRAM_COUNT),NPU_COUNTER(Counter_Base, DEQ_SRAM_FAIL_BUFID_COUNT), NPU_COUNTER(Counter_Base,DEQ_SRAM_FAIL_LEN_COUNT));
				//printk("DEQ_SRAM_NO_INFO_COUNT\t\t%lu\n", NPU_COUNTER(Counter_Base, DEQ_SRAM_NO_INFO_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TO_HOSTAPD_COUNT\t%u\n", NPU_COUNTER(Counter_Base, TO_HOSTAPD_COUNT));
				//printk("ENQ_DRAM_FAIL_COUNT:\t\t%lu\n",NPU_COUNTER(Counter_Base, ENQ_DRAM_FAIL_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"HOST_APD_ERROR_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base, HOST_APD_ERROR_COUNT));
	
	desc += scnprintf(buff + desc, bufsz - desc,"HOSTADPT_API_Q_FULL:\t\t%u\n", NPU_COUNTER(Counter_Base, HOSTADPT_API_Q_FULL));
	desc += scnprintf(buff + desc, bufsz - desc,"API_HOSTADPT_SEND:\t\t%u\n", NPU_COUNTER(Counter_Base, API_HOSTADPT_SEND));
	desc += scnprintf(buff + desc, bufsz - desc,"RX_G4_PKT_NO_SN:\t\t\t%u\n", NPU_COUNTER(Counter_Base, RX_G4_PKT_NO_SN));
	desc += scnprintf(buff + desc, bufsz - desc,"------pipe queue----------\n");
	desc += scnprintf(buff + desc, bufsz - desc,"RX_PIPE_Q_FULL:\t\t\t%u\n", NPU_COUNTER(Counter_Base, RX_PIPE_Q_FULL));
	desc += scnprintf(buff + desc, bufsz - desc,"RX_PIPE_Q3_Q4_FULL:\t\t\t%u\n", NPU_COUNTER(Counter_Base, RX_PIPE_Q3_Q4_FULL));
	desc += scnprintf(buff + desc, bufsz - desc,"PIPE_Q3_ENQ:\t\t\t%u\n", NPU_COUNTER(Counter_Base, PIPE_Q3_ENQ));
	desc += scnprintf(buff + desc, bufsz - desc,"PIPE_Q3_DEQ:\t\t\t%u\n", NPU_COUNTER(Counter_Base, PIPE_Q3_DEQ));
	desc += scnprintf(buff + desc, bufsz - desc,"PIPE_Q4_ENQ:\t\t\t%u\n", NPU_COUNTER(Counter_Base, PIPE_Q4_ENQ));
	desc += scnprintf(buff + desc, bufsz - desc,"PIPE_Q4_DEQ:\t\t\t%u\n", NPU_COUNTER(Counter_Base, PIPE_Q4_DEQ));
	desc += scnprintf(buff + desc, bufsz - desc,"------Enq & Deq scatter packet Counter----------\n");
	desc += scnprintf(buff + desc, bufsz - desc,"NO_BIGPKT_COUNT:\t%u\n", NPU_COUNTER(Counter_Base, NO_BIGPKT_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"GET_BUFID_FOR_BIGPKT_FAIL:\t%u\n", NPU_COUNTER(Counter_Base, GET_BUFID_FOR_BIGPKT_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"BIGPKT_TO_HOSTADPT_ERROR_COUNT:\t%u\n", NPU_COUNTER(Counter_Base, BIGPKT_TO_HOSTADPT_ERROR_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_BIGPKT_SRAM_FULL_COUNT:\t%u\n", NPU_COUNTER(Counter_Base, ENQ_BIGPKT_SRAM_FULL_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_BIGPKT_SRAM_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base, ENQ_BIGPKT_SRAM_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT:\t\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT2:\t\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT2));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT_ISLAST:\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT_ISLAST));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT_IDX_FAIL:\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT_IDX_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT_CNT_FAIL:\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT_CNT_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT_FAIL:\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"CHECK_BIGPKT_OVER_RETRY:\t\t%u\n", NPU_COUNTER(Counter_Base, CHECK_BIGPKT_OVER_RETRY));
	desc += scnprintf(buff + desc, bufsz - desc,"BIGPKT_TO_HOSTAPD_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base,BIGPKT_TO_HOSTAPD_COUNT ));
	desc += scnprintf(buff + desc, bufsz - desc,"BIGPKT_DESC_FREE_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base,BIGPKT_DESC_FREE_COUNT ));
	desc += scnprintf(buff + desc, bufsz - desc,"------TDMA----------\n");
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_TXDESC_FULL_COUNT: \t\t%u\n", NPU_COUNTER(Counter_Base,TDMA_TXDESC_FULL_COUNT ));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_TXDESC_FULL_COUNT2: \t\t%u\n", NPU_COUNTER(Counter_Base,TDMA_TXDESC_FULL_COUNT2 ));


	desc += scnprintf(buff + desc, bufsz - desc,"------BA Counter----------\n");	
	desc += scnprintf(buff + desc, bufsz - desc,"BA1(in order):\t\t\t%u\nBA2(Dupl Packet):\t\t%u\nBA3(old packet):\t\t\t%u\nBA4(with in window):\t\t%u\nBA5(surpasses Win):\t\t%u\nBA_AMSDU:\t\t\t%u\n!AMPDU_COUNT:\t\t\t%u\n",
	NPU_COUNTER(Counter_Base, BA_IN_ORDER_PKT_COUNT),NPU_COUNTER(Counter_Base, BA_DUPL_PKT_COUNT), NPU_COUNTER(Counter_Base,BA_OLD_PKT_COUNT), NPU_COUNTER(Counter_Base,BA_WITHIN_WS_PKT_COUNT), 	
	NPU_COUNTER(Counter_Base, BA_POP_PKT_COUNT), NPU_COUNTER(Counter_Base, BA_AMSDU_COUNT),  NPU_COUNTER(Counter_Base, AMPDU_COUNT));
				//printk("BA_NO_PKT_IN_LIST_COUNT:\t%lu\n", NPU_COUNTER(Counter_Base, BA_NO_PKT_IN_LIST_COUNT));
				//printk("BA_NOT_DATA_PKT_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base, BA_NOT_DATA_PKT_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_NO_MEM_COUNT:\t\t\t%u\n", NPU_COUNTER(Counter_Base, BA_NO_MEM_COUNT));
				//printk("BA_NODE_ALLOC_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base, BA_NODE_ALLOC_COUNT));
				//printk("BA_NODE_ALLOC_FAIL:\t\t%lu\n", NPU_COUNTER(Counter_Base, BA_NODE_ALLOC_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_AMSDU_MISS:\t\t\t%u\n", NPU_COUNTER(Counter_Base, BA_AMSDU_MISS));
				//printk("BA_TIMEOUT_FLUSH:\t\t%lu\n", NPU_COUNTER(Counter_Base, BA_TIMEOUT_FLUSH));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_TIMEOUT_FLUSH100:\t\t%u\n", NPU_COUNTER(Counter_Base, BA_TIMEOUT_FLUSH100));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_TIMEOUT_FLUSH250:\t\t%u\n", NPU_COUNTER(Counter_Base, BA_TIMEOUT_FLUSH250));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_ENQ_DUP_SEQ:\t\t\t%u\n", NPU_COUNTER(Counter_Base, BA_ENQ_DUP_SEQ));	
	desc += scnprintf(buff + desc, bufsz - desc,"BA_REORDERING_NODE_ALLOC_COUNT\t%u\n", NPU_COUNTER(Counter_Base,BA_REORDERING_NODE_ALLOC_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_REORDERING_NODE_ALLOC_DRAM_COUNT\t%u\n", NPU_COUNTER(Counter_Base,BA_REORDERING_NODE_ALLOC_DRAM_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_REORDERING_NODE_FREE_COUNT\t%u\n", NPU_COUNTER(Counter_Base,BA_REORDERING_NODE_FREE_COUNT));	
	desc += scnprintf(buff + desc, bufsz - desc,"BA_REORDERINT_NODE_ALLOC_FAIL\t%u\n", NPU_COUNTER(Counter_Base,BA_REORDERINT_NODE_ALLOC_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_ENQ_QLEN_ERROR_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base,BA_ENQ_QLEN_ERROR_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_WCID_ERROR:\t\t\t%u\n", NPU_COUNTER(Counter_Base,BA_WCID_ERROR));
	desc += scnprintf(buff + desc, bufsz - desc,"BA_BAR_WCID_ERROR:\t\t\t%u\n", NPU_COUNTER(Counter_Base,BAR_WCID_ERROR));
				
	//printk("------TDMA return Counter----------\n");
				//printk("TDMA_RETURN_UNBIND_COUNT:\t%lu\n", NPU_COUNTER(Counter_Base, QDMA_RETURN_UNBIND_COUNT));
				//printk("TDMA_RETURN_FAIL_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base, QDMA_RETURN_FAIL_COUNT));
				//printk("TDMA_RETURN_OK_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base, QDMA_RETURN_OK_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"============ TEMP_DBG ==========\n");
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_PKT_TO_SLOWPATH_CASE1\t\t%u\n", NPU_COUNTER(Counter_Base,ENQ_PKT_TO_SLOWPATH_CASE1));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_PKT_TO_SLOWPATH_CASE2\t\t%u\n", NPU_COUNTER(Counter_Base,ENQ_PKT_TO_SLOWPATH_CASE2));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_PKT_TO_SLOWPATH_CASE3\t\t%u\n", NPU_COUNTER(Counter_Base,ENQ_PKT_TO_SLOWPATH_CASE3));
	desc += scnprintf(buff + desc, bufsz - desc,"ENQ_PIPELINE_PKT_CNT\t\t%u\n", NPU_COUNTER(Counter_Base,ENQ_PIPELINE_PKT_CNT));
	desc += scnprintf(buff + desc, bufsz - desc,"DEQ_PIPELINE_PKT_CNT\t\t%u\n", NPU_COUNTER(Counter_Base,DEQ_PIPELINE_PKT_CNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_TXRING_SEND\t\t%u\n", NPU_COUNTER(Counter_Base,TDMA_TXRING_SEND));
	//printk("BUFID_FREE_CASE1\t\t%lu\n", NPU_COUNTER(Counter_Base,BUFID_FREE_CASE1));

	desc += scnprintf(buff + desc, bufsz - desc,"========= RRO related ======== \n");
	desc += scnprintf(buff + desc, bufsz - desc,"0:MSDU_PG_2G_PKT 1:IND_CMD_WHILE\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_PG_2G_PKT));
	desc += scnprintf(buff + desc, bufsz - desc,"0:MSDU_PG_5G_PKT 1:IND_CMD_DESC\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_PG_5G_PKT));
	desc += scnprintf(buff + desc, bufsz - desc,"0:MSDU_PG_6G_PKT 1:IND_CMD_SIG_FAIL\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_PG_6G_PKT));
	desc += scnprintf(buff + desc, bufsz - desc,"0:MSDU_PG_READ_FAIL 1:OLD_DUP_PKT\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_PG_READ_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"0:RRO EXCEPT PKT 1:ALLOW_OLD_PN_CHK_PKT\t\t%u\n", 
	NPU_COUNTER(Counter_Base,BUFID_FREE_CASE1));
	desc += scnprintf(buff + desc, bufsz - desc,"0:PN_CHECK_FAIL 1:SP_TOKEN\t\t%u\n", NPU_COUNTER(Counter_Base,PN_CHECK_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"SKB_BUFID_STATE_ABNORMAL1\t\t%u\n", NPU_COUNTER(Counter_Base,SKB_BUFID_STATE_ABNORMAL1));
	desc += scnprintf(buff + desc, bufsz - desc,"SKB_BUFID_STATE_ABNORMAL2\t\t%u\n", NPU_COUNTER(Counter_Base,SKB_BUFID_STATE_ABNORMAL2));
	desc += scnprintf(buff + desc, bufsz - desc,"SKB_BUFID_STATE_ABNORMAL3\t\t%u\n", NPU_COUNTER(Counter_Base,SKB_BUFID_STATE_ABNORMAL3));

	Counter_Base = (unsigned long int *)counter_base[2];

	desc += scnprintf(buff + desc, bufsz - desc,"============ TDMA Counter =============\n");
	//printk("TDMA_TXDESC_NULL_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base,QDMA_TXDESC_NULL_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_UNBIND_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base,QDMA_UNBIND_COUNT));
				//printk("TDMA_TO_PPE_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base,QDMA_TO_PPE_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_DONE_COUNT:\t\t\t%u\n", NPU_COUNTER(Counter_Base,QDMA_DONE_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_FREE_BUFID_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base,QDMA_FREE_BUFID_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_TO_ENQ_FAIL_COUNT:\t\t%u\n", NPU_COUNTER(Counter_Base,QDMA_TO_ENQ_FAIL_COUNT));
				//printk("TDMA_TO_ENQ_FAIL_COUNT2:\t%lu\n", NPU_COUNTER(Counter_Base,QDMA_TO_ENQ_FAIL_COUNT2));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_TX_DSCP_IDX_INVALID:\t%u\n", NPU_COUNTER(Counter_Base,QDMA_TX_DSCP_IDX_INVALID));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_TX_DSCP_INFO_ERROR:\t\t%u\n", NPU_COUNTER(Counter_Base,QDMA_TX_DSCP_INFO_ERROR));
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_DONE_DROP_BIT_ERROR:\t%u\n", NPU_COUNTER(Counter_Base,QDMA_DONE_DROP_BIT_ERROR));
				//printk("TDMA_TXDESC_PUSH_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base,QDMA_TXDESC_PUSH_COUNT));
				//printk("TDMA_TXDESC_PUSH2_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base,QDMA_TXDESC_PUSH2_COUNT));
				//printk("TDMA_TXDESC_POP_COUNT:\t\t%lu\n", NPU_COUNTER(Counter_Base,QDMA_TXDESC_POP_COUNT));

	desc += scnprintf(buff + desc, bufsz - desc,"============ Bufid Counter ==========\n");
	desc += scnprintf(buff + desc, bufsz - desc,"SKB_ALLOC_BUFID_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,SKB_ALLOC_BUFID_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"SKB_FREE_BUFID_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,SKB_FREE_BUFID_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"SKB_BUFID_ALLOC_FAIL\t\t%u\n", NPU_COUNTER(Counter_Base,SKB_BUFID_ALLOC_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"BMGR0_BUFID_OVERFLOW\t\t%u\n", NPU_COUNTER(Counter_Base,BMGR0_BUFID_OVERFLOW));

	desc += scnprintf(buff + desc, bufsz - desc,"============ Hostadpt API Counter ==========\n");
	desc += scnprintf(buff + desc, bufsz - desc,"API_BUFF_SEND_BIGPKT\t\t%u\n", NPU_COUNTER(Counter_Base,API_BUFF_SEND_BIGPKT));
	desc += scnprintf(buff + desc, bufsz - desc,"API_BUFF_SEND_NOMALPKT\t\t%u\n", NPU_COUNTER(Counter_Base,API_BUFF_SEND_NOMALPKT));
	desc += scnprintf(buff + desc, bufsz - desc,"HOSTADPT_API_DONE_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,HOSTADPT_API_DONE_COUNT));

	desc += scnprintf(buff + desc, bufsz - desc,"======= RRO MSDU PG SKB counter =======\n");
	desc += scnprintf(buff + desc, bufsz - desc,"MSDU_SKB_ALLOC_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_SKB_ALLOC_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"MSDU_SKB_FREE_BUFID_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_SKB_FREE_BUFID_COUNT));	
	desc += scnprintf(buff + desc, bufsz - desc,"MSDU_SKB_BUFID_ALLOC_FAIL\t\t%u\n", NPU_COUNTER(Counter_Base,MSDU_SKB_BUFID_ALLOC_FAIL));
	ret = simple_read_from_buffer(user_buf, count, ppos, buff, desc);
	kfree(buff);
	return ret;
}		

static ssize_t
npu_tx_wifioffload_dbg_set(struct file *file, const char __user *user_buf,
			size_t count, loff_t *ppos)
{
	struct mt7996_phy *phy = file->private_data;
	struct mt7996_dev *dev = phy->dev;
	char buf[16];
	int ret = 0;
	u16 val;
	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	if (kstrtou16(buf, 0, &val))
		return -EINVAL;

	switch (val) {
	case 0:
	case 1:
		printk("[%s tx debug counter]\n",(val==0)?"2.4G/5G":"6G");
		npu_debug_band = val;
		if(counter_base[2] == 0)
		{
			npu_wifi_offload_get_dbg_counter_address(dev);
		}		
		break;
	default:
		printk("plz echo 0(2.4G) or 1(5G).\n");
		break;
	}

	return ret ? ret : count;
}

static ssize_t
npu_tx_wifioffload_dbg_get(struct file *file, char __user *user_buf,
			size_t count, loff_t *ppos){
	char *buff;
	int desc = 0;
	ssize_t ret;
	unsigned long int *Counter_Base;
	static const size_t bufsz = 1536;

	buff = kmalloc(bufsz, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;	
	
	if(counter_base[npu_debug_band]==0) 
		return 0;
	else Counter_Base = (unsigned long int *)
		counter_base[npu_debug_band];
	
	desc += scnprintf(buff + desc, bufsz - desc,"------WIFI TX (band%d)----------\n", npu_debug_band);
	desc += scnprintf(buff + desc, bufsz - desc,"TDMA_RX_GET_PKT: \t\t%u\n", NPU_COUNTER(Counter_Base,TDMA_RX_GET_PKT));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_PKT_FROM_HOSTADPT: \t\t%u\n", NPU_COUNTER(Counter_Base,TX_PKT_FROM_HOSTADPT));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_FULL_DESC: \t\t\t%u\n", NPU_COUNTER(Counter_Base,TX_FULL_DESC));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_DONE_GET_PKT: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_DONE_GET_PKT));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_SLOW_PATH_Q_FULL: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_SLOW_PATH_Q_FULL));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_SLOW_PATH_TXRING_FULL: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_SLOW_PATH_TXRING_FULL));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_SLOW_PATH_TOO_MUCH_PKT: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_SLOW_PATH_TOO_MUCH_PKT));
	//printk("TX_PKT_FROM_HOSTADP: \t\t%lu(band%d)\n", NPU_COUNTER(Counter_Base,TX_PKT_FROM_HOSTADP),band);
	Counter_Base = (unsigned long int *)counter_base[2];
	desc += scnprintf(buff + desc, bufsz - desc,"TX_SKB_ALLOC_BUFID_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,TX_SKB_ALLOC_BUFID_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_SKB_FREE_BUFID_COUNT\t\t%u\n", NPU_COUNTER(Counter_Base,TX_SKB_FREE_BUFID_COUNT));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_SKB_BUFID_ALLOC_FAIL\t\t%u\n", NPU_COUNTER(Counter_Base,TX_SKB_BUFID_ALLOC_FAIL));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_DONE_VER_ABNORMAL: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_DONE_VER_ABNORMAL));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_DONE_TOKEN_LEAK: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_DONE_TOKEN_LEAK));	
	desc += scnprintf(buff + desc, bufsz - desc,"TX_BUF_POOL_ABNORMAL_ALLOC: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_BUF_POOL_ABNORMAL_ALLOC));
	desc += scnprintf(buff + desc, bufsz - desc,"TX_BUF_POOL_ABNORMAL_FREE: \t\t%u\n", NPU_COUNTER(Counter_Base, TX_BUF_POOL_ABNORMAL_FREE));
	desc += scnprintf(buff + desc, bufsz - desc,"WIFI_TX_TOKEN_ABNORMAL: \t\t%u\n", NPU_COUNTER(Counter_Base, WIFI_TX_TOKEN_ABNORMAL));
	desc += scnprintf(buff + desc, bufsz - desc,"WIFI_TX_MSDU_PKT: \t\t%u\n", NPU_COUNTER(Counter_Base, WIFI_TX_MSDU_PKT));	
	ret = simple_read_from_buffer(user_buf, count, ppos, buff, desc);
	kfree(buff);
	return ret;	
}	

static const struct file_operations npu_wifioffload_dbg_counter = {
	.write = npu_wifioffload_dbg_set,
	.read = npu_wifioffload_dbg_get,
	.open = simple_open,
	.llseek = default_llseek,
};

static const struct file_operations npu_wifioffload_tx_dbg_counter = {
	.write = npu_tx_wifioffload_dbg_set,
	.read = npu_tx_wifioffload_dbg_get,
	.open = simple_open,
	.llseek = default_llseek,
};

int mt7996_init_debugfs(struct mt7996_dev *dev)
{
	struct dentry *dir;

	dir = mt76_register_debugfs_fops(&dev->mphy, NULL);
	if (!dir)
		return -ENOMEM;

	debugfs_create_file("hw-queues", 0400, dir, dev,
			    &mt7996_hw_queues_fops);
	debugfs_create_file("xmit-queues", 0400, dir, dev,
			    &mt7996_xmit_queues_fops);
	debugfs_create_file("tx_stats", 0400, dir, dev, &mt7996_tx_stats_fops);
	debugfs_create_file("sys_recovery", 0600, dir, dev,
			    &mt7996_sys_recovery_ops);
	debugfs_create_file("fw_debug_wm", 0600, dir, dev, &fops_fw_debug_wm);
	debugfs_create_file("fw_debug_wa", 0600, dir, dev, &fops_fw_debug_wa);
	debugfs_create_file("fw_debug_bin", 0600, dir, dev, &fops_fw_debug_bin);
	/* TODO: wm fw cpu utilization */
	debugfs_create_file("fw_util_wa", 0400, dir, dev,
			    &mt7996_fw_util_wa_fops);
	debugfs_create_file("implicit_txbf", 0600, dir, dev,
			    &fops_implicit_txbf);
	debugfs_create_devm_seqfile(dev->mt76.dev, "twt_stats", dir,
				    mt7996_twt_stats);
	debugfs_create_file("rf_regval", 0600, dir, dev, &fops_rf_regval);

	debugfs_create_u32("dfs_hw_pattern", 0400, dir, &dev->hw_pattern);
	debugfs_create_file("radar_trigger", 0200, dir, dev,
			    &fops_radar_trigger);
	debugfs_create_devm_seqfile(dev->mt76.dev, "rdd_monitor", dir,
				    mt7996_rdd_monitor);
	debugfs_create_devm_seqfile(dev->mt76.dev, "tr_info", dir, mt7996_trinfo_read);

	/*npu debug count*/
	debugfs_create_file("npu_dbg", 0600, dir, dev, &npu_wifioffload_dbg_counter);
	/*npu wifi tx count*/
	debugfs_create_file("npu_tx_dbg", 0600, dir, dev,
			    &npu_wifioffload_tx_dbg_counter);

	dev->debugfs_dir = dir;

	return 0;
}

static void
mt7996_debugfs_write_fwlog(struct mt7996_dev *dev, const void *hdr, int hdrlen,
			   const void *data, int len)
{
	static DEFINE_SPINLOCK(lock);
	unsigned long flags;
	void *dest;

	spin_lock_irqsave(&lock, flags);
	dest = relay_reserve(dev->relay_fwlog, hdrlen + len + 4);
	if (dest) {
		*(u32 *)dest = hdrlen + len;
		dest += 4;

		if (hdrlen) {
			memcpy(dest, hdr, hdrlen);
			dest += hdrlen;
		}

		memcpy(dest, data, len);
		relay_flush(dev->relay_fwlog);
	}
	spin_unlock_irqrestore(&lock, flags);
}

void mt7996_debugfs_rx_fw_monitor(struct mt7996_dev *dev, const void *data, int len)
{
	struct {
		__le32 magic;
		u8 version;
		u8 _rsv;
		__le16 serial_id;
		__le32 timestamp;
		__le16 msg_type;
		__le16 len;
	} hdr = {
		.version = 0x1,
		.magic = cpu_to_le32(FW_BIN_LOG_MAGIC),
		.msg_type = cpu_to_le16(PKT_TYPE_RX_FW_MONITOR),
	};

	if (!dev->relay_fwlog)
		return;

	hdr.serial_id = cpu_to_le16(dev->fw_debug_seq++);
	hdr.timestamp = cpu_to_le32(mt76_rr(dev, MT_LPON_FRCR(0)));
	hdr.len = *(__le16 *)data;
	mt7996_debugfs_write_fwlog(dev, &hdr, sizeof(hdr), data, len);
}

bool mt7996_debugfs_rx_log(struct mt7996_dev *dev, const void *data, int len)
{
	if (get_unaligned_le32(data) != FW_BIN_LOG_MAGIC)
		return false;

	if (dev->relay_fwlog)
		mt7996_debugfs_write_fwlog(dev, NULL, 0, data, len);

	return true;
}

#ifdef CONFIG_MAC80211_DEBUGFS
/** per-station debugfs **/

static int
mt7996_queues_show(struct seq_file *s, void *data)
{
	struct ieee80211_sta *sta = s->private;

	mt7996_sta_hw_queue_read(s, sta);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7996_queues);

void mt7996_sta_add_debugfs(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta, struct dentry *dir)
{
	debugfs_create_file("hw-queues", 0400, dir, sta, &mt7996_queues_fops);
}

static ssize_t mt7996_link_sta_fixed_rate_set(struct file *file,
					      const char __user *user_buf,
					      size_t count, loff_t *ppos)
{
#define SHORT_PREAMBLE 0
#define LONG_PREAMBLE 1
	struct ieee80211_link_sta *link_sta = file->private_data;
	struct mt7996_sta *msta = (struct mt7996_sta *)link_sta->sta->drv_priv;
	struct mt7996_dev *dev = msta->vif->deflink.phy->dev;
	struct mt7996_sta_link *msta_link;
	struct ra_rate phy = {};
	char buf[100];
	int ret;
	u16 gi, ltf;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	/* mode - cck: 0, ofdm: 1, ht: 2, gf: 3, vht: 4, he_su: 8, he_er: 9 EHT: 15
	 * bw - bw20: 0, bw40: 1, bw80: 2, bw160: 3, BW320: 4
	 * mcs - cck: 0~4, ofdm: 0~7, ht: 0~32, vht: 0~9, he_su: 0~11, he_er: 0~2, eht: 0~13
	 * nss - vht: 1~4, he: 1~4, eht: 1~4, others: ignore
	 * gi - (ht/vht) lgi: 0, sgi: 1; (he) 0.8us: 0, 1.6us: 1, 3.2us: 2
	 * preamble - short: 1, long: 0
	 * stbc - off: 0, on: 1
	 * ldpc - off: 0, on: 1
	 * spe - off: 0, on: 1
	 * ltf - 1xltf: 0, 2xltf: 1, 4xltf: 2
	 */
	if (sscanf(buf, "%hhu %hhu %hhu %hhu %hu %hhu %hhu %hhu %hhu %hu",
		   &phy.mode, &phy.bw, &phy.mcs, &phy.nss, &gi,
		   &phy.preamble, &phy.stbc, &phy.ldpc, &phy.spe, &ltf) != 10) {
		dev_warn(dev->mt76.dev,
			 "format: Mode BW MCS NSS GI Preamble STBC LDPC SPE ltf\n");
		return -EINVAL;
	}

	mutex_lock(&dev->mt76.mutex);

	msta_link = mt76_dereference(msta->link[link_sta->link_id], &dev->mt76);
	if (!msta_link) {
		ret = -EINVAL;
		goto out;
	}
	phy.wlan_idx = cpu_to_le16(msta_link->wcid.idx);
	phy.gi = cpu_to_le16(gi);
	phy.ltf = cpu_to_le16(ltf);
	phy.ldpc = phy.ldpc ? 7 : 0;
	phy.preamble = phy.preamble ? SHORT_PREAMBLE : LONG_PREAMBLE;

	ret = mt7996_mcu_set_fixed_rate_ctrl(dev, &phy, 0);
	if (ret)
		goto out;

	ret = count;
out:
	mutex_unlock(&dev->mt76.mutex);
	return ret;
}

static const struct file_operations fops_fixed_rate = {
	.write = mt7996_link_sta_fixed_rate_set,
	.open = simple_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

void mt7996_link_sta_add_debugfs(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
				 struct ieee80211_link_sta *link_sta,
				 struct dentry *dir)
{
	debugfs_create_file("fixed_rate", 0600, dir, link_sta, &fops_fixed_rate);
}

#endif
