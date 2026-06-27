// SPDX-License-Identifier: MIT OR GPL-2.0-only

#ifndef _SGXERRATA_KM_H_
#define _SGXERRATA_KM_H_

/* ignore warnings about unrecognised preprocessing directives in conditional inclusion directives */
/* PRQA S 3115 ++ */

#if defined(SGX520) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX520 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 111
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		/* RTL head - no BRNs to apply */
	#else
		#error "sgxerrata.h: SGX520 Core Revision unspecified"
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if defined(SGX530) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX530 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 120
		#define FIX_HW_BRN_22934/* Workaround in sgx featuredefs */
		#define FIX_HW_BRN_28889/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == 121
		#define FIX_HW_BRN_22934/* Workaround in sgx featuredefs */
		#define FIX_HW_BRN_28889/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == 125
		#define FIX_HW_BRN_22934/* Workaround in sgx featuredefs */
		#define FIX_HW_BRN_28889/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == 130
		#define FIX_HW_BRN_22934/* Workaround in sgx featuredefs */
		#define FIX_HW_BRN_28889/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		/* RTL head - no BRNs to apply */
	#else
		#error "sgxerrata.h: SGX530 Core Revision unspecified"
	#endif
	#endif
	#endif
#endif
        #endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if defined(SGX531) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX531 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 110
		#define FIX_HW_BRN_34028/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		/* RTL head - no BRNs to apply */
	#else
		#error "sgxerrata.h: SGX531 Core Revision unspecified"
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if (defined(SGX535) || defined(SGX535_V1_1)) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX535 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 121
		#define FIX_HW_BRN_22934/* Workaround in sgx featuredefs */
		#define FIX_HW_BRN_23944/* Workaround in code (services) */
		#define FIX_HW_BRN_23410/* Workaround in code (services) and ucode */
	#else
	#if SGX_CORE_REV == 126
		#define FIX_HW_BRN_22934/* Workaround in sgx featuredefs */
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		/* RTL head - no BRNs to apply */
	#else
		#error "sgxerrata.h: SGX535 Core Revision unspecified"

	#endif
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if defined(SGX540) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX540 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 120
		#define FIX_HW_BRN_26620/* Workaround in services (srvkm) */
		#define FIX_HW_BRN_28011/* Workaround in services (srvkm) */
		#define FIX_HW_BRN_34028/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == 121
		#define FIX_HW_BRN_28011/* Workaround in services (srvkm) */
		#define FIX_HW_BRN_34028/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == 130
		#define FIX_HW_BRN_34028/* Workaround in services (srvkm) */
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		/* RTL head - no BRNs to apply */
	#else
		#error "sgxerrata.h: SGX540 Core Revision unspecified"
	#endif
	#endif
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif


