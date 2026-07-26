// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _SGXDEFS_H_
#define	_SGXDEFS_H_

#include "sgxerrata.h"
#include "sgxfeaturedefs.h"

#if defined(SGX520)
#include "sgx520defs.h"
#else
#if defined(SGX530)
#include "sgx530defs.h"
#else
#if defined(SGX535)
#include "sgx535defs.h"
#else
#if defined(SGX535_V1_1)
#include "sgx535defs.h"
#else
#if defined(SGX540)
#include "sgx540defs.h"
#else
#if defined(SGX543)
#if defined(FIX_HW_BRN_29954)
#include "sgx543_v1.164defs.h"
#else
#include "sgx543defs.h"
#endif
#else
#if defined(SGX544)
#include "sgx544defs.h"
#else
#if defined(SGX545)
#include "sgx545defs.h"
#else
#if defined(SGX531)
#include "sgx531defs.h"
#else
#if defined(SGX554)
#include "sgx554defs.h"
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif
#endif

#if defined(SGX_FEATURE_MP)
#if defined(SGX554)
#include "sgxmpplusdefs.h"
#else
#include "sgxmpdefs.h"
#endif /* SGX554 */
#else /* SGX_FEATURE_MP */
#if defined(SGX_FEATURE_SYSTEM_CACHE)
#include "mnemedefs.h"
#endif
#endif /* SGX_FEATURE_MP */

/*****************************************************************************
 Core specific defines.
*****************************************************************************/

#endif /* _SGXDEFS_H_ */

/*****************************************************************************
 End of file (sgxdefs.h)
*****************************************************************************/
