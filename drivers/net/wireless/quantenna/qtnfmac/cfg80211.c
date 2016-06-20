/**
 * Copyright (c) 2015-2016 Quantenna Communications, Inc.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 **/

#include <linux/kernel.h>
#include <linux/etherdevice.h>
#include <linux/vmalloc.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/netlink.h>

#include "cfg80211.h"
#include "commands.h"
#include "core.h"
#include "util.h"
#include "bus.h"

/* Supported rates to be advertised to the cfg80211 */
static struct ieee80211_rate qtnf_rates[] = {
	{.bitrate = 10, .hw_value = 2, },
	{.bitrate = 20, .hw_value = 4, },
	{.bitrate = 55, .hw_value = 11, },
	{.bitrate = 110, .hw_value = 22, },
	{.bitrate = 60, .hw_value = 12, },
	{.bitrate = 90, .hw_value = 18, },
	{.bitrate = 120, .hw_value = 24, },
	{.bitrate = 180, .hw_value = 36, },
	{.bitrate = 240, .hw_value = 48, },
	{.bitrate = 360, .hw_value = 72, },
	{.bitrate = 480, .hw_value = 96, },
	{.bitrate = 540, .hw_value = 108, },
};

/* Channel definitions to be advertised to cfg80211 */
static struct ieee80211_channel qtnf_channels_2ghz[] = {
	{.center_freq = 2412, .hw_value = 1, },
	{.center_freq = 2417, .hw_value = 2, },
	{.center_freq = 2422, .hw_value = 3, },
	{.center_freq = 2427, .hw_value = 4, },
	{.center_freq = 2432, .hw_value = 5, },
	{.center_freq = 2437, .hw_value = 6, },
	{.center_freq = 2442, .hw_value = 7, },
	{.center_freq = 2447, .hw_value = 8, },
	{.center_freq = 2452, .hw_value = 9, },
	{.center_freq = 2457, .hw_value = 10, },
	{.center_freq = 2462, .hw_value = 11, },
	{.center_freq = 2467, .hw_value = 12, },
	{.center_freq = 2472, .hw_value = 13, },
	{.center_freq = 2484, .hw_value = 14, },
};

static struct ieee80211_supported_band qtnf_band_2ghz = {
	.channels = qtnf_channels_2ghz,
	.n_channels = ARRAY_SIZE(qtnf_channels_2ghz),
	.bitrates = qtnf_rates,
	.n_bitrates = ARRAY_SIZE(qtnf_rates),
};

static struct ieee80211_channel qtnf_channels_5ghz[] = {
	{.center_freq = 5040, .hw_value = 8, },
	{.center_freq = 5060, .hw_value = 12, },
	{.center_freq = 5080, .hw_value = 16, },
	{.center_freq = 5170, .hw_value = 34, },
	{.center_freq = 5190, .hw_value = 38, },
	{.center_freq = 5210, .hw_value = 42, },
	{.center_freq = 5230, .hw_value = 46, },
	{.center_freq = 5180, .hw_value = 36, },
	{.center_freq = 5200, .hw_value = 40, },
	{.center_freq = 5220, .hw_value = 44, },
	{.center_freq = 5240, .hw_value = 48, },
	{.center_freq = 5260, .hw_value = 52, },
	{.center_freq = 5280, .hw_value = 56, },
	{.center_freq = 5300, .hw_value = 60, },
	{.center_freq = 5320, .hw_value = 64, },
	{.center_freq = 5500, .hw_value = 100, },
	{.center_freq = 5520, .hw_value = 104, },
	{.center_freq = 5540, .hw_value = 108, },
	{.center_freq = 5560, .hw_value = 112, },
	{.center_freq = 5580, .hw_value = 116, },
	{.center_freq = 5600, .hw_value = 120, },
	{.center_freq = 5620, .hw_value = 124, },
	{.center_freq = 5640, .hw_value = 128, },
	{.center_freq = 5660, .hw_value = 132, },
	{.center_freq = 5680, .hw_value = 136, },
	{.center_freq = 5700, .hw_value = 140, },
	{.center_freq = 5745, .hw_value = 149, },
	{.center_freq = 5765, .hw_value = 153, },
	{.center_freq = 5785, .hw_value = 157, },
	{.center_freq = 5805, .hw_value = 161, },
	{.center_freq = 5825, .hw_value = 165, },
};

static struct ieee80211_supported_band qtnf_band_5ghz = {
	.channels = qtnf_channels_5ghz,
	.n_channels = ARRAY_SIZE(qtnf_channels_5ghz),
	.bitrates = qtnf_rates + 4,
	.n_bitrates = ARRAY_SIZE(qtnf_rates) - 4,
};

/* Supported crypto cipher suits to be advertised to cfg80211 */
static const u32 qtnf_cipher_suites[] = {
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_AES_CMAC,
};

