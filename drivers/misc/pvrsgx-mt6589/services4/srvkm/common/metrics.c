// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "services_headers.h"
#include "metrics.h"

/* VGX: */
#if defined(SUPPORT_VGX)
#include "vgxapi_km.h"
#endif

/* SGX: */
#if defined(SUPPORT_SGX)
#include "sgxapi_km.h"
#endif

#if defined(DEBUG) || defined(TIMING)

static volatile IMG_UINT32 *pui32TimerRegister = 0;

#define PVRSRV_TIMER_TOTAL_IN_TICKS(X)	asTimers[X].ui32Total
#define PVRSRV_TIMER_TOTAL_IN_MS(X)		((1000*asTimers[X].ui32Total)/ui32TicksPerMS)
#define PVRSRV_TIMER_COUNT(X)			asTimers[X].ui32Count


Temporal_Data asTimers[PVRSRV_NUM_TIMERS];


/***********************************************************************************
 Function Name      : PVRSRVTimeNow
 Inputs             : None
 Outputs            : None
 Returns            : Current timer register value
 Description        : Returns the current timer register value
************************************************************************************/
IMG_UINT32 PVRSRVTimeNow(IMG_VOID)
{
	if (!pui32TimerRegister)
	{
		static IMG_BOOL bFirstTime = IMG_TRUE;

		if (bFirstTime)
		{
			PVR_DPF((PVR_DBG_ERROR,"PVRSRVTimeNow: No timer register set up"));

			bFirstTime = IMG_FALSE;
		}

		return 0;
	}

#if defined(__sh__)

	return (0xffffffff-*pui32TimerRegister);

#else /* defined(__sh__) */

	return 0;

#endif /* defined(__sh__) */
}


/***********************************************************************************
 Function Name      : PVRSRVGetCPUFreq
 Inputs             : None
 Outputs            : None
 Returns            : CPU timer frequency
 Description        : Returns the CPU timer frequency
************************************************************************************/
static IMG_UINT32 PVRSRVGetCPUFreq(IMG_VOID)
{
	IMG_UINT32 ui32Time1, ui32Time2;

	ui32Time1 = PVRSRVTimeNow();

	OSWaitus(1000000);

	ui32Time2 = PVRSRVTimeNow();

	PVR_DPF((PVR_DBG_WARNING, "PVRSRVGetCPUFreq: timer frequency = %d Hz", ui32Time2 - ui32Time1));

	return (ui32Time2 - ui32Time1);
}


/***********************************************************************************
 Function Name      : PVRSRVSetupMetricTimers
 Inputs             : pvDevInfo
 Outputs            : None
 Returns            : None
 Description        : Resets metric timers and sets up the timer register
************************************************************************************/
IMG_VOID PVRSRVSetupMetricTimers(IMG_VOID *pvDevInfo)
{
	IMG_UINT32 ui32Loop;

	PVR_UNREFERENCED_PARAMETER(pvDevInfo);

	for(ui32Loop=0; ui32Loop < (PVRSRV_NUM_TIMERS); ui32Loop++)
	{
		asTimers[ui32Loop].ui32Total = 0;
		asTimers[ui32Loop].ui32Count = 0;
	}

	#if defined(__sh__)

		/* timer control register */
		// clock / 1024 when TIMER_DIVISOR = 4
		// underflow int disabled
		// we get approx 38 uS per timer tick
		*TCR_2 = TIMER_DIVISOR;

		/* reset the timer counter to 0 */
		*TCOR_2 = *TCNT_2 = (IMG_UINT)0xffffffff;

		/* start timer 2 */
		*TST_REG |= (IMG_UINT8)0x04;

		pui32TimerRegister = (IMG_UINT32 *)TCNT_2;

	#else /* defined(__sh__) */

		pui32TimerRegister = 0;

	#endif /* defined(__sh__) */
}


/***********************************************************************************
 Function Name      : PVRSRVOutputMetricTotals
 Inputs             : None
 Outputs            : None
 Returns            : None
 Description        : Displays final metric data
************************************************************************************/
IMG_VOID PVRSRVOutputMetricTotals(IMG_VOID)
{
	IMG_UINT32 ui32TicksPerMS, ui32Loop;

	ui32TicksPerMS = PVRSRVGetCPUFreq();

	if (!ui32TicksPerMS)
	{
		PVR_DPF((PVR_DBG_ERROR,"PVRSRVOutputMetricTotals: Failed to get CPU Freq"));
		return;
	}

	for(ui32Loop=0; ui32Loop < (PVRSRV_NUM_TIMERS); ui32Loop++)
	{
		if (asTimers[ui32Loop].ui32Count & 0x80000000L)
		{
			PVR_DPF((PVR_DBG_WARNING,"PVRSRVOutputMetricTotals: Timer %u is still ON", ui32Loop));
		}
	}
#if 0
	/*
	** EXAMPLE TIMER OUTPUT
	*/
	PVR_DPF((PVR_DBG_ERROR," Timer(%u): Total = %u",PVRSRV_TIMER_EXAMPLE_1, PVRSRV_TIMER_TOTAL_IN_TICKS(PVRSRV_TIMER_EXAMPLE_1)));
	PVR_DPF((PVR_DBG_ERROR," Timer(%u): Time = %ums",PVRSRV_TIMER_EXAMPLE_1, PVRSRV_TIMER_TOTAL_IN_MS(PVRSRV_TIMER_EXAMPLE_1)));
	PVR_DPF((PVR_DBG_ERROR," Timer(%u): Count = %u",PVRSRV_TIMER_EXAMPLE_1, PVRSRV_TIMER_COUNT(PVRSRV_TIMER_EXAMPLE_1)));
#endif
}

#endif /* defined(DEBUG) || defined(TIMING) */

/******************************************************************************
 End of file (metrics.c)
******************************************************************************/
