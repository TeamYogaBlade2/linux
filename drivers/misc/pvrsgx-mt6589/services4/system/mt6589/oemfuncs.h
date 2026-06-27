// SPDX-License-Identifier: GPL-2.0-only

#if !defined(__OEMFUNCS_H__)
#define __OEMFUNCS_H__

#if defined (__cplusplus)
extern "C" {
#endif

typedef IMG_UINT32   (*PFN_SRV_BRIDGEDISPATCH)( IMG_UINT32  Ioctl,
												IMG_BYTE   *pInBuf,
												IMG_UINT32  InBufLen,
											    IMG_BYTE   *pOutBuf,
												IMG_UINT32  OutBufLen,
												IMG_UINT32 *pdwBytesTransferred);
typedef struct PVRSRV_DC_OEM_JTABLE_TAG
{
	PFN_SRV_BRIDGEDISPATCH			pfnOEMBridgeDispatch;
	IMG_PVOID						pvDummy1;
	IMG_PVOID						pvDummy2;
	IMG_PVOID						pvDummy3;

} PVRSRV_DC_OEM_JTABLE;

#define OEM_GET_EXT_FUNCS			(1<<1)

#if defined(__cplusplus)
}
#endif

#endif