/* Supported mgmt frame types to be advertised to cfg80211 */
static const struct ieee80211_txrx_stypes
qtnf_mgmt_stypes[NUM_NL80211_IFTYPES] = {
	[NL80211_IFTYPE_STATION] = {
		.tx = BIT(IEEE80211_STYPE_ACTION >> 4),
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		      BIT(IEEE80211_STYPE_PROBE_REQ >> 4),
	},
	[NL80211_IFTYPE_AP] = {
		.tx = BIT(IEEE80211_STYPE_ACTION >> 4),
		.rx = BIT(IEEE80211_STYPE_ACTION >> 4) |
		      BIT(IEEE80211_STYPE_PROBE_REQ >> 4),
	},
};

static int
qtnf_change_virtual_intf(struct wiphy *wiphy,
			 struct net_device *dev,
			 enum nl80211_iftype type, u32 *flags,
			 struct vif_params *params)
{
	struct qtnf_vif *vif;
	u8 *mac_addr;

	vif = qtnf_netdev_get_priv(dev);

	if (params)
		mac_addr = params->macaddr;
	else
		mac_addr = NULL;

	if (qtnf_cmd_send_change_intf_type(vif, type, mac_addr)) {
		pr_err("%s: failed to change interface type\n", __func__);
		return -EFAULT;
	}

	vif->wdev.iftype = type;
	return 0;
}

int qtnf_del_virtual_intf(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct net_device *netdev =  wdev->netdev;
	struct qtnf_vif *vif;

	if (WARN_ON(!netdev)) {
		pr_err("could not get netdev for wdev\n");
		return -EFAULT;
	}

	vif = qtnf_netdev_get_priv(wdev->netdev);

	if (qtnf_cmd_send_del_intf(vif))
		pr_err("%s: failed to send del_intf command\n", __func__);

	/* Stop data */
	netif_tx_stop_all_queues(netdev);
	if (netif_carrier_ok(netdev))
		netif_carrier_off(netdev);

	if (netdev->reg_state == NETREG_REGISTERED)
		unregister_netdevice(netdev);

	/* Clear the vif in mac */
	vif->netdev->ieee80211_ptr = NULL;
	vif->netdev = NULL;
	vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
	eth_zero_addr(vif->mac_addr);

	return 0;
}

struct wireless_dev *qtnf_add_virtual_intf(struct wiphy *wiphy,
					   const char *name,
					   unsigned char name_assign_type,
					   enum nl80211_iftype type,
					   u32 *flags,
					   struct vif_params *params)
{
	struct qtnf_wmac *mac;
	struct qtnf_vif *vif;
	u8 *mac_addr = NULL;

	mac = wiphy_priv(wiphy);

	if (!mac)
		return ERR_PTR(-EFAULT);

	switch (type) {
	case NL80211_IFTYPE_STATION:
	case NL80211_IFTYPE_AP:
		vif = qtnf_get_free_vif(mac);
		if (!vif) {
			pr_err("qtnfmac: %s: could not get free private structure\n",
			       __func__);
			return ERR_PTR(-EFAULT);
		}

		eth_zero_addr(vif->mac_addr);
		vif->bss_priority = QTNF_DEF_BSS_PRIORITY;
		vif->wdev.wiphy = wiphy;
		vif->wdev.iftype = type;
		vif->sta_state = QTNF_STA_DISCONNECTED;
		break;
	default:
		pr_err("qtnfmac: %s: unsupported virtual interface type (%d)\n",
		       __func__, type);
		return ERR_PTR(-ENOTSUPP);
	}

	if (params)
		mac_addr = params->macaddr;

	if (qtnf_cmd_send_add_intf(vif, type, mac_addr)) {
		vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		pr_err("%s: failed to send add_intf command\n", __func__);
		return ERR_PTR(-EFAULT);
	}

	if (!is_valid_ether_addr(vif->mac_addr)) {
		vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		pr_err("%s: invalid MAC address from FW EP for add_intf\n",
		       __func__);
		return ERR_PTR(-EFAULT);
	}

	if (qtnf_net_attach(mac, vif, name, name_assign_type, type)) {
		pr_err("could not attach netdev\n");
		vif->netdev = NULL;
		vif->wdev.iftype = NL80211_IFTYPE_UNSPECIFIED;
		return ERR_PTR(-EFAULT);
	}

	vif->wdev.netdev = vif->netdev;
	return &vif->wdev;
}

/* concatenate all the beacon IEs into one buffer
 * Take IEs from head, tail and beacon_ies fields of cfg80211_beacon_data
 * and append it to provided buffer.
 * Checks total IE buf length to be <= than IEEE80211_MAX_DATA_LEN.
 * Checks IE buffers to be valid, so that resulting buffer
 * should be a valid IE buffer with length <= IEEE80211_MAX_DATA_LEN.
 */
