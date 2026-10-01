#ifndef __MTK_HAL_MM_H__
#define __MTK_HAL_MM_H__

#include "img_types.h"


extern atomic_t g_MtkSysRAMUseInByte_atomic;
static inline IMG_VOID MTKSysRAMInc(IMG_UINT32 uiByte)
{
    atomic_add (uiByte, &g_MtkSysRAMUseInByte_atomic);    
}
static inline IMG_VOID MTKSysRAMDec(IMG_UINT32 uiByte)
{
    atomic_sub (uiByte, &g_MtkSysRAMUseInByte_atomic);   
}

static inline IMG_UINT32 MTKGetSysRAMStats(IMG_VOID)
{
    return ((unsigned int)atomic_read (&g_MtkSysRAMUseInByte_atomic));
}


#endif
