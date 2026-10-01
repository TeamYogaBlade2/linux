/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PVR_DRM_DISPLAY_H__
#define __PVR_DRM_DISPLAY_H__

struct platform_device;

#if defined(CONFIG_DRM)
int pvr_drm_display_init(struct platform_device *pdev);
void pvr_drm_display_fini(struct platform_device *pdev);
#else
static inline int pvr_drm_display_init(struct platform_device *pdev)
{
	return 0;
}
static inline void pvr_drm_display_fini(struct platform_device *pdev)
{
}
#endif

#endif /* __PVR_DRM_DISPLAY_H__ */