static int qtnf_get_beacon_ie(const struct cfg80211_beacon_data *info,
			      uint8_t *buf)
{
	const struct ieee80211_mgmt *frame = (void *)info->head;
	const size_t head_tlv_offset = offsetof(struct ieee80211_mgmt,
						u.beacon.variable);
	const size_t head_tlv_len = (info->head_len > head_tlv_offset) ?
				     info->head_len - head_tlv_offset : 0;
	size_t pos = 0;

	if (frame && head_tlv_len) {
		if (pos + head_tlv_len > IEEE80211_MAX_DATA_LEN) {
			pr_warn("%s: too large beacon head IEs: %zu\n",
				__func__, pos + head_tlv_len);
			return -E2BIG;
		} else if (qtnf_ieee80211_check_ie_buf(frame->u.beacon.variable,
						       head_tlv_len)) {
			memcpy(buf, frame->u.beacon.variable, head_tlv_len);
			pos += head_tlv_len;
			buf += head_tlv_len;
		} else {
			pr_warn("%s: invalid head IE buf\n", __func__);
			return -EINVAL;
		}
	}

	if (info->tail && info->tail_len) {
		if (pos + info->tail_len > IEEE80211_MAX_DATA_LEN) {
			pr_warn("%s: too large beacon tail IEs: %zu\n",
				__func__, pos + info->tail_len);
			return -E2BIG;
		} else if (qtnf_ieee80211_check_ie_buf(info->tail,
						       info->tail_len)) {
			memcpy(buf, info->tail, info->tail_len);
			pos += info->tail_len;
			buf += info->tail_len;
		} else {
			pr_warn("%s: invalid tail IE buf\n", __func__);
			return -EINVAL;
		}
	}

	if (info->beacon_ies && info->beacon_ies_len) {
		if (pos + info->beacon_ies_len > IEEE80211_MAX_DATA_LEN) {
			pr_warn("%s: too large beacon extra IEs: %zu\n",
				__func__, pos + info->beacon_ies_len);
			return -E2BIG;
		} else if (qtnf_ieee80211_check_ie_buf(info->beacon_ies,
						       info->beacon_ies_len)) {
			memcpy(buf, info->beacon_ies, info->beacon_ies_len);
			pos += info->beacon_ies_len;
			buf += info->beacon_ies_len;
		} else {
			pr_warn("%s: invalid beacon_ies IE buf\n", __func__);
			return -EINVAL;
		}
	}

	return pos;
}

static int qtnf_mgmt_set_appie(struct qtnf_vif *vif,
			       const struct cfg80211_beacon_data *info)
{
	u8 *beacon_ies;
	int beacon_ies_len;
	int ret = 0;

	beacon_ies = kmalloc(IEEE80211_MAX_DATA_LEN, GFP_KERNEL);

	if (unlikely(!beacon_ies))
		return -ENOMEM;

	beacon_ies_len = qtnf_get_beacon_ie(info, beacon_ies);

	if (unlikely(beacon_ies_len < 0)) {
		ret = beacon_ies_len;
		goto out;
	}

	ret = qtnf_cmd_send_mgmt_set_appie(vif, QLINK_MGMT_FRAME_BEACON,
					   beacon_ies, beacon_ies_len);

	if (ret)
		goto out;

	if (!info->proberesp_ies || !info->proberesp_ies_len) {
		ret = qtnf_cmd_send_mgmt_set_appie(vif,
						   QLINK_MGMT_FRAME_PROBE_RESP,
						   NULL, 0);
	} else if (qtnf_ieee80211_check_ie_buf(info->proberesp_ies,
					       info->proberesp_ies_len)) {
		ret = qtnf_cmd_send_mgmt_set_appie(vif,
						   QLINK_MGMT_FRAME_PROBE_RESP,
						   info->proberesp_ies,
						   info->proberesp_ies_len);
	} else {
		pr_err("%s: proberesp_ies is not a valid IE buffer\n",
		       __func__);
		ret = -EINVAL;
	}

	if (ret)
		goto out;

	if (!info->assocresp_ies || !info->assocresp_ies_len) {
		ret = qtnf_cmd_send_mgmt_set_appie(vif,
						   QLINK_MGMT_FRAME_ASSOC_RESP,
						   NULL, 0);
	} else if (qtnf_ieee80211_check_ie_buf(info->assocresp_ies,
					       info->assocresp_ies_len)) {
		ret = qtnf_cmd_send_mgmt_set_appie(vif,
						   QLINK_MGMT_FRAME_ASSOC_RESP,
						   info->assocresp_ies,
						   info->assocresp_ies_len);
	} else {
		pr_err("%s: assocresp_ies is not a valid IE buffer\n",
		       __func__);
		ret = -EINVAL;
	}

out:
	kfree(beacon_ies);
	return ret;
}

static int qtnf_change_beacon(struct wiphy *wiphy, struct net_device *dev,
			      struct cfg80211_beacon_data *info)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	if (!(vif->bss_status & QTNF_STATE_AP_START)) {
		pr_err("%s: bss not started\n", __func__);
		return -EFAULT;
	}

	return qtnf_mgmt_set_appie(vif, info);
}

