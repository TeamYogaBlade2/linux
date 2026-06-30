// SPDX-License-Identifier: MIT OR GPL-2.0-only

#if !defined (__IMG_DEFS_H__)
#define __IMG_DEFS_H__

#include "img_types.h"

typedef		enum	img_tag_TriStateSwitch
{
	IMG_ON		=	0x00,
	IMG_OFF,
	IMG_IGNORE

} img_TriStateSwitch, * img_pTriStateSwitch;

#define		IMG_SUCCESS				0

#define		IMG_NO_REG				1

#if defined (NO_INLINE_FUNCS)
	#define	INLINE
	#define	FORCE_INLINE
#else
#if	!defined(INLINE)
	#define	INLINE					inline
#endif
	#define	FORCE_INLINE			static inline
#endif


/* Use this in any file, or use attributes under GCC - see below */
#ifndef PVR_UNREFERENCED_PARAMETER
#define	PVR_UNREFERENCED_PARAMETER(param) (param) = (param)
#endif

/* The best way to supress unused parameter warnings using GCC is to use a
 * variable attribute.  Place the unref__ between the type and name of an
 * unused parameter in a function parameter list, eg `int unref__ var'. This
 * should only be used in GCC build environments, for example, in files that
 * compile only on Linux. Other files should use UNREFERENCED_PARAMETER */
#ifdef __GNUC__
#define unref__ __attribute__ ((unused))
#else
#define unref__
#endif

/*
	Wide character definitions
*/
#ifndef _TCHAR_DEFINED
#if defined(UNICODE)
typedef unsigned short		TCHAR, *PTCHAR, *PTSTR;
#else	/* #if defined(UNICODE) */
typedef char				TCHAR, *PTCHAR, *PTSTR;
#endif	/* #if defined(UNICODE) */
#define _TCHAR_DEFINED
#endif /* #ifndef _TCHAR_DEFINED */


			#if defined(__linux__) || defined(__QNXNTO__) || defined(__METAG)

				#define IMG_CALLCONV
				#define IMG_INTERNAL	__attribute__((visibility("hidden")))
				#define IMG_EXPORT		__attribute__((visibility("default")))
				#define IMG_IMPORT
				#define IMG_RESTRICT	__restrict__

			#else
					#error("define an OS")
			#endif

// Use default definition if not overridden
#ifndef IMG_ABORT
	#define IMG_ABORT()	abort()
#endif

#ifndef IMG_MALLOC
	#define IMG_MALLOC(A)		malloc	(A)
#endif

#ifndef IMG_FREE
	#define IMG_FREE(A)			free	(A)
#endif

#define IMG_CONST const

#if defined(__GNUC__)
#define IMG_FORMAT_PRINTF(x,y)		__attribute__((format(printf,x,y)))
#else
#define IMG_FORMAT_PRINTF(x,y)
#endif

/*
 * Cleanup request defines
  */
#define  CLEANUP_WITH_POLL		IMG_FALSE
#define  FORCE_CLEANUP			IMG_TRUE

#if defined (_WIN64)
#define IMG_UNDEF	(~0ULL)
#else
#define IMG_UNDEF	(~0UL)
#endif

/*
   Do the right thing when using printf to output cpu addresses,
   depending on architecture.
 */
#if defined (_WIN64)
    #define UINTPTR_FMT "%016llX"
#else
    #if defined (__x86_64__)
        #define UINTPTR_FMT "%016lX"
    #else
        #define UINTPTR_FMT "%08lX"
    #endif
#endif

/*
   Similarly for DEV_ and SYS_ PHYSADDRs, but this is dependent on 32/36-bit MMU
   capability, in addition to host architecture.
 */
#if IMG_ADDRSPACE_PHYSADDR_BITS == 32
	#if defined(IMG_UINT32_IS_ULONG)
		#define CPUPADDR_FMT "%08lX"
		#define DEVPADDR_FMT "%08lX"
		#define SYSPADDR_FMT "%08lX"
	#else
		#define CPUPADDR_FMT "%08X"
		#define DEVPADDR_FMT "%08X"
		#define SYSPADDR_FMT "%08X"
	#endif
#else
	#if defined(__x86_64__)
			#define CPUPADDR_FMT "%016lX"
			#define DEVPADDR_FMT "%016lX"
			#define SYSPADDR_FMT "%016lX"
	#else

			#define CPUPADDR_FMT "%016llX"
			#define DEVPADDR_FMT "%016llX"
			#define SYSPADDR_FMT "%016llX"
	#endif
#endif

/*
   Define a printf format macro for the length property of the format-specifier
   for size_t, that allows avoidance of C99 dependency on compilers that don't
   support this, while still ensuring that whatever the size of size_t (eg 32,
   64 bit Linux builds, or Win32/64 builds), a size_t (or IMG_SIZE_T) can be
   passed to printf-type functions without a cast.
*/
#if defined LINUX
	/* Use C99 format specifier where possible */
	#define SIZE_T_FMT_LEN "z"
#elif  defined _WIN64
	#define SIZE_T_FMT_LEN "I"
#else
	#define SIZE_T_FMT_LEN "l" /* May need to be updated as required, for other OSs */
#endif


#if defined (__x86_64__)
	#define IMG_UINT64_FMT "l"
#else
	#define IMG_UINT64_FMT "ll" /* May need to be updated as required, for other OSs */
#endif


#endif /* #if !defined (__IMG_DEFS_H__) */
/*****************************************************************************
 End of file (IMG_DEFS.H)
*****************************************************************************/
