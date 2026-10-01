// SPDX-License-Identifier: GPL-2.0-only
/*
 * MTK system RAM usage counter (optional statistic).
 */

#include <linux/atomic.h>

#ifdef MTK_HAL_MM_STATISTIC

atomic_t g_MtkSysRAMUseInByte_atomic = ATOMIC_INIT(0);

#endif
