// SPDX-License-Identifier: GPL-2.0
/* Disabled Android/WEXT/P2P entry points still referenced by vendor core. */

#include "precomp.h"
#include "gl_wext.h"
#include "gl_wext_priv.h"

BOOLEAN fgIsResetting;

int wext_get_priv(struct net_device *dev, struct ifreq *ifr)
{
	return -EOPNOTSUPP;
}

int wext_support_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return -EOPNOTSUPP;
}

int priv_support_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	return -EOPNOTSUPP;
}

void wext_indicate_wext_event(P_GLUE_INFO_T glue, unsigned int cmd,
			      unsigned char *data, unsigned int len)
{
}

#if CFG_SUPPORT_WPS
BOOLEAN wextSrchDesiredWPSIE(const UINT_8 *ie, INT_32 len, UINT_8 id,
			     PUINT_8 *desired)
{
	while (len >= 2) {
		INT_32 ie_len = ie[1] + 2;

		if (ie_len > len)
			break;
		if (ie[0] == id && (id != 0xdd ||
		    (ie[1] >= 4 && !memcmp(ie + 2, "\x00\x50\xf2\x04", 4)))) {
			*desired = (PUINT_8)ie;
			return TRUE;
		}
		ie += ie_len;
		len -= ie_len;
	}
	return FALSE;
}
#endif

void bssClearClientList(P_ADAPTER_T adapter, P_BSS_INFO_T bss)
{
}

WLAN_STATUS bssUpdateBeaconContent(P_ADAPTER_T adapter,
				   ENUM_NETWORK_TYPE_INDEX_T network)
{
	return WLAN_STATUS_NOT_SUPPORTED;
}

void bssInitForAP(P_ADAPTER_T adapter, P_BSS_INFO_T bss,
		  BOOLEAN update_rates)
{
}
