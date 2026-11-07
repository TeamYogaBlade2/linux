// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_fw.c — uKernel microcode loading.
 *
 * The blob is uploaded into GPU memory by prismrv_hw_init(); the register
 * init script (prismrv-init.bin) is consumed separately in prismrv_init.c.
 * Patch slots in the image are resolved by the userspace tooling before
 * installation.
 *
 * Firmware file names are selected as follows (in priority order):
 *
 *   1. The Device Tree "firmware-name" property (e.g. for OMAP SGX530,
 *      set  firmware-name = "ti/omap-sgx530-ukernel.fw";  in the DT node).
 *   2. The compile-time default for the matched SoC
 *      ("mediatek/mt6589-sgx544-init.bin", "mediatek/mt6589-sgx544-ukernel.fw").
 *
 * The init-script name is derived from the ukernel name by replacing
 * "-ukernel.fw" with "-init.bin".  If "firmware-name" specifies a name
 * that does not end in "-ukernel.fw" the init script is loaded from the
 * same directory with the stem replaced.
 */
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>
#include <linux/array_size.h>
#include <linux/slab.h>
#include <linux/sizes.h>

#include "prismrv_device.h"

/* Default firmware paths for the MT6589 / SGX544 */
#define FW_DEFAULT_INIT		"mediatek/mt6589-sgx544-init.bin"
#define FW_DEFAULT_UKERNEL	"mediatek/mt6589-sgx544-ukernel.fw"

/*
 * prismrv_fw_names() - resolve the firmware file names.
 *
 * Reads the "firmware-name" DT property; falls back to the hardcoded
 * default.  The init-script name is the ukernel name with "-ukernel.fw"
 * replaced by "-init.bin".
 *
 * @pv:          device
 * @out_ukernel: output buffer for the ukernel name (must be >= 128 bytes)
 * @out_init:    output buffer for the init-script name (must be >= 128 bytes)
 */
static void prismrv_fw_names(struct prismrv_device *pv,
			     char *out_ukernel, char *out_init)
{
	const char *dt_name = NULL;
	struct device_node *np = pv->drm.dev->of_node;

	if (np)
		of_property_read_string(np, "firmware-name", &dt_name);

	if (dt_name) {
		strscpy(out_ukernel, dt_name, 128);
	} else {
		strscpy(out_ukernel, FW_DEFAULT_UKERNEL, 128);
	}

	/*
	 * Derive the init-script name:
	 *   foo-ukernel.fw -> foo-init.bin
	 *   foo.bin         -> foo-init.bin
	 *   foo             -> foo-init.bin
	 */
	strscpy(out_init, out_ukernel, 128);
	{
		static const char *const sfx[] = { "-ukernel.fw", "-ukernel.bin", ".fw", ".bin" };
		size_t len = strlen(out_init), sl;
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(sfx); i++) {
			sl = strlen(sfx[i]);
			if (len > sl && !strcmp(out_init + len - sl, sfx[i])) {
				out_init[len - sl] = '\0';
				break;
			}
		}
		strlcat(out_init, "-init.bin", 128);
	}
}


/* find the (first) object with a given role */
struct prismrv_uk_obj *prismrv_uk_find(struct prismrv_device *pv, u32 role)
{
	unsigned int i;

	for (i = 0; i < pv->uk_nobj; i++)
		if (pv->uk_obj[i].role == role)
			return &pv->uk_obj[i];
	return NULL;
}

static void prismrv_uk_free_objs(struct prismrv_device *pv)
{
	unsigned int i;

	for (i = 0; i < pv->uk_nobj; i++)
		if (pv->uk_obj[i].cpu)
			dma_free_coherent(pv->drm.dev, PAGE_ALIGN(pv->uk_obj[i].size),
					  pv->uk_obj[i].cpu, pv->uk_obj[i].dma);
	kfree(pv->uk_obj);
	pv->uk_obj = NULL;
	pv->uk_nobj = 0;
}

/*
 * Parse and allocate the PRVUKFW1 container.  Contents are copied (again) by
 * prismrv_fw_map_objects() on every hw_init so a recovery restarts from the
 * pristine uKernel/PDS/kernel-data state.
 */