static int qtnf_start_ap(struct wiphy *wiphy, struct net_device *dev,
			 struct cfg80211_ap_settings *settings)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);
	struct qtnf_bss_config *bss_cfg;

	bss_cfg = &vif->bss_cfg;

	memset(bss_cfg, 0, sizeof(*bss_cfg));

	bss_cfg->bcn_period = settings->beacon_interval;
	bss_cfg->dtim = settings->dtim_period;
	bss_cfg->auth_type = settings->auth_type;
	bss_cfg->privacy = settings->privacy;

	bss_cfg->ssid_len = settings->ssid_len;
	memcpy(&bss_cfg->ssid, settings->ssid, bss_cfg->ssid_len);

	memcpy(&bss_cfg->chandef, &settings->chandef,
	       sizeof(struct cfg80211_chan_def));
	memcpy(&bss_cfg->crypto, &settings->crypto,
	       sizeof(struct cfg80211_crypto_settings));

	if (qtnf_cmd_send_config_ap(vif)) {
		pr_err("failed to download AP configuration\n");
		return -EFAULT;
	}

	if (!(vif->bss_status & QTNF_STATE_AP_CONFIG)) {
		pr_err("failed to configure AP settings in FW\n");
		return -EFAULT;
	}

	/* update beacon extra IEs */
	if (qtnf_mgmt_set_appie(vif, &settings->beacon)) {
		pr_err("failed to setup mgmt frames IEs in FW\n");
		return -EFAULT;
	}

	if (qtnf_cmd_send_start_ap(vif)) {
		pr_err("failed to issue start AP command\n");
		return -EFAULT;
	}

	if (!(vif->bss_status & QTNF_STATE_AP_START)) {
		pr_err("failed to start AP operations in FW\n");
		return -EFAULT;
	}

	return 0;
}

static int qtnf_stop_ap(struct wiphy *wiphy, struct net_device *dev)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	if (qtnf_cmd_send_stop_ap(vif)) {
		pr_err("failed to stop AP operation in FW\n");
		vif->bss_status &= ~QTNF_STATE_AP_START;
		vif->bss_status &= ~QTNF_STATE_AP_CONFIG;

		netif_carrier_off(vif->netdev);
		return -EFAULT;
	}
	return 0;
}

static int qtnf_set_wiphy_params(struct wiphy *wiphy, u32 changed)
{
	struct qtnf_wmac *mac = wiphy_priv(wiphy);
	struct qtnf_vif *vif;
	int ret;

	vif = qtnf_get_base_vif(mac);
	if (!vif) {
		pr_err("core_attach: could not get valid vif pointer\n");
		return -EFAULT;
	}

	if (changed & (WIPHY_PARAM_RETRY_LONG | WIPHY_PARAM_RETRY_SHORT)) {
		pr_err("device doesn't support modifing retry parameters\n");
		return -EOPNOTSUPP;
	}

	ret = qtnf_cmd_send_update_phy_params(mac, QLINK_CMD_ACTION_SET,
					      changed);

	if (ret)
		pr_warn("failed to configure phy thresholds\n");

	return ret;
}

static void
qtnf_mgmt_frame_register(struct wiphy *wiphy, struct wireless_dev *wdev,
			 u16 frame_type, bool reg)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(wdev->netdev);
	u16 mgmt_type;
	u16 new_mask;
	u16 qlink_frame_type = 0;

	mgmt_type = (frame_type & IEEE80211_FCTL_STYPE) >> 4;

	if (reg)
		new_mask = vif->mgmt_frames_bitmask | BIT(mgmt_type);
	else
		new_mask = vif->mgmt_frames_bitmask & ~BIT(mgmt_type);

	if (new_mask == vif->mgmt_frames_bitmask)
		return;

	switch (frame_type & IEEE80211_FCTL_STYPE) {
	case IEEE80211_STYPE_PROBE_REQ:
		qlink_frame_type = QLINK_MGMT_FRAME_PROBE_REQ;
		break;
	case IEEE80211_STYPE_ACTION:
		qlink_frame_type = QLINK_MGMT_FRAME_ACTION;
		break;
	default:
		pr_warn("%s: unsupported frame type: %X\n", __func__,
			(frame_type & IEEE80211_FCTL_STYPE) >> 4);
		return;
	}

	if (qtnf_cmd_send_register_mgmt(vif, qlink_frame_type, reg)) {
		pr_warn("%s: failed to %sregistered mgmt frame type 0x%x\n",
			__func__, reg ? "" : "un", frame_type);
		return;
	}

	vif->mgmt_frames_bitmask = new_mask;
	pr_info("%s: %sregistered mgmt frame type 0x%x\n", __func__,
		reg ? "" : "un", frame_type);
}

static int
qtnf_mgmt_tx(struct wiphy *wiphy, struct wireless_dev *wdev,
	     struct cfg80211_mgmt_tx_params *params, u64 *cookie)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(wdev->netdev);
	const struct ieee80211_mgmt *mgmt_frame = (void *)params->buf;
	u32 short_cookie = prandom_u32();
	u16 flags = 0;

	*cookie = short_cookie;

	if (params->offchan)
		flags |= QLINK_MGMT_FRAME_TX_FLAG_OFFCHAN;

	if (params->no_cck)
		flags |= QLINK_MGMT_FRAME_TX_FLAG_NO_CCK;

	if (params->dont_wait_for_ack)
		flags |= QLINK_MGMT_FRAME_TX_FLAG_ACK_NOWAIT;

	pr_debug("%s: %s freq:%u; FC:%.4X; DA:%pM; len:%zu; C:%.8X; FL:%.4X\n",
		 __func__, wdev->netdev->name, params->chan->center_freq,
		 le16_to_cpu(mgmt_frame->frame_control), mgmt_frame->da,
		 params->len, short_cookie, flags);

	return qtnf_cmd_send_mgmt_frame(vif, short_cookie, flags,
					params->chan->center_freq,
					params->buf, params->len);
}

