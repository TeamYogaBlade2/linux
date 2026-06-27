// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _ENV_DATA_
#define _ENV_DATA_

#include <linux/interrupt.h>
#include <linux/pci.h>

#if defined(PVR_LINUX_MISR_USING_WORKQUEUE) || defined(PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE)
#include <linux/workqueue.h>
#endif

/*
 *	Env data specific to linux - convenient place to put this
 */

/* Fairly arbitrary sizes - hopefully enough for all bridge calls */
#define PVRSRV_MAX_BRIDGE_IN_SIZE	0x1000
#define PVRSRV_MAX_BRIDGE_OUT_SIZE	0x1000

typedef	struct _PVR_PCI_DEV_TAG
{
	struct pci_dev		*psPCIDev;
	HOST_PCI_INIT_FLAGS	ePCIFlags;
	IMG_BOOL abPCIResourceInUse[DEVICE_COUNT_RESOURCE];
} PVR_PCI_DEV;

typedef struct _ENV_DATA_TAG
{
	IMG_VOID		*pvBridgeData;
	struct pm_dev		*psPowerDevice;
	IMG_BOOL		bLISRInstalled;
	IMG_BOOL		bMISRInstalled;
	IMG_UINT32		ui32IRQ;
	IMG_VOID		*pvISRCookie;
#if defined(PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE)
	struct workqueue_struct	*psWorkQueue;
#endif
#if defined(PVR_LINUX_MISR_USING_WORKQUEUE) || defined(PVR_LINUX_MISR_USING_PRIVATE_WORKQUEUE)
	struct work_struct	sMISRWork;
	IMG_VOID		*pvMISRData;
#else
	struct tasklet_struct	sMISRTasklet;
#endif
#if defined (SUPPORT_ION)
	IMG_HANDLE		hIonHeaps;
	IMG_HANDLE		hIonDev;
#endif
} ENV_DATA;

#endif /* _ENV_DATA_ */
/*****************************************************************************
 End of file (env_data.h)
*****************************************************************************/