static int prismrv_uk_parse(struct prismrv_device *pv, const struct firmware *fw)
{
	const struct prismrv_ukfw_hdr *h = (const void *)fw->data;
	const struct prismrv_ukfw_obj *t;
	unsigned int i, n;
	int ret;

	if (fw->size < sizeof(*h) || memcmp(h->magic, PRISMRV_UKFW_MAGIC, 8) ||
	    le32_to_cpu(h->version) != 1) {
		dev_err(pv->drm.dev, "not a PRVUKFW1 uKernel container\n");
		return -EINVAL;
	}
	n = le32_to_cpu(h->nobj);
	if (!n || n > PRISMRV_UKFW_MAX_OBJS ||
	    fw->size < sizeof(*h) + n * sizeof(*t)) {
		dev_err(pv->drm.dev, "uKernel container: bad object count %u\n", n);
		return -EINVAL;
	}
	t = (const void *)(h + 1);

	pv->uk_obj = kcalloc(n, sizeof(*pv->uk_obj), GFP_KERNEL);
	if (!pv->uk_obj)
		return -ENOMEM;
	pv->uk_nobj = n;

	for (i = 0; i < n; i++) {
		struct prismrv_uk_obj *o = &pv->uk_obj[i];
		u32 va = le32_to_cpu(t[i].va), size = le32_to_cpu(t[i].size);
		u32 off = le32_to_cpu(t[i].data_off);

		if (!size || (va & (PAGE_SIZE - 1)) ||
		    (u64)va + PAGE_ALIGN(size) > SZ_4G ||
		    (u64)off + size > fw->size) {
			dev_err(pv->drm.dev,
				"uKernel container object %u invalid (va %#x size %#x off %#x)\n",
				i, va, size, off);
			ret = -EINVAL;
			goto err;
		}
		o->va = va;
		o->size = size;
		o->role = le32_to_cpu(t[i].role);
		o->data_off = off;
		o->cpu = dma_alloc_coherent(pv->drm.dev, PAGE_ALIGN(size),
					    &o->dma, GFP_KERNEL);
		if (!o->cpu) {
			ret = -ENOMEM;
			goto err;
		}
	}
	return 0;
err:
	prismrv_uk_free_objs(pv);
	return ret;
}

/*
 * Restore every object to its pristine contents and (re)map it at the VA the
 * vendor loader assigned.  Called from hw_init after prismrv_mmu_init().
 */
int prismrv_fw_map_objects(struct prismrv_device *pv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pv->uk_nobj; i++) {
		struct prismrv_uk_obj *o = &pv->uk_obj[i];

		memset(o->cpu, 0, PAGE_ALIGN(o->size));
		memcpy(o->cpu, pv->uk_fw->data + o->data_off, o->size);
		ret = prismrv_mmu_map(pv, o->va, o->dma, PAGE_ALIGN(o->size));
		if (ret) {
			dev_err(pv->drm.dev, "mapping uKernel object %u at %#x failed (%d)\n",
				i, o->va, ret);
			return ret;
		}
	}
	return 0;
}


/*
 * uKernel immediate cross-check.  The container is produced by running the vendor loader against a
 * *simulated* heap map, and the loader writes object addresses into `mov rN,#imm32` sites of the
 * uKernel (analysis/UKERNEL_PATCHING.md).  If the object table and those immediates disagree the
 * uKernel would read the wrong structures, silently.  Only sites whose meaning is VERIFIED are
 * enforced; the rest are reported.
 *
 * mov-imm32 layout: imm[20:0] = w[20:0], imm[25:21] = w[40:36], imm[31:26] = w[49:44].
 */
static u32 prismrv_uk_imm(const void *img, u32 instr)
{
	u64 w = le64_to_cpu(((const __le64 *)img)[instr]);

	return (u32)(w & 0x1fffff) | (u32)(((w >> 36) & 0x1f) << 21) | (u32)(((w >> 44) & 0x3f) << 26);
}

