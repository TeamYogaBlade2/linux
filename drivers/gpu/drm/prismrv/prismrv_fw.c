// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * prismrv_fw.c — uKernel microcode loading.
 *
 * The blob is uploaded into GPU memory by prismrv_hw_init(); the register
 * init script (prismrv-init.bin) is consumed separately in prismrv_init.c.
 * Patch slots in the image are resolved by the userspace tooling before
 * installation.
 */
#include <linux/firmware.h>
#include <linux/dma-mapping.h>

#include "prismrv_device.h"

#define FW_NAME_INIT		"mediatek/mt6589-sgx544-init.bin"
#define FW_NAME_UKERNEL		"mediatek/mt6589-sgx544-ukernel.bin"

int prismrv_fw_load(struct prismrv_device *pv)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, FW_NAME_UKERNEL, pv->drm.dev);
	if (ret) {
		dev_err(pv->drm.dev, "failed to load %s (%d)\n",
			FW_NAME_UKERNEL, ret);
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

	dev_info(pv->drm.dev, "loaded uKernel image (%zu bytes)\n",
		 pv->ukernel_size);

	return 0;
}
