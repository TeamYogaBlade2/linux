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
	 * Derive the init-script name: replace "-ukernel.bin" suffix with
	 * "-init.bin".  If the suffix is absent, append "-init.bin" to the
	 * stem so we still produce a useful path.
	 */
	strscpy(out_init, out_ukernel, 128);
	{
		char *suffix = strstr(out_init, "-ukernel.bin");

		if (suffix)
			strcpy(suffix, "-init.bin");
		else
			strlcat(out_init, "-init.bin", 128);
	}
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
