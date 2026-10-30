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
 *      set  firmware-name = "ti/omap-sgx530-ukernel.bin";  in the DT node).
 *   2. The compile-time default for the matched SoC
 *      ("mediatek/mt6589-sgx544-{init,ukernel}.bin").
 *
 * The init-script name is derived from the ukernel name by replacing
 * "-ukernel.bin" with "-init.bin".  If "firmware-name" specifies a name
 * that does not end in "-ukernel.bin" the init script is loaded from the
 * same directory with the stem replaced.
 */
#include <linux/firmware.h>
#include <linux/dma-mapping.h>
#include <linux/of.h>

#include "prismrv_device.h"

/* Default firmware paths for the MT6589 / SGX544 */
#define FW_DEFAULT_INIT		"mediatek/mt6589-sgx544-init.bin"
#define FW_DEFAULT_UKERNEL	"mediatek/mt6589-sgx544-ukernel.bin"

/*
 * prismrv_fw_names() - resolve the firmware file names.
 *
 * Reads the "firmware-name" DT property; falls back to the hardcoded
 * default.  The init-script name is the ukernel name with "-ukernel.bin"
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
	 *   foo-ukernel.bin -> foo-init.bin
	 *   foo.bin         -> foo-init.bin
	 *   foo             -> foo-init.bin
	 */
	strscpy(out_init, out_ukernel, 128);
	{
		size_t len = strlen(out_init);
		static const char uk[] = "-ukernel.bin";
		static const char bin[] = ".bin";

		if (len > strlen(uk) && !strcmp(out_init + len - strlen(uk), uk))
			out_init[len - strlen(uk)] = '\0';
		else if (len > strlen(bin) &&
			 !strcmp(out_init + len - strlen(bin), bin))
			out_init[len - strlen(bin)] = '\0';
		strlcat(out_init, "-init.bin", 128);
	}
}


/*
 * `mov rN, #imm32` (opcode byte 0xfc, bits [55:52] == 2) scatters its
 * immediate over the 64-bit instruction word:
 *	imm[20:0]  = word[20:0]
 *	imm[25:21] = word[40:36]
 *	imm[31:26] = word[49:44]
 * (solved from 2114 instructions of the stock uKernel against the vendor
 * disassembler; prismrv analysis/UKERNEL_IMMEDIATES.md).  The stock image
 * carries 0xDEADBEEF placeholders that the vendor loader replaces.  A tool
 * that rewrote only the low dword left imm[31:21] at 0xDEA, i.e. values
 * such as 0xDEB01000.  Report what is still placeholder-shaped.
 */
static u32 prismrv_mov_imm32(u64 w)
{
	return (u32)(w & 0x1fffff) | (u32)(((w >> 36) & 0x1f) << 21) |
	       (u32)(((w >> 44) & 0x3f) << 26);
}

static void prismrv_fw_check_placeholders(struct prismrv_device *pv,
					  const void *data, size_t size)
{
	const __le64 *w = data;
	unsigned int raw = 0, partial = 0;
	size_t i;

	for (i = 0; i < size / 8; i++) {
		u64 v = le64_to_cpu(w[i]);
		u32 imm;

		if ((v >> 56) != 0xfc || ((v >> 52) & 0xf) != 2)
			continue;
		imm = prismrv_mov_imm32(v);
		if (imm == 0xdeadbeef)
			raw++;
		else if ((imm >> 21) == (0xdea00000u >> 21))
			partial++;
	}
	if (raw)
		dev_warn(pv->drm.dev,
			 "uKernel has %u unpatched 0xDEADBEEF immediates; the loader patch step (DEVINITPART2 equivalent) is missing\n", raw);
	if (partial)
		dev_warn(pv->drm.dev,
			 "uKernel has %u immediates whose upper bits are still 0xDEA (partially patched image)\n", partial);
}

int prismrv_fw_load(struct prismrv_device *pv)
{
	char fw_ukernel[128];
	char fw_init[128];
	const struct firmware *fw;
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

	/*
	 * Sanity-check the image before it is DMA-mapped into the GPU:
	 * the uKernel is a stream of 64-bit USSE instructions and must
	 * fit the region reserved for it in the GPU VA layout (up to the
	 * next fixed carve-out).
	 */
	if (!fw->size || fw->size % 8 || fw->size > PRISMRV_UKERNEL_MAX_SIZE) {
		dev_err(pv->drm.dev,
			"%s: bad uKernel size %zu (need non-zero multiple of 8, <= %u)\n",
			fw_ukernel, fw->size, PRISMRV_UKERNEL_MAX_SIZE);
		release_firmware(fw);
		return -EINVAL;
	}

	prismrv_fw_check_placeholders(pv, fw->data, fw->size);

	pv->ukernel_size = fw->size;
	pv->ukernel_cpu = dma_alloc_coherent(pv->drm.dev, fw->size,
					     &pv->ukernel_dma, GFP_KERNEL);
	if (!pv->ukernel_cpu) {
		release_firmware(fw);
		return -ENOMEM;
	}
	memcpy(pv->ukernel_cpu, fw->data, fw->size);
	release_firmware(fw);

	dev_info(pv->drm.dev, "loaded uKernel image (%zu bytes) from %s\n",
		 pv->ukernel_size, fw_ukernel);

	return 0;
}

void prismrv_fw_release(struct prismrv_device *pv)
{
	if (pv->ukernel_cpu) {
		dma_free_coherent(pv->drm.dev, pv->ukernel_size,
				  pv->ukernel_cpu, pv->ukernel_dma);
		pv->ukernel_cpu = NULL;
		pv->ukernel_size = 0;
		pv->ukernel_dma  = 0;
	}
}