static int
qtnf_get_station(struct wiphy *wiphy, struct net_device *dev,
		 const u8 *mac, struct station_info *sinfo)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	return qtnf_cmd_get_sta_info(vif, mac, sinfo);
}

static int
qtnf_dump_station(struct wiphy *wiphy, struct net_device *dev,
		  int idx, u8 *mac, struct station_info *sinfo)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);
	const struct qtnf_sta_node *sta_node;
	int ret;

	sta_node = qtnf_sta_list_lookup_index(&vif->sta_list, idx);

	if (unlikely(!sta_node))
		return -ENOENT;

	ether_addr_copy(mac, sta_node->mac_addr);

	ret = qtnf_cmd_get_sta_info(vif, sta_node->mac_addr, sinfo);

	if (unlikely(ret == -ENOENT)) {
		sinfo->filled = 0;
		ret = 0;
	}

	return ret;
}

static int qtnf_add_key(struct wiphy *wiphy, struct net_device *dev,
			u8 key_index, bool pairwise, const u8 *mac_addr,
			struct key_params *params)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	pr_info("QTNF: %s cipher=%x, idx=%u, pairwise=%u\n", __func__,
		params->cipher, key_index, pairwise);
	if (qtnf_cmd_send_add_key(vif, key_index, pairwise, mac_addr,
				  params)) {
		pr_err("QTNF: failed to add key\n");
		return -EFAULT;
	}
	return 0;
}

static int qtnf_del_key(struct wiphy *wiphy, struct net_device *dev,
			u8 key_index, bool pairwise, const u8 *mac_addr)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	pr_info("QTNF: %s idx=%u, pairwise=%u\n", __func__, key_index,
		pairwise);
	if (qtnf_cmd_send_del_key(vif, key_index, pairwise, mac_addr)) {
		pr_err("QTNF: failed to delete key\n");
		return -EFAULT;
	}
	return 0;
}

static int qtnf_set_default_key(struct wiphy *wiphy, struct net_device *dev,
				u8 key_index, bool unicast, bool multicast)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	pr_info("QTNF: %s idx=%u, unicast=%u, multicast=%u\n", __func__,
		key_index, unicast, multicast);
	if (qtnf_cmd_send_set_default_key(vif, key_index, unicast,
					  multicast)) {
		pr_err("QTNF: failed to set default key\n");
		return -EFAULT;
	}
	return 0;
}

static int
qtnf_set_default_mgmt_key(struct wiphy *wiphy, struct net_device *dev,
			  u8 key_index)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	pr_info("QTNF: %s idx=%u\n", __func__, key_index);
	if (qtnf_cmd_send_set_default_mgmt_key(vif, key_index)) {
		pr_err("QTNF: failed to set default mgmt key\n");
		return -EFAULT;
	}
	return 0;
}

static int
qtnf_change_station(struct wiphy *wiphy, struct net_device *dev,
		    const u8 *mac, struct station_parameters *params)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	if (qtnf_cmd_send_change_sta(vif, mac, params)) {
		pr_err("QTNF: failed to change STA\n");
		return -EFAULT;
	}
	return 0;
}

static int
qtnf_del_station(struct wiphy *wiphy, struct net_device *dev,
		 struct station_del_parameters *params)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(dev);

	if (params->mac &&
	    (vif->wdev.iftype == NL80211_IFTYPE_AP) &&
	    !is_broadcast_ether_addr(params->mac) &&
	    !qtnf_sta_list_lookup(&vif->sta_list, params->mac))
		return 0;
	if (qtnf_cmd_send_del_sta(vif, params)) {
		pr_err("QTNF: failed to delete STA\n");
		return -EFAULT;
	}
	return 0;
}

static int
qtnf_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct qtnf_wmac *mac = wiphy_priv(wiphy);

	mac->scan_req = request;

	if (qtnf_cmd_send_scan(mac)) {
		pr_err("QTNF: failed to start scan\n");
		return -EFAULT;
	}
	return 0;
}

static int
qtnf_connect(struct wiphy *wiphy, struct net_device *dev,
	     struct cfg80211_connect_params *sme)
{
	struct qtnf_vif *vif;
	struct qtnf_bss_config *bss_cfg;

	vif = qtnf_netdev_get_priv(dev);
	if (!vif) {
		pr_err("core_attach: could not get valid vif pointer\n");
		return -EFAULT;
	}
	if (vif->wdev.iftype != NL80211_IFTYPE_STATION) {
		pr_err("can't connect when not in STA mode\n");
		return -EOPNOTSUPP;
	}

	if (vif->sta_state != QTNF_STA_DISCONNECTED)
		return -EBUSY;

	bss_cfg = &vif->bss_cfg;
	memset(bss_cfg, 0, sizeof(*bss_cfg));

