/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_MFD_MT6628_PWRSEQ_H
#define __LINUX_MFD_MT6628_PWRSEQ_H

#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/of.h>
#include <linux/types.h>

struct mt6628_pwrseq;

/*
 * The returned handle holds a consumer reference until @dev is released.
 * When "mediatek,combo-pwrseq" is absent, return NULL so that transports
 * which do not use this board-specific power controller remain supported.
 */
#if IS_REACHABLE(CONFIG_PWRSEQ_MT6628)
struct mt6628_pwrseq *devm_mt6628_pwrseq_get(struct device *dev);
int mt6628_pwrseq_recover(struct mt6628_pwrseq *pwrseq);
void mt6628_pwrseq_set_stp_ready(struct mt6628_pwrseq *pwrseq, bool ready);
bool mt6628_pwrseq_is_stp_ready(struct mt6628_pwrseq *pwrseq);
#else
static inline struct mt6628_pwrseq *
devm_mt6628_pwrseq_get(struct device *dev)
{
	if (dev->of_node &&
	    of_find_property(dev->of_node, "mediatek,combo-pwrseq", NULL))
		return ERR_PTR(-EOPNOTSUPP);

	return NULL;
}

static inline int mt6628_pwrseq_recover(struct mt6628_pwrseq *pwrseq)
{
	return -EOPNOTSUPP;
}

static inline void
mt6628_pwrseq_set_stp_ready(struct mt6628_pwrseq *pwrseq, bool ready)
{
}

static inline bool
mt6628_pwrseq_is_stp_ready(struct mt6628_pwrseq *pwrseq)
{
	return false;
}
#endif

#endif /* __LINUX_MFD_MT6628_PWRSEQ_H */
