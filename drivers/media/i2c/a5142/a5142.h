/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A5142 camera sensor driver
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * The A5142 is a 5 megapixel RAW sensor with a 2-lane MIPI CSI-2 output.
 * It is the main (rear) camera of the Lenovo YOGA Tablet 8/10 (MT6589).
 */

#ifndef _A5142_H
#define _A5142_H

#include <linux/bits.h>
#include <linux/gpio/consumer.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-subdev.h>

struct a5142_mode_info {
	unsigned int width;
	unsigned int height;
	unsigned int hts;
	unsigned int vts;
	u32 pix_rate;
	const struct reg_sequence *regs;
	size_t num_regs;
};

enum a5142_mode {
	A5142_MODE_PREVIEW = 0,
	A5142_MODE_CAPTURE,
};

struct a5142 {
	struct i2c_client *client;
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct mutex lock;

	struct gpio_desc *pwdn;
	struct gpio_desc *reset;

	struct a5142_mode_info *cur_mode;
	u32 hts;
	u32 vts;
	u32 integration_time_min;

	bool streaming;
};

#define to_a5142(sd) container_of(sd, struct a5142, sd)

#endif /* _A5142_H */
