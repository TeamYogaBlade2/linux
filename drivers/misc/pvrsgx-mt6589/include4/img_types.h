// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __IMG_TYPES_H__
#define __IMG_TYPES_H__

/* define all address space bit depths: */
/* CPU virtual address space defaults to 32bits */
#if !defined(IMG_ADDRSPACE_CPUVADDR_BITS)
#define IMG_ADDRSPACE_CPUVADDR_BITS		32
#endif

/* Physical address space defaults to 32bits */
#if !defined(IMG_ADDRSPACE_PHYSADDR_BITS)
#define IMG_ADDRSPACE_PHYSADDR_BITS		32
#endif

typedef unsigned int	IMG_UINT,	*IMG_PUINT;
typedef signed int		IMG_INT,	*IMG_PINT;

typedef unsigned char	IMG_UINT8,	*IMG_PUINT8;
typedef unsigned char	IMG_BYTE,	*IMG_PBYTE;
typedef signed char		IMG_INT8,	*IMG_PINT8;
typedef char			IMG_CHAR,	*IMG_PCHAR;

typedef unsigned short	IMG_UINT16,	*IMG_PUINT16;
typedef signed short	IMG_INT16,	*IMG_PINT16;
#if !defined(IMG_UINT32_IS_ULONG)
typedef unsigned int	IMG_UINT32,	*IMG_PUINT32;
typedef signed int		IMG_INT32,	*IMG_PINT32;
#else
typedef unsigned long	IMG_UINT32,	*IMG_PUINT32;
typedef signed long		IMG_INT32,	*IMG_PINT32;
#endif
#if !defined(IMG_UINT32_MAX)
	#define IMG_UINT32_MAX 0xFFFFFFFFUL
#endif

#if  defined(USE_CODE)
	typedef unsigned __int64	IMG_UINT64, *IMG_PUINT64;
	typedef __int64				IMG_INT64,  *IMG_PINT64;
#elif defined(LINUX) && defined (__x86_64)
	typedef unsigned long		IMG_UINT64,	*IMG_PUINT64;
	typedef long 				IMG_INT64,	*IMG_PINT64;
#elif defined(LINUX) || defined(__METAG) || defined (__QNXNTO__)
	typedef unsigned long long		IMG_UINT64,	*IMG_PUINT64;
	typedef long long 				IMG_INT64,	*IMG_PINT64;
#else
	#error("define an OS")
#endif

#if !(defined(LINUX) && defined (__KERNEL__))
/* Linux kernel mode does not use floating point */
typedef float			IMG_FLOAT,	*IMG_PFLOAT;
typedef double			IMG_DOUBLE, *IMG_PDOUBLE;
#endif

typedef	enum tag_img_bool
{
	IMG_FALSE		= 0,
	IMG_TRUE		= 1,
	IMG_FORCE_ALIGN = 0x7FFFFFFF
} IMG_BOOL, *IMG_PBOOL;

typedef void            IMG_VOID, *IMG_PVOID;

typedef IMG_INT32       IMG_RESULT;

#if defined(_WIN64)
	typedef unsigned __int64	IMG_UINTPTR_T;
    typedef signed   __int64    IMG_INTPTR_T;
	typedef signed __int64		IMG_PTRDIFF_T;
	typedef IMG_UINT64			IMG_SIZE_T;
#else
    #if defined (__x86_64__)
    	typedef IMG_UINT64		IMG_SIZE_T;
        typedef unsigned long   IMG_UINTPTR_T;
        typedef signed long     IMG_INTPTR_T;
    #else
    	typedef IMG_UINT32		IMG_SIZE_T;
        typedef unsigned long	IMG_UINTPTR_T;
        typedef signed long     IMG_INTPTR_T;
    #endif
#endif

typedef IMG_PVOID       IMG_HANDLE;

typedef void**          IMG_HVOID,	* IMG_PHVOID;

#define IMG_NULL        0

/* services/stream ID */
typedef IMG_UINTPTR_T      IMG_SID;

typedef IMG_UINTPTR_T      IMG_EVENTSID;

/*
 * Address types.
 * All types used to refer to a block of memory are wrapped in structures
 * to enforce some degree of type safety, i.e. a IMG_DEV_VIRTADDR cannot
 * be assigned to a variable of type IMG_DEV_PHYADDR because they are not the
 * same thing.
 *
 * There is an assumption that the system contains at most one non-cpu mmu,
 * and a memory block is only mapped by the MMU once.
 *
 * Different devices could have offset views of the physical address space.
 *
 */


/*
 *
 * +------------+    +------------+      +------------+        +------------+
 * |    CPU     |    |    DEV     |      |    DEV     |        |    DEV     |
 * +------------+    +------------+      +------------+        +------------+
 *       |                 |                   |                     |
 *       | PVOID           |IMG_DEV_VIRTADDR   |IMG_DEV_VIRTADDR     |
 *       |                 \-------------------/                     |
 *       |                          |                                |
 * +------------+             +------------+                         |
 * |    MMU     |             |    MMU     |                         |
 * +------------+             +------------+                         |
 *       |                          |                                |
 *       |                          |                                |
 *       |                          |                                |
 *   +--------+                +---------+                      +--------+
 *   | Offset |                | (Offset)|                      | Offset |
 *   +--------+                +---------+                      +--------+
 *       |                          |                IMG_DEV_PHYADDR |
 *       |                          |                                |
 *       |                          | IMG_DEV_PHYADDR                |
 * +---------------------------------------------------------------------+
 * |                         System Address bus                          |
 * +---------------------------------------------------------------------+
 *
 */

typedef IMG_PVOID IMG_CPU_VIRTADDR;

/* device virtual address */
typedef struct _IMG_DEV_VIRTADDR
{
	/* device virtual addresses are 32bit for now */
	IMG_UINT32  uiAddr;
#define IMG_CAST_TO_DEVVADDR_UINT(var)		(IMG_UINT32)(var)

} IMG_DEV_VIRTADDR;

typedef IMG_UINT32 IMG_DEVMEM_SIZE_T;

/* cpu physical address */
typedef struct _IMG_CPU_PHYADDR
{
	/* variable sized type (32,64) */
#if IMG_ADDRSPACE_PHYSADDR_BITS == 32
	/* variable sized type (32,64) */
	IMG_UINT32 uiAddr;
#else
	IMG_UINT64 uiAddr;
#endif
} IMG_CPU_PHYADDR;

/* device physical address */
typedef struct _IMG_DEV_PHYADDR
{
#if IMG_ADDRSPACE_PHYSADDR_BITS == 32
	/* variable sized type (32,64) */
	IMG_UINT32 uiAddr;
#else
	IMG_UINT64 uiAddr;
#endif
} IMG_DEV_PHYADDR;

/* system physical address */
typedef struct _IMG_SYS_PHYADDR
{
	/* variable sized type (32,64) */
#if IMG_ADDRSPACE_PHYSADDR_BITS == 32
	/* variable sized type (32,64) */
	IMG_UINT32 uiAddr;
#else
	IMG_UINT64 uiAddr;
#endif
} IMG_SYS_PHYADDR;

#include "img_defs.h"

#endif	/* __IMG_TYPES_H__ */
/******************************************************************************
 End of file (img_types.h)
******************************************************************************/
