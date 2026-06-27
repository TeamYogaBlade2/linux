// SPDX-License-Identifier: MIT OR GPL-2.0-only

#include "img_types.h"

#ifndef __TTRACE_COMMON_H__
#define __TTRACE_COMMON_H__

/*
 * Trace item
 * ==========
 *
 * A trace item contains a trace header, a timestamp, a UID and a
 * data header all of which are 32-bit and mandatory. If there
 * is no data then the data header size is set to 0.
 *
 * Trace header
 * ------------
 * 31   27   23   19   15   11   7    3
 * GGGG GGGG CCCC CCCC TTTT TTTT TTTT TTTT
 *
 * G = group
 *     Note:
 *     Group 0xff means the message is padding
 *
 * C = class
 * T = Token
 *
 * Data header
 *-----------
 * 31   27   23   19   15   11   7    3
 * SSSS SSSS SSSS SSSS TTTT CCCC CCCC CCCC
 *
 * S = data packet size
 * T = Type
 *		0000 - 8 bit
 *		0001 - 16 bit
 *		0010 - 32 bit
 *		0011 - 64 bit
 *
 * C = data item count
 *
 * Note: It might look strange having both the packet
 *       size and the data item count, but the idea
 *       is the you might have a "special" data type
 *       who's size might not be known by the post
 *       processing program and rather then fail
 *       processing the buffer after that point if we
 *       know the size we can just skip it and move to
 *       the next item.
 */


#define PVRSRV_TRACE_HEADER		0
#define PVRSRV_TRACE_TIMESTAMP		1
#define PVRSRV_TRACE_HOSTUID		2
#define PVRSRV_TRACE_DATA_HEADER	3
#define PVRSRV_TRACE_DATA_PAYLOAD	4

#define PVRSRV_TRACE_ITEM_SIZE		16

#define PVRSRV_TRACE_GROUP_MASK		0xff
#define PVRSRV_TRACE_CLASS_MASK		0xff
#define PVRSRV_TRACE_TOKEN_MASK		0xffff

#define PVRSRV_TRACE_GROUP_SHIFT	24
#define PVRSRV_TRACE_CLASS_SHIFT	16
#define PVRSRV_TRACE_TOKEN_SHIFT	0

#define PVRSRV_TRACE_SIZE_MASK		0xffff
#define PVRSRV_TRACE_TYPE_MASK		0xf
#define PVRSRV_TRACE_COUNT_MASK		0xfff

#define PVRSRV_TRACE_SIZE_SHIFT		16
#define PVRSRV_TRACE_TYPE_SHIFT		12
#define PVRSRV_TRACE_COUNT_SHIFT	0


#define WRITE_HEADER(n,m) \
	((m & PVRSRV_TRACE_##n##_MASK) << PVRSRV_TRACE_##n##_SHIFT)

#define READ_HEADER(n,m) \
	((m & (PVRSRV_TRACE_##n##_MASK << PVRSRV_TRACE_##n##_SHIFT)) >> PVRSRV_TRACE_##n##_SHIFT)


#if defined(TTRACE_LARGE_BUFFER)
#define TIME_TRACE_BUFFER_SIZE		8192
#else
#define TIME_TRACE_BUFFER_SIZE		4096
#endif

/* Type defines for trace items */
#define PVRSRV_TRACE_TYPE_UI8		0
#define PVRSRV_TRACE_TYPE_UI16		1
#define PVRSRV_TRACE_TYPE_UI32		2
#define PVRSRV_TRACE_TYPE_UI64		3

#define PVRSRV_TRACE_TYPE_SYNC		15
 #define PVRSRV_TRACE_SYNC_UID		0
 #define PVRSRV_TRACE_SYNC_WOP		1
 #define PVRSRV_TRACE_SYNC_WOC		2
 #define PVRSRV_TRACE_SYNC_ROP		3
 #define PVRSRV_TRACE_SYNC_ROC		4
 #define PVRSRV_TRACE_SYNC_WO_DEV_VADDR	5
 #define PVRSRV_TRACE_SYNC_RO_DEV_VADDR	6
 #define PVRSRV_TRACE_SYNC_OP		7
 #define PVRSRV_TRACE_SYNC_RO2P		8
 #define PVRSRV_TRACE_SYNC_RO2C		9
 #define PVRSRV_TRACE_SYNC_RO2_DEV_VADDR 10
#define PVRSRV_TRACE_TYPE_SYNC_SIZE	((PVRSRV_TRACE_SYNC_RO2_DEV_VADDR + 1) * sizeof(IMG_UINT32))

#endif /* __TTRACE_COMMON_H__*/
