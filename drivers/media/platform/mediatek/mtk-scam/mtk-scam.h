/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek MT6589 sensor-CAM (SCAM) adaptor
 *
 * Copyright (c) 2026 Lenovo Linux Team
 */

#ifndef _MTK_SCAM_H
#define _MTK_SCAM_H

#include <linux/mutex.h>
#include <media/v4l2-subdev.h>

/* Two SCAM instances exist, one per camera port. */
#define SCAM_MAX_PORTS	2

/* Pad ids; the DT declares one sink and one source port on this node. */
enum {
	SCAM_PAD_SINK = 0,	/* from the CSI-2 receiver */
	SCAM_PAD_SRC,		/* to the CAM/ISP */
};

/*
 * Geometry seeded into the subdev state before anything has been negotiated.
 * It matches the CSI-2 receiver's own default and the A5142's preview mode,
 * so the graph starts out consistent across the bridge.
 */
#define SCAM_DEFAULT_WIDTH	1280
#define SCAM_DEFAULT_HEIGHT	960

/*
 * The frame size SCAM was told to expect.  It is carried here because
 * SCAM_SIZE is programmed on format change, and the sensor's negotiated
 * format reaches this driver through set_pad() rather than through the
 * CSI-2 capability negotiation this tree does not have.
 */
struct mtk_scam_frame_size {
	u32 width;
	u32 height;
	u32 code;
};

struct mtk_scam {
	struct device *dev;
	void __iomem *regs;

	/* Port 0 is SCAM1 at 0x15008200, port 1 is SCAM2 at 0x15008280. */
	unsigned int port;

	struct v4l2_subdev sd;
	struct mutex lock;

	struct mtk_scam_frame_size size;

	bool streaming;
};

#define to_mtk_scam(sd) container_of(sd, struct mtk_scam, sd)

#endif /* _MTK_SCAM_H */