// SPDX-License-Identifier: GPL-2.0-only

#if !defined(__SYSINFO_H__)
#define __SYSINFO_H__

#if defined(PVR_LINUX_USING_WORKQUEUES)
#define MAX_HW_TIME_US				(1000000)
#define WAIT_TRY_COUNT				(20000)
#else
#define MAX_HW_TIME_US				(500000)
#define WAIT_TRY_COUNT				(10000)
#endif


#define SYS_DEVICE_COUNT 15

#endif
