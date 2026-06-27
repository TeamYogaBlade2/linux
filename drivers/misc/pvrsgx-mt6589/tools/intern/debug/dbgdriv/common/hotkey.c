// SPDX-License-Identifier: MIT OR GPL-2.0-only

#if !defined(LINUX) && !defined(__QNXNTO__)
#include <ntddk.h>
#include <windef.h>
#endif

#include "img_types.h"
#include "pvr_debug.h"
#include "dbgdrvif.h"
#include "dbgdriv.h"
#include "hotkey.h"
#include "hostfunc.h"




/*****************************************************************************
 Global vars
*****************************************************************************/

IMG_UINT32	g_ui32HotKeyFrame = 0xFFFFFFFF;
IMG_BOOL	g_bHotKeyPressed = IMG_FALSE;
IMG_BOOL	g_bHotKeyRegistered = IMG_FALSE;

/* Hotkey stuff */
PRIVATEHOTKEYDATA    g_PrivateHotKeyData;


/*****************************************************************************
 Code
*****************************************************************************/


/******************************************************************************
 * Function Name: ReadInHotKeys
 *
 * Inputs       : none
 * Outputs      : -
 * Returns      : nothing
 * Globals Used : -
 *
 * Description  : Gets Hot key entries from system.ini
 *****************************************************************************/
IMG_VOID ReadInHotKeys(IMG_VOID)
{
	g_PrivateHotKeyData.ui32ScanCode = 0x58;	/* F12	*/
	g_PrivateHotKeyData.ui32ShiftState = 0x0;

	/*
		Find buffer names etc..
	*/
	HostReadRegistryDWORDFromString("DEBUG\\Streams", "ui32ScanCode"  , &g_PrivateHotKeyData.ui32ScanCode);
	HostReadRegistryDWORDFromString("DEBUG\\Streams", "ui32ShiftState", &g_PrivateHotKeyData.ui32ShiftState);
}

/******************************************************************************
 * Function Name: RegisterKeyPressed
 *
 * Inputs       : IMG_UINT32 dwui32ScanCode, PHOTKEYINFO pInfo
 * Outputs      : -
 * Returns      : nothing
 * Globals Used : -
 *
 * Description  : Called when hotkey pressed.
 *****************************************************************************/
IMG_VOID RegisterKeyPressed(IMG_UINT32 dwui32ScanCode, PHOTKEYINFO pInfo)
{
	PDBG_STREAM	psStream;

	PVR_UNREFERENCED_PARAMETER(pInfo);

	if (dwui32ScanCode == g_PrivateHotKeyData.ui32ScanCode)
	{
		PVR_DPF((PVR_DBG_MESSAGE,"PDUMP Hotkey pressed !\n"));

		psStream = (PDBG_STREAM) g_PrivateHotKeyData.sHotKeyInfo.pvStream;

		if (!g_bHotKeyPressed)
		{
			/*
				Capture the next frame.
			*/
			g_ui32HotKeyFrame = psStream->psCtrl->ui32Current + 2;

			/*
				Do the flag.
			*/
			g_bHotKeyPressed = IMG_TRUE;
		}
	}
}

/******************************************************************************
 * Function Name: ActivateHotKeys
 *
 * Inputs       : -
 * Outputs      : -
 * Returns      : -
 * Globals Used : -
 *
 * Description  : Installs HotKey callbacks
 *****************************************************************************/
IMG_VOID ActivateHotKeys(PDBG_STREAM psStream)
{
	/*
		Setup hotkeys.
	*/
	ReadInHotKeys();

	/*
		Has it already been allocated.
	*/
	if (!g_PrivateHotKeyData.sHotKeyInfo.hHotKey)
	{
		if (g_PrivateHotKeyData.ui32ScanCode != 0)
		{
			PVR_DPF((PVR_DBG_MESSAGE,"Activate HotKey for PDUMP.\n"));

			/*
				Add in stream data.
			*/
			g_PrivateHotKeyData.sHotKeyInfo.pvStream = psStream;

			DefineHotKey(g_PrivateHotKeyData.ui32ScanCode, g_PrivateHotKeyData.ui32ShiftState, &g_PrivateHotKeyData.sHotKeyInfo);
		}
		else
		{
			g_PrivateHotKeyData.sHotKeyInfo.hHotKey = 0;
		}
	}
}

/******************************************************************************
 * Function Name: DeactivateHotKeys
 *
 * Inputs       : -
 * Outputs      : -
 * Returns      : -
 * Globals Used : -
 *
 * Description  : Removes HotKey callbacks
 *****************************************************************************/
IMG_VOID DeactivateHotKeys(IMG_VOID)
{
	if (g_PrivateHotKeyData.sHotKeyInfo.hHotKey != 0)
	{
		PVR_DPF((PVR_DBG_MESSAGE,"Deactivate HotKey.\n"));

		RemoveHotKey(g_PrivateHotKeyData.sHotKeyInfo.hHotKey);
		g_PrivateHotKeyData.sHotKeyInfo.hHotKey = 0;
	}
}


/*****************************************************************************
 End of file (HOTKEY.C)
*****************************************************************************/
