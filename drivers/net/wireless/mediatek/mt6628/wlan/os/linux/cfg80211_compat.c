// SPDX-License-Identifier: GPL-2.0
/* cfg80211 callback shims for the current kernel API. */

#include "gl_os.h"
#include "gl_cfg80211.h"

int mt6628_cfg_change_iface(struct wiphy *wiphy, struct net_device *ndev,
			    enum nl80211_iftype type,
			    struct vif_params *params)
{
	return mtk_cfg80211_change_iface(wiphy, ndev, type, NULL, params);
}

int mt6628_cfg_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr, struct key_params *params)
{
	return mtk_cfg80211_add_key(wiphy, wdev->netdev, key_index, pairwise,
				    mac_addr, params);
}

int mt6628_cfg_get_key(struct wiphy *wiphy, struct wireless_dev *wdev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr, void *cookie,
		       void (*callback)(void *, struct key_params *))
{
	return mtk_cfg80211_get_key(wiphy, wdev->netdev, key_index, pairwise,
				    mac_addr, cookie, callback);
}

int mt6628_cfg_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
		       int link_id, u8 key_index, bool pairwise,
		       const u8 *mac_addr)
{
	return mtk_cfg80211_del_key(wiphy, wdev->netdev, key_index, pairwise,
				    mac_addr);
}

int mt6628_cfg_set_default_key(struct wiphy *wiphy, struct net_device *ndev,
			       int link_id, u8 key_index, bool unicast,
			       bool multicast)
{
	return mtk_cfg80211_set_default_key(wiphy, ndev, key_index, unicast,
					    multicast);
}

int mt6628_cfg_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			   const u8 *mac, struct station_info *sinfo)
{
	return mtk_cfg80211_get_station(wiphy, wdev->netdev, (u8 *)mac, sinfo);
}

int mt6628_cfg_scan(struct wiphy *wiphy,
		    struct cfg80211_scan_request *request)
{
	return mtk_cfg80211_scan(wiphy, request->wdev->netdev, request);
}