	bss_cfg->ssid_len = sme->ssid_len;
	memcpy(&bss_cfg->ssid, sme->ssid, bss_cfg->ssid_len);
	bss_cfg->chandef.chan = sme->channel;
	bss_cfg->auth_type = sme->auth_type;
	bss_cfg->privacy = sme->privacy;
	bss_cfg->mfp = sme->mfp;
	if ((sme->bg_scan_period > 0) &&
	    (sme->bg_scan_period <= QTNF_MAX_BG_SCAN_PERIOD))
		bss_cfg->bg_scan_period = sme->bg_scan_period;
	else if (sme->bg_scan_period == -1)
		bss_cfg->bg_scan_period = QTNF_DEFAULT_BG_SCAN_PERIOD;
	else
		bss_cfg->bg_scan_period = 0; /* disabled */
	bss_cfg->connect_flags = 0;
	if (sme->flags & ASSOC_REQ_DISABLE_HT)
		bss_cfg->connect_flags |= QLINK_STA_CONNECT_DISABLE_HT;
	if (sme->flags & ASSOC_REQ_DISABLE_VHT)
		bss_cfg->connect_flags |= QLINK_STA_CONNECT_DISABLE_VHT;
	if (sme->flags & ASSOC_REQ_USE_RRM)
		bss_cfg->connect_flags |= QLINK_STA_CONNECT_USE_RRM;
	memcpy(&bss_cfg->crypto, &sme->crypto, sizeof(bss_cfg->crypto));
	if (sme->bssid)
		ether_addr_copy(bss_cfg->bssid, sme->bssid);
	else
		eth_zero_addr(bss_cfg->bssid);

	if (qtnf_cmd_send_connect(vif, sme)) {
		pr_err("QTNF: failed to connect\n");
		return -EFAULT;
	}

	vif->sta_state = QTNF_STA_CONNECTING;
	return 0;
}

static int
qtnf_disconnect(struct wiphy *wiphy, struct net_device *dev,
		u16 reason_code)
{
	struct qtnf_wmac *mac = wiphy_priv(wiphy);
	struct qtnf_vif *vif;

	vif = qtnf_get_base_vif(mac);
	if (!vif) {
		pr_err("core_attach: could not get valid vif pointer\n");
		return -EFAULT;
	}

	if (vif->wdev.iftype != NL80211_IFTYPE_STATION) {
		pr_err("can't disconnect when not in STA mode\n");
		return -EOPNOTSUPP;
	}

	if (vif->sta_state == QTNF_STA_DISCONNECTED)
		return 0;

	if (qtnf_cmd_send_disconnect(vif, reason_code)) {
		pr_err("QTNF: failed to disconnect\n");
		return -EFAULT;
	}

	vif->sta_state = QTNF_STA_DISCONNECTED;
	return 0;
}

static struct cfg80211_ops qtn_cfg80211_ops = {
	.add_virtual_intf	= qtnf_add_virtual_intf,
	.change_virtual_intf	= qtnf_change_virtual_intf,
	.del_virtual_intf	= qtnf_del_virtual_intf,
	.start_ap		= qtnf_start_ap,
	.change_beacon		= qtnf_change_beacon,
	.stop_ap		= qtnf_stop_ap,
	.set_wiphy_params	= qtnf_set_wiphy_params,
	.mgmt_frame_register	= qtnf_mgmt_frame_register,
	.mgmt_tx		= qtnf_mgmt_tx,
	.change_station		= qtnf_change_station,
	.del_station		= qtnf_del_station,
	.get_station		= qtnf_get_station,
	.dump_station		= qtnf_dump_station,
	.add_key		= qtnf_add_key,
	.del_key		= qtnf_del_key,
	.set_default_key	= qtnf_set_default_key,
	.set_default_mgmt_key	= qtnf_set_default_mgmt_key,
	.scan			= qtnf_scan,
	.connect		= qtnf_connect,
	.disconnect		= qtnf_disconnect
};

static void qtnf_cfg80211_reg_notifier(struct wiphy *wiphy,
				       struct regulatory_request *req)
{
	struct qtnf_wmac *mac = wiphy_priv(wiphy);
	struct qtnf_bus *bus;
	struct qtnf_vif *vif;
	struct qtnf_wmac *chan_mac;
	int i;

	bus = mac->bus;

	pr_info("%s: initiator=%d, alpha=%c%c, macid=%d\n", __func__,
		req->initiator, req->alpha2[0], req->alpha2[1], mac->macid);

	vif = qtnf_get_base_vif(mac);
	if (!vif) {
		pr_err("%s: could not get valid vif pointer\n", __func__);
		return;
	}
	/* ignore non-ISO3166 country codes */
	for (i = 0; i < sizeof(req->alpha2); i++) {
		if (req->alpha2[i] < 'A' || req->alpha2[i] > 'Z') {
			pr_err("not a ISO3166 code\n");
			return;
		}
	}
	if (!strncasecmp(req->alpha2, bus->hw_info.country_code,
			 sizeof(req->alpha2))) {
		pr_warn("unchanged country code\n");
		return;
	}

