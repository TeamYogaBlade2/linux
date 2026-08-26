# SPDX-License-Identifier: GPL-2.0-only
prismrv-y := \
	prismrv_drv.o \
	prismrv_device.o \
	prismrv_errata.o \
	prismrv_fw.o \
	prismrv_init.o \
	prismrv_mmu.o \
	prismrv_gem.o \
	prismrv_submit.o \
	prismrv_irq.o \
	prismrv_devfreq.o

# the driver includes its uapi header as <uapi/drm/prismrv_drm.h>;
# point the search path at include/ so that resolves in external/M=
# builds as well
ccflags-y += -I$(srctree)/$(src)

obj-$(CONFIG_DRM_PRISMRV) += prismrv.o
