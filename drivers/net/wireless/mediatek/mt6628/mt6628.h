/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MT6628 glue shared by the WMT and WLAN modules.
 *
 * Hardware constants are taken from Lenovo's GPL kernel release.  This file
 * deliberately contains no Android private API.
 */
#ifndef _B8000_MT6628_H
#define _B8000_MT6628_H

int mt6628_wmt_wifi_on(void);
void mt6628_wmt_wifi_off(void);

#endif