#if defined(SGX543) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX543 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 122
		#define FIX_HW_BRN_29954/* turns off regbank split feature */
		#define FIX_HW_BRN_29997/* workaround in services */
		#define FIX_HW_BRN_30954/* workaround in services */
		#define FIX_HW_BRN_31093/* workaround in services */
		#define FIX_HW_BRN_31195/* workaround in services */
		#define FIX_HW_BRN_31272/* workaround in services (srvclient) and uKernel */
		#define FIX_HW_BRN_31278/* disabled prefetching in MMU */
		#define FIX_HW_BRN_31542/* workaround in uKernel and Services */
		#define FIX_HW_BRN_31620/* workaround in services */
		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#define FIX_HW_BRN_32044 /* workaround in uKernel, services and client drivers */
		#define FIX_HW_BRN_32085 /* workaround in services: prefetch fix applied, investigating PT based fix */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_33920/* workaround in ukernel */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
			/* add BRNs here */
	#else
	#if SGX_CORE_REV == 1221
		#define FIX_HW_BRN_29954/* turns off regbank split feature */
        #define FIX_HW_BRN_31195/* workaround in services */
		#define FIX_HW_BRN_31272/* workaround in services (srvclient) and uKernel */
		#define FIX_HW_BRN_31278/* disabled prefetching in MMU */
		#define FIX_HW_BRN_31542/* workaround in uKernel and Services */
		#define FIX_HW_BRN_31671/* workaround in uKernel */
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#define FIX_HW_BRN_32044/* workaround in uKernel, services and client drivers */
		#define FIX_HW_BRN_32085 /* workaround in services: prefetch fix applied, investigating PT based fix */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_33920/* workaround in ukernel */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
			/* add BRNs here */
	#else
	#if SGX_CORE_REV == 141
		#define FIX_HW_BRN_29954/* turns off regbank split feature */
		#define FIX_HW_BRN_31671 /* workaround in uKernel */
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
			/* add BRNs here */
	#else
	#if SGX_CORE_REV == 142
		#define FIX_HW_BRN_29954/* turns off regbank split feature */
		#define FIX_HW_BRN_31671 /* workaround in uKernel */
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
			/* add BRNs here */
	#else
	#if SGX_CORE_REV == 213
		#define FIX_HW_BRN_31272/* workaround in services (srvclient) and uKernel */
		#define FIX_HW_BRN_31671 /* workaround in uKernel */
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#define FIX_HW_BRN_32085 /* workaround in services: prefetch fix applied, investigating PT based fix */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_33920/* workaround in ukernel */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
			/* add BRNs here */
	#else
	#if SGX_CORE_REV == 216
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 303
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
		#if defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_43863 /* Workaround in ukernel */
		#endif
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
	#else
		#error "sgxerrata.h: SGX543 Core Revision unspecified"
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if defined(SGX544) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX544 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 104
		#define FIX_HW_BRN_29954/* turns off regbank split feature */
		#define FIX_HW_BRN_31093/* workaround in services */
		#define FIX_HW_BRN_31195/* workaround in services */
		#define FIX_HW_BRN_31272/* workaround in services (srvclient) and uKernel */
		#define FIX_HW_BRN_31278/* disabled prefetching in MMU */
		#define FIX_HW_BRN_31542 /* workaround in uKernel and Services */
 		#define FIX_HW_BRN_31620/* workaround in services */
		#define FIX_HW_BRN_31671 /* workaround in uKernel */
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#define FIX_HW_BRN_32044 /* workaround in uKernel, services and client drivers */
		#define FIX_HW_BRN_32085 /* workaround in services: prefetch fix applied, investigating PT based fix */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_33920/* workaround in ukernel */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 105
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_33920/* workaround in ukernel */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 112
		#define FIX_HW_BRN_31272/* workaround in services (srvclient) and uKernel */
		#define FIX_HW_BRN_33920/* workaround in ukernel */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 114
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
	#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 115
 		#define FIX_HW_BRN_31780/* workaround in uKernel */
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 116
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel */
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 117
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel */
		#endif
		#define FIX_HW_BRN_33809/* workaround in kernel (enable burst combiner) */
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
	#else
	#if SGX_CORE_REV == 118
		#define FIX_HW_BRN_33920/* workaround in ukernel */
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
	#else
		#error "sgxerrata.h: SGX544 Core Revision unspecified"
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if defined(SGX545) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX545 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 10131
	#else
	#if SGX_CORE_REV == 1014
	#else
	#if SGX_CORE_REV == 10141
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		/* RTL head - no BRNs to apply */
	#else
		#error "sgxerrata.h: SGX545 Core Revision unspecified"
	#endif
	#endif
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if defined(SGX554) && !defined(SGX_CORE_DEFINED)
	/* define the _current_ SGX554 RTL head revision */
	#define SGX_CORE_REV_HEAD	0
	#if defined(USE_SGX_CORE_REV_HEAD)
		/* build config selects Core Revision to be the Head */
		#define SGX_CORE_REV	SGX_CORE_REV_HEAD
	#endif

	#if SGX_CORE_REV == 1251
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
		#define FIX_HW_BRN_36513 /* workaround in uKernel and Services */
		/* add BRNs here */
	#else
	#if SGX_CORE_REV == SGX_CORE_REV_HEAD
		#if defined(SUPPORT_SGX_LOW_LATENCY_SCHEDULING) && defined(SGX_FEATURE_MP)
			#define FIX_HW_BRN_33657/* workaround in ukernel*/
		#endif
	#else
		#error "sgxerrata.h: SGX554 Core Revision unspecified"
	#endif
	#endif
	/* signal that the Core Version has a valid definition */
	#define SGX_CORE_DEFINED
#endif

#if !defined(SGX_CORE_DEFINED)
#if defined (__GNUC__)
	#warning "sgxerrata.h: SGX Core Version unspecified"
#else
	#pragma message("sgxerrata.h: SGX Core Version unspecified")
#endif
#endif

/* restore warning */
/* PRQA S 3115 -- */

#endif /* _SGXERRATA_KM_H_ */

/******************************************************************************
 End of file (sgxerrata.h)
******************************************************************************/