	if (qtnf_cmd_send_regulatory_config(mac, QLINK_CMD_ACTION_SET,
					    req->alpha2)) {
		pr_err("failed to download regulatory configuration\n");
		return;
	}

	for (i = 0; i < bus->hw_info.num_mac; i++) {
		chan_mac = bus->mac[i];
		if (!chan_mac || !chan_mac->mac_started)
			continue;
		if (!(bus->hw_info.mac_bitmap & BIT(i)))
			continue;
		if (qtnf_cmd_get_mac_chan_info(chan_mac)) {
			pr_err("reg_notifier: could not get channel information for mac%d\n",
			       chan_mac->macid);
			pr_err("cannot continue without valid channel information from EP");
			qtnf_core_detach(bus);
			return;
		}
	}
}

static void
qtnf_setup_htvht_caps(struct qtnf_wmac *mac, struct wiphy *wiphy)
{
	struct ieee80211_supported_band *band;
	struct ieee80211_sta_ht_cap *ht_cap;
	struct ieee80211_sta_vht_cap *vht_cap;
	int i;

	for (i = 0; i <= NL80211_BAND_5GHZ; i++) {
		band = wiphy->bands[i];
		if (!band)
			continue;

		ht_cap = &band->ht_cap;
		ht_cap->ht_supported = true;
		memcpy(&ht_cap->cap, &mac->macinfo.ht_cap.cap_info,
		       sizeof(u16));
		ht_cap->ampdu_factor = IEEE80211_HT_MAX_AMPDU_64K;
		ht_cap->ampdu_density = IEEE80211_HT_MPDU_DENSITY_NONE;
		memcpy(&ht_cap->mcs, &mac->macinfo.ht_cap.mcs,
		       sizeof(ht_cap->mcs));

		if (mac->macinfo.phymode & QLINK_PHYMODE_AC) {
			vht_cap = &band->vht_cap;
			vht_cap->vht_supported = true;
			memcpy(&vht_cap->cap,
			       &mac->macinfo.vht_cap.vht_cap_info, sizeof(u32));
			/* Update MCS support for VHT */
			memcpy(&vht_cap->vht_mcs,
			       &mac->macinfo.vht_cap.supp_mcs,
			       sizeof(struct ieee80211_vht_mcs_info));
		}
	}
}

struct wiphy *qtnf_allocate_wiphy(struct qtnf_bus *bus)
{
	struct wiphy *wiphy;

	wiphy = wiphy_new(&qtn_cfg80211_ops, sizeof(struct qtnf_wmac));
	if (!wiphy) {
		pr_err("could not create new wiphy\n");
		return NULL;
	}

	set_wiphy_dev(wiphy, bus->dev);

	return wiphy;
}

static int qtnf_wiphy_setup_if_comb(struct wiphy *wiphy,
				    struct ieee80211_iface_combination *if_comb,
				    const struct qtnf_mac_info *mac_info)
{
	size_t max_interfaces = 0;
	u16 interface_modes = 0;
	size_t i;

	if (unlikely(!mac_info->limits || !mac_info->n_limits)) {
		pr_err("%s: no interface types supported\n", __func__);
		return -ENOENT;
	}

	if_comb->limits = mac_info->limits;
	if_comb->n_limits = mac_info->n_limits;

	for (i = 0; i < mac_info->n_limits; i++) {
		max_interfaces += mac_info->limits[i].max;
		interface_modes |= mac_info->limits[i].types;
	}

	if_comb->num_different_channels = 1;
	if_comb->beacon_int_infra_match = true;
	if_comb->max_interfaces = max_interfaces;
	if_comb->radar_detect_widths = mac_info->radar_detect_widths;
	wiphy->interface_modes = interface_modes;

	pr_info("%s: MAX_IF: %zu; MODES: %.4X; RADAR WIDTHS: %.2X\n", __func__,
		max_interfaces, interface_modes, if_comb->radar_detect_widths);

	return 0;
}

