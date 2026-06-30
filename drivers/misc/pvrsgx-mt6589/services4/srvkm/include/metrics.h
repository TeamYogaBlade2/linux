// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _METRICS_
#define _METRICS_


#if defined(DEBUG) || defined(TIMING)


typedef struct
{
	IMG_UINT32 ui32Start;
	IMG_UINT32 ui32Stop;
	IMG_UINT32 ui32Total;
	IMG_UINT32 ui32Count;
} Temporal_Data;

extern Temporal_Data asTimers[];

extern IMG_UINT32 PVRSRVTimeNow(IMG_VOID);
extern IMG_VOID   PVRSRVSetupMetricTimers(IMG_VOID *pvDevInfo);
extern IMG_VOID   PVRSRVOutputMetricTotals(IMG_VOID);


#define PVRSRV_TIMER_DUMMY				0

#define PVRSRV_TIMER_EXAMPLE_1			1
#define PVRSRV_TIMER_EXAMPLE_2			2


#define PVRSRV_NUM_TIMERS		(PVRSRV_TIMER_EXAMPLE_2 + 1)

#define PVRSRV_TIME_START(X)	{ \
									asTimers[X].ui32Count += 1; \
									asTimers[X].ui32Count |= 0x80000000L; \
									asTimers[X].ui32Start = PVRSRVTimeNow(); \
									asTimers[X].ui32Stop  = 0; \
								}

#define PVRSRV_TIME_SUSPEND(X)	{ \
									asTimers[X].ui32Stop += PVRSRVTimeNow() - asTimers[X].ui32Start; \
								}

#define PVRSRV_TIME_RESUME(X)	{ \
									asTimers[X].ui32Start = PVRSRVTimeNow(); \
								}

#define PVRSRV_TIME_STOP(X)		{ \
									asTimers[X].ui32Stop  += PVRSRVTimeNow() - asTimers[X].ui32Start; \
									asTimers[X].ui32Total += asTimers[X].ui32Stop; \
									asTimers[X].ui32Count &= 0x7FFFFFFFL; \
								}

#define PVRSRV_TIME_RESET(X)	{ \
									asTimers[X].ui32Start = 0; \
									asTimers[X].ui32Stop  = 0; \
									asTimers[X].ui32Total = 0; \
									asTimers[X].ui32Count = 0; \
								}


#if defined(__sh__)

#define TST_REG   ((volatile IMG_UINT8 *) (psDevInfo->pvSOCRegsBaseKM)) 	// timer start register

#define TCOR_2    ((volatile IMG_UINT *)  (psDevInfo->pvSOCRegsBaseKM+28))	// timer constant register_2
#define TCNT_2    ((volatile IMG_UINT *)  (psDevInfo->pvSOCRegsBaseKM+32))	// timer counter register_2
#define TCR_2     ((volatile IMG_UINT16 *)(psDevInfo->pvSOCRegsBaseKM+36))	// timer control register_2

#define TIMER_DIVISOR  4

#endif /* defined(__sh__) */



#else /* defined(DEBUG) || defined(TIMING) */



#define PVRSRV_TIME_START(X)
#define PVRSRV_TIME_SUSPEND(X)
#define PVRSRV_TIME_RESUME(X)
#define PVRSRV_TIME_STOP(X)
#define PVRSRV_TIME_RESET(X)

#define PVRSRVSetupMetricTimers(X)
#define PVRSRVOutputMetricTotals()



#endif /* defined(DEBUG) || defined(TIMING) */

#endif /* _METRICS_ */

/**************************************************************************
 End of file (metrics.h)
**************************************************************************/
