/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * prismrv_ukpatch.h - in-kernel patching of the *stock* vendor uKernel image.
 *
 * The vendor loader (libsrv_init SetupuKernel) patches 19 words of the uKernel and 29 words of the
 * objects it uploads, all as a function of where the objects were placed in GPU virtual memory.
 * This code reproduces exactly that, so the firmware file can carry the unmodified vendor image
 * (all 0xDEADBEEF placeholders intact) and the driver decides the layout.
 *
 * Every rule here was derived by running the vendor loader at several randomised layouts and
 * differencing against .rodata (prismrv repo: mainline-tools/ukernel_loader_emu/derive_patches.py,
 * analysis/ukernel/LOADER_PATCH_RULES.md) and is checked bit-for-bit against the loader's output by
 * mainline-tools/ukpatch_check/ in that repo.  Pure C so the same file builds on the host.
 */
#ifndef PRISMRV_UKPATCH_H
#define PRISMRV_UKPATCH_H

#ifdef PRISMRV_UKP_HOST
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#else
#include <linux/types.h>
#include <linux/stddef.h>
#endif

/* container roles (see prismrv_device.h / gen_ukernel_fw.py) */
enum {
	UKP_R_CCB = 1, UKP_R_CCBCTL = 2, UKP_R_HOSTCTL = 4, UKP_R_TA3DCTL = 5, UKP_R_PTLAWB = 6,
	UKP_R_EDMSTATUS = 8, UKP_R_UNK9 = 9, UKP_R_UNK10 = 10, UKP_R_UNK11 = 11,
	UKP_R_UKERNEL = 16, UKP_R_UKSLAVE = 17,
};

struct prismrv_ukp_obj {
	u32 role;
	u32 va;		/* GPU VA the driver maps the object at */
	u32 size;
	void *data;	/* writable copy of the object, `size` bytes */
};

/* returns 0, or a negative errno; *why names the failing rule (static string) */
int prismrv_ukp_apply(const struct prismrv_ukp_obj *objs, unsigned int n, const char **why);

/*
 * Init-script (-init.bin) values that depend on the layout.  Derived like the uKernel rules
 * (derive_patches.py --init): across randomised layouts exactly three WRITE records change -
 *   reg 0xac4  = VA of the event-kicker object (role 3)
 *   reg 0xa68  = VA of PDS program role 18,   reg 0x8a68 = VA of PDS program role 19
 * Everything else in the script is layout independent.  Call for every WRITE record before it is
 * issued; returns 1 and sets *val if the register is layout dependent, 0 if the record is used as is,
 * negative errno if the needed object is missing.
 */
int prismrv_ukp_init_fixup(const struct prismrv_ukp_obj *objs, unsigned int n, u32 reg, u32 *val);

#endif