static int prismrv_fw_check_immediates(struct prismrv_device *pv)
{
	const struct prismrv_uk_obj *uk = prismrv_uk_find(pv, PRISMRV_UKR_UKERNEL);
	const struct prismrv_uk_obj *ccb = prismrv_uk_find(pv, PRISMRV_UKR_CCB);
	const struct prismrv_uk_obj *ta3d = prismrv_uk_find(pv, PRISMRV_UKR_TA3DCTL);
	const struct prismrv_uk_obj *hc = prismrv_uk_find(pv, PRISMRV_UKR_HOSTCTL);
	const u8 *img;
	int err = 0;

	if (!uk || !ccb || !ta3d || !hc || uk->size < 8 * 1294)
		return -EINVAL;
	img = (const u8 *)pv->uk_fw->data + uk->data_off;

	/* VERIFIED: instr 1255 = kernel CCB VA, instr 1193 = TA3DCtl VA (loader patch list, analysis/UKERNEL_IMMEDIATES.md) */
	if (prismrv_uk_imm(img, 1255) != ccb->va) {
		dev_err(pv->drm.dev, "uKernel instr 1255 = %#x but the kernel CCB is at %#x\n",
			prismrv_uk_imm(img, 1255), ccb->va);
		err = -EINVAL;
	}
	if (prismrv_uk_imm(img, 1193) != ta3d->va) {
		dev_err(pv->drm.dev, "uKernel instr 1193 = %#x but TA3DCtl is at %#x\n",
			prismrv_uk_imm(img, 1193), ta3d->va);
		err = -EINVAL;
	}
	/*
	 * ASSUMED (not verified): instr 1190 carries the HostCtl address in some word form.  The real-device capture has
	 * 0x00101000 (= HostCtl at 0x00101000), this container 0x01001000 with HostCtl at hc->va.  Warn, do not refuse.
	 */
	if (prismrv_uk_imm(img, 1190) != (hc->va >> 4) && prismrv_uk_imm(img, 1190) != hc->va)
		dev_warn(pv->drm.dev,
			 "uKernel instr 1190 = %#x does not match HostCtl VA %#x (>>4 %#x): container is for a simulated layout, not verified on hardware\n",
			 prismrv_uk_imm(img, 1190), hc->va, hc->va >> 4);
	return err;
}

int prismrv_fw_load(struct prismrv_device *pv)
{
	char fw_ukernel[128];
	char fw_init[128];
	const struct firmware *fw;
	struct prismrv_uk_obj *uk;
	int ret;

	prismrv_fw_names(pv, fw_ukernel, fw_init);

	/* store derived init-script name for prismrv_hw_init() */
	strscpy(pv->fw_init_name, fw_init, sizeof(pv->fw_init_name));

	ret = request_firmware(&fw, fw_ukernel, pv->drm.dev);
	if (ret) {
		dev_err(pv->drm.dev, "failed to load %s (%d)\n",
			fw_ukernel, ret);
		return ret;
	}

	ret = prismrv_uk_parse(pv, fw);
	if (ret) {
		release_firmware(fw);
		return ret;
	}
	pv->uk_fw = fw;

	uk = prismrv_uk_find(pv, PRISMRV_UKR_UKERNEL);
	if (!uk || !prismrv_uk_find(pv, PRISMRV_UKR_CCB) ||
	    !prismrv_uk_find(pv, PRISMRV_UKR_CCBCTL) ||
	    !prismrv_uk_find(pv, PRISMRV_UKR_HOSTCTL)) {
		dev_err(pv->drm.dev, "%s: uKernel/CCB/CCBCtl/HostCtl object missing\n",
			fw_ukernel);
		prismrv_fw_release(pv);
		return -EINVAL;
	}
	ret = prismrv_fw_check_immediates(pv);
	if (ret) {
		dev_err(pv->drm.dev, "%s: object table and uKernel immediates disagree; refusing to start\n", fw_ukernel);
		prismrv_fw_release(pv);
		return ret;
	}
	pv->ukernel_size = uk->size;

	dev_info(pv->drm.dev, "loaded %u uKernel objects from %s (uKernel %u bytes at %#x)\n",
		 pv->uk_nobj, fw_ukernel, uk->size, uk->va);
	return 0;
}

void prismrv_fw_release(struct prismrv_device *pv)
{
	prismrv_uk_free_objs(pv);
	if (pv->uk_fw) {
		release_firmware(pv->uk_fw);
		pv->uk_fw = NULL;
	}
	pv->ukernel_size = 0;
}