int qtnf_register_wiphy(struct qtnf_bus *bus, struct qtnf_wmac *mac)
{
	struct wiphy *wiphy = priv_to_wiphy(mac);
	struct ieee80211_iface_combination *iface_comb = NULL;
	int ret;

	if (!wiphy) {
		pr_err("%s:invalid wiphy pointer\n", __func__);
		return -EFAULT;
	}

	if (!(mac->macinfo.phymode & (QLINK_PHYMODE_BGN | QLINK_PHYMODE_AN))) {
		pr_err("%s:invalid phymode reported by FW\n", __func__);
		ret = -EFAULT;
		goto out;
	}

	iface_comb = kzalloc(sizeof(*iface_comb), GFP_KERNEL);
	if (!iface_comb) {
		ret = -ENOMEM;
		goto out;
	}

	ret = qtnf_wiphy_setup_if_comb(wiphy, iface_comb, &mac->macinfo);
	if (ret)
		goto out;

	pr_info("macid=%d, phymode=%#x\n", mac->macid, mac->macinfo.phymode);
	if (mac->macinfo.phymode & QLINK_PHYMODE_BGN) {
		if (!(bus->hw_info.hw_capab & QLINK_HW_SUPPORTS_REG_UPDATE)) {
			/* This means driver should use channel info from EP */
			qtnf_band_2ghz.n_channels = mac->macinfo.n_channels;
			qtnf_band_2ghz.channels = mac->macinfo.channels;
		}
		wiphy->bands[NL80211_BAND_2GHZ] = &qtnf_band_2ghz;
	}
	if (mac->macinfo.phymode & QLINK_PHYMODE_AN) {
		if (!(bus->hw_info.hw_capab & QLINK_HW_SUPPORTS_REG_UPDATE)) {
			/* This means driver should use channel info from EP */
			qtnf_band_5ghz.n_channels = mac->macinfo.n_channels;
			qtnf_band_5ghz.channels = mac->macinfo.channels;
		}
		wiphy->bands[NL80211_BAND_5GHZ] = &qtnf_band_5ghz;
	}
	qtnf_setup_htvht_caps(mac, wiphy);

	wiphy->frag_threshold = mac->macinfo.frag_thr;
	wiphy->rts_threshold = mac->macinfo.rts_thr;
	wiphy->retry_short = mac->macinfo.sretry_limit;
	wiphy->retry_long = mac->macinfo.lretry_limit;
	wiphy->coverage_class = mac->macinfo.coverage_class;

	wiphy->max_scan_ssids = QTNF_MAX_SSID_LIST_LENGTH;
	wiphy->max_scan_ie_len = QTNF_MAX_VSIE_LEN;
	wiphy->mgmt_stypes = qtnf_mgmt_stypes;
	wiphy->max_remain_on_channel_duration = 5000;

	wiphy->iface_combinations = iface_comb;
	wiphy->n_iface_combinations = 1;

	/* Initialize cipher suits */
	wiphy->cipher_suites = qtnf_cipher_suites;
	wiphy->n_cipher_suites = ARRAY_SIZE(qtnf_cipher_suites);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME |
			WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD |
			WIPHY_FLAG_AP_UAPSD;

	wiphy->probe_resp_offload = NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS |
				    NL80211_PROBE_RESP_OFFLOAD_SUPPORT_WPS2;

	wiphy->available_antennas_tx = mac->macinfo.num_tx_chain;
	wiphy->available_antennas_rx = mac->macinfo.num_rx_chain;

	wiphy->max_ap_assoc_sta = mac->macinfo.max_ap_assoc_sta;

	ether_addr_copy(wiphy->perm_addr, mac->macaddr);

	if (bus->hw_info.hw_capab & QLINK_HW_SUPPORTS_REG_UPDATE) {
		pr_debug("Device supports REG_UPDATE\n");
		wiphy->reg_notifier = qtnf_cfg80211_reg_notifier;
		pr_debug("Hint regulatory about EP region:%c%c\n",
			 bus->hw_info.country_code[0],
			 bus->hw_info.country_code[1]);
		regulatory_hint(wiphy, bus->hw_info.country_code);
	} else {
		pr_debug("Device doesn't support REG_UPDATE\n");
		wiphy->regulatory_flags |= REGULATORY_WIPHY_SELF_MANAGED;
	}

	pr_debug("Registering regulatory for WMAC %d\n", mac->macid);
	ret = wiphy_register(wiphy);

out:
	if (ret < 0) {
		pr_err("could not register wiphy\n");
		kfree(iface_comb);
		return ret;
	}

	return 0;
}

void qtnf_netdev_updown(struct net_device *ndev, bool up)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(ndev);

	if (qtnf_cmd_send_updown_intf(vif, up))
		pr_err("QTNF: failed to send intf up/down event to FW\n");
}

void qtnf_virtual_intf_cleanup(struct net_device *ndev)
{
	struct qtnf_vif *vif = qtnf_netdev_get_priv(ndev);
	struct qtnf_wmac *mac = mac = wiphy_priv(vif->wdev.wiphy);

	if (vif->wdev.iftype == NL80211_IFTYPE_STATION) {
		switch (vif->sta_state) {
		case QTNF_STA_DISCONNECTED:
			break;
		case QTNF_STA_CONNECTING:
			cfg80211_connect_result(vif->netdev,
						vif->bss_cfg.bssid, NULL, 0,
						NULL, 0,
						WLAN_STATUS_UNSPECIFIED_FAILURE,
						GFP_KERNEL);
			qtnf_disconnect(vif->wdev.wiphy, ndev,
					WLAN_REASON_DEAUTH_LEAVING);
			break;
		case QTNF_STA_CONNECTED:
			cfg80211_disconnected(vif->netdev,
					      WLAN_REASON_DEAUTH_LEAVING,
					      NULL, 0, 1, GFP_KERNEL);
			qtnf_disconnect(vif->wdev.wiphy, ndev,
					WLAN_REASON_DEAUTH_LEAVING);
			break;
		}
		vif->sta_state = QTNF_STA_DISCONNECTED;
		if (mac->scan_req) {
			cfg80211_scan_done(mac->scan_req, 1);
			mac->scan_req = NULL;
		}
	}
}
