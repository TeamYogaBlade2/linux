// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __SGXSCRIPT_H__
#define __SGXSCRIPT_H__

#include "sgxfeaturedefs.h"

#define	SGX_MAX_INIT_COMMANDS	64
#define	SGX_MAX_PRINT_COMMANDS	64
#define	SGX_MAX_DEINIT_COMMANDS	16

typedef	enum _SGX_INIT_OPERATION
{
	SGX_INIT_OP_ILLEGAL = 0,
	SGX_INIT_OP_WRITE_HW_REG,
	SGX_INIT_OP_READ_HW_REG,
	SGX_INIT_OP_PRINT_HW_REG,
#if defined(PDUMP)
	SGX_INIT_OP_PDUMP_HW_REG,
#endif
	SGX_INIT_OP_HALT
} SGX_INIT_OPERATION;

typedef union _SGX_INIT_COMMAND
{
	SGX_INIT_OPERATION eOp;
	struct {
		SGX_INIT_OPERATION eOp;
		IMG_UINT32 ui32Offset;
		IMG_UINT32 ui32Value;
	} sWriteHWReg;
	struct {
		SGX_INIT_OPERATION eOp;
		IMG_UINT32 ui32Offset;
		IMG_UINT32 ui32Value;
	} sReadHWReg;
#if defined(PDUMP)
	struct {
		SGX_INIT_OPERATION eOp;
		IMG_UINT32 ui32Offset;
		IMG_UINT32 ui32Value;
	} sPDumpHWReg;
#endif
} SGX_INIT_COMMAND;

typedef struct _SGX_INIT_SCRIPTS_
{
	SGX_INIT_COMMAND asInitCommandsPart1[SGX_MAX_INIT_COMMANDS];
	SGX_INIT_COMMAND asInitCommandsPart2[SGX_MAX_INIT_COMMANDS];
	SGX_INIT_COMMAND asDeinitCommands[SGX_MAX_DEINIT_COMMANDS];
#if defined(SGX_FEATURE_MP)
	SGX_INIT_COMMAND asSGXREGDebugCommandsMaster[SGX_MAX_PRINT_COMMANDS];
#endif
	SGX_INIT_COMMAND asSGXREGDebugCommandsSlave[SGX_MAX_PRINT_COMMANDS];
} SGX_INIT_SCRIPTS;

#endif /* __SGXSCRIPT_H__ */

/*****************************************************************************
 End of file (sgxscript.h)
*****************************************************************************/
