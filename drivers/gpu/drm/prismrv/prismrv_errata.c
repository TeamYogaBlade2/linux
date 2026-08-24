// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_errata.c — hardware errata (BRN) handling.
 *
 * The SGX family ships several RTL revisions per core type.  Some revisions
 * require software workarounds, identified by BRN numbers.  The table below
 * mirrors the associations between core revisions and workarounds that the
 * vendor kernel applies (services4/srvkm/hwdefs/sgxerrata.h), expressed as
 * runtime data instead of compile-time #ifdefs so that a single kernel image
 * can drive any revision.
 */
#include "prismrv_device.h"

/* BRN bit assignments */
#define PRISMRV_BRN_29954	BIT(0)	/* disable regbank split */
#define PRISMRV_BRN_31093	BIT(1)
#define PRISMRV_BRN_31195	BIT(2)
#define PRISMRV_BRN_31272	BIT(3)
#define PRISMRV_BRN_31542	BIT(4)
#define PRISMRV_BRN_31620	BIT(5)
#define PRISMRV_BRN_31671	BIT(6)
#define PRISMRV_BRN_31780	BIT(7)	/* PTLA write-back workaround */
#define PRISMRV_BRN_32044	BIT(8)
#define PRISMRV_BRN_32085	BIT(9)
#define PRISMRV_BRN_33920	BIT(10)
#define PRISMRV_BRN_36513	/* clear-clip WA: extra buffers */ \
				BIT(11)

struct prismrv_errata_entry {
	u32 core_id;
	u32 rev;		/* EUR_CR_CORE_REVISION major value */
	u32 brns;
};

static const struct prismrv_errata_entry prismrv_errata_table[] = {
	/* SGX544 revisions */
	{ PRISMRV_CORE_SGX544, 104,
	  PRISMRV_BRN_29954 | PRISMRV_BRN_31093 | PRISMRV_BRN_31195 |
	  PRISMRV_BRN_31272 | PRISMRV_BRN_31542 | PRISMRV_BRN_31620 |
	  PRISMRV_BRN_31671 | PRISMRV_BRN_31780 | PRISMRV_BRN_32044 |
	  PRISMRV_BRN_32085 | PRISMRV_BRN_33920 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 105,
	  PRISMRV_BRN_31780 | PRISMRV_BRN_33920 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 112,
	  PRISMRV_BRN_31272 | PRISMRV_BRN_33920 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 114,
	  PRISMRV_BRN_31780 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 115,
	  PRISMRV_BRN_31780 | PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 116,
	  PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 117,
	  PRISMRV_BRN_36513 },
	{ PRISMRV_CORE_SGX544, 118,
	  PRISMRV_BRN_33920 },
};

/**
 * prismrv_read_revision() — read the core revision registers.
 *
 * EUR_CR_CORE_REVISION layout:
 *   [31:24] designer   [23:16] major   [15:8] minor   [7:0] maintenance
 *
 * The major field is the RTL head revision the vendor driver keys its
 * errata tables on (e.g. 115 for MT6589).
 */
u32 prismrv_read_revision(struct prismrv_device *pv)
{
	u32 val = readl(pv->regs + EUR_CR_CORE_REVISION);

	pv->core_rev_major = (val >> EUR_CR_CORE_REVISION_MAJOR_SHIFT) & 0xff;
	pv->core_rev_minor = (val >> EUR_CR_CORE_REVISION_MINOR_SHIFT) & 0xff;

	return pv->core_rev_major;
}

void prismrv_errata_init(struct prismrv_device *pv)
{
	unsigned int i;

	pv->errata = 0;

	for (i = 0; i < ARRAY_SIZE(prismrv_errata_table); i++) {
		const struct prismrv_errata_entry *e = &prismrv_errata_table[i];

		if (e->core_id == pv->info->core_id &&
		    e->rev == pv->core_rev_major) {
			pv->errata = e->brns;
			break;
		}
	}

	dev_info(pv->drm.dev, "%s rev %u.%u: active errata mask %#x\n",
		 pv->info->name, pv->core_rev_major, pv->core_rev_minor,
		 pv->errata);
}
