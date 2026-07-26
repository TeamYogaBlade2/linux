// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef SERVICES_HEADERS_H
#define SERVICES_HEADERS_H

#ifdef DEBUG_RELEASE_BUILD
#pragma optimize( "", off )
#define DEBUG		1
#endif

#include "img_defs.h"
#include "services.h"
#include "servicesint.h"
#include "power.h"
#include "resman.h"
#include "queue.h"
#include "srvkm.h"
#include "kerneldisplay.h"
#include "syscommon.h"
#include "pvr_debug.h"
#include "metrics.h"
#include "osfunc.h"
#include "refcount.h"

#endif /* SERVICES_HEADERS_H */
