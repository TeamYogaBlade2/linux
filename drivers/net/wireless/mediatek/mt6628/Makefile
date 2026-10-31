# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_MT6628_WMT) += wmt.o
obj-$(CONFIG_MT6628_WLAN) += wlan_mt6628.o

ccflags-y += -DLINUX -DMT6628 -D_HIF_SDIO=1
ccflags-y += -DCFG_SUPPORT_WAPI=0 -DCFG_SUPPORT_XLOG=0
ccflags-y += -DCFG_SUPPORT_AEE=0 -DDBG=1
ccflags-y += -I$(src)/wlan/os -I$(src)/wlan/os/linux/include
ccflags-y += -I$(src)
ccflags-y += -I$(src)/wlan/os/linux/hif/sdio/include
ccflags-y += -I$(src)/wlan/include -I$(src)/wlan/include/nic
ccflags-y += -I$(src)/wlan/include/mgmt

wlan_mt6628-y := \
	wlan/common/dump.o \
	wlan/common/wlan_lib.o \
	wlan/common/wlan_oid.o \
	wlan/common/wlan_bow.o \
	wlan/nic/nic.o \
	wlan/nic/nic_tx.o \
	wlan/nic/nic_rx.o \
	wlan/nic/nic_pwr_mgt.o \
	wlan/nic/cmd_buf.o \
	wlan/nic/que_mgt.o \
	wlan/nic/nic_cmd_event.o \
	wlan/os/linux/gl_init.o \
	wlan/os/linux/gl_kal.o \
	wlan/os/linux/gl_bow.o \
	wlan/os/linux/gl_cfg80211.o \
	wlan/os/linux/cfg80211_compat.o \
	wlan/os/linux/legacy_stubs.o \
	wlan/os/linux/gl_rst.o \
	wlan/os/linux/platform.o \
	wlan/os/linux/hif/sdio/arm.o \
	wlan/os/linux/hif/sdio/sdio.o \
	wlan/mgmt/ais_fsm.o \
	wlan/mgmt/aaa_fsm.o \
	wlan/mgmt/assoc.o \
	wlan/mgmt/auth.o \
	wlan/mgmt/bss.o \
	wlan/mgmt/cnm.o \
	wlan/mgmt/cnm_timer.o \
	wlan/mgmt/cnm_mem.o \
	wlan/mgmt/hem_mbox.o \
	wlan/mgmt/mib.o \
	wlan/mgmt/privacy.o \
	wlan/mgmt/rate.o \
	wlan/mgmt/rlm.o \
	wlan/mgmt/rlm_domain.o \
	wlan/mgmt/rlm_obss.o \
	wlan/mgmt/rlm_protection.o \
	wlan/mgmt/rsn.o \
	wlan/mgmt/saa_fsm.o \
	wlan/mgmt/scan.o \
	wlan/mgmt/scan_fsm.o \
	wlan/mgmt/sec_fsm.o \
	wlan/mgmt/swcr.o \
	wlan/mgmt/roaming_fsm.o

ccflags-y += -Wall -Wno-format -Wno-unused-but-set-variable
ccflags-y += -Wno-unused-variable -Wno-missing-prototypes
ccflags-y += -Wno-sometimes-uninitialized
