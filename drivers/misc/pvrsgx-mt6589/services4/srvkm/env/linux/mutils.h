// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef __IMG_LINUX_MUTILS_H__
#define __IMG_LINUX_MUTILS_H__

#include <linux/version.h>

#if !(defined(__i386__))
#if defined(SUPPORT_LINUX_X86_PAT)
#undef SUPPORT_LINUX_X86_PAT
#endif
#endif

#if defined(SUPPORT_LINUX_X86_PAT)
	pgprot_t pvr_pgprot_writecombine(pgprot_t prot);
	#define	PGPROT_WC(pv)	pvr_pgprot_writecombine(pv)
#else
	#if defined(__arm__) || defined(__sh__)
		#define	PGPROT_WC(pv)	pgprot_writecombine(pv)
	#elif defined(__mips__)
		#define PGPROT_WC(pv)	pgprot_writecombine(pv)
	#elif defined(__i386__) || defined(__x86_64)
		/* PAT support supersedes this */
		#define	PGPROT_WC(pv)	pgprot_noncached(pv)
	#else
		#define PGPROT_WC(pv)	pgprot_noncached(pv)
		#error  Unsupported architecture!
	#endif
#endif

#define	PGPROT_UC(pv)	pgprot_noncached(pv)

#if defined(__i386__)
	#define	IOREMAP(pa, bytes)	ioremap_cache(pa, bytes)
#else
	#if defined(__arm__)
		#define	IOREMAP(pa, bytes)	ioremap_cached(pa, bytes)
	#else
		#define IOREMAP(pa, bytes)	ioremap(pa, bytes)
	#endif
#endif

#if defined(SUPPORT_LINUX_X86_PAT)
	#if defined(SUPPORT_LINUX_X86_WRITECOMBINE)
		#define IOREMAP_WC(pa, bytes) ioremap_wc(pa, bytes)
	#else
		#define IOREMAP_WC(pa, bytes) ioremap_nocache(pa, bytes)
	#endif
#else
	#if defined(__arm__)
		#define IOREMAP_WC(pa, bytes) ioremap_wc(pa, bytes)
	#else
		#define IOREMAP_WC(pa, bytes)	ioremap_nocache(pa, bytes)
	#endif
#endif

#define	IOREMAP_UC(pa, bytes)	ioremap_nocache(pa, bytes)

IMG_VOID PVRLinuxMUtilsInit(IMG_VOID);

#endif /* __IMG_LINUX_MUTILS_H__ */
