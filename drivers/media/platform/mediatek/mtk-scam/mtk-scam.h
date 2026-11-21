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

/*
 * Pad ids; the DT declares one sink and one source port on this node.
 *
 * The order is load-bearing in three places at once, so it is fixed here and
 * not re-derived anywhere:
 *
 *   - the DT, whose scam_in is port@0 and scam_out is port@1.  A port's
 *     reg-names index is its pad index, resolved by the core's default
 *     v4l2_subdev_get_fwnode_pad_1_to_1();
 *   - pads[] below, which must be in the same order, because
 *     media_entity_pads_init() assigns each pad the index it has in that
 *     array and every pad_ops handler in mtk-scam.c indexes by pad number;
 *   - the pad_ops themselves, which walk SCAM_PAD_SINK..SCAM_PAD_SRC
 *     inclusively to keep the two ends of the bridge in step.
 *
 * SCAM_PAD_NUM exists so those loops and the pads_init() call cannot disagree
 * about how many pads there are.
 */
enum {
	SCAM_PAD_SINK = 0,	/* from the CSI-2 receiver */
	SCAM_PAD_SRC,		/* to the CAM/ISP */
	SCAM_PAD_NUM,
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

	/*
	 * The media pads, indexed exactly as the enum above.  SCAM is a
	 * bridge, so it has one of each: the receiver's output arrives on the
	 * sink and the bytes leave towards the CAM/ISP on the source.  The
	 * flags are filled in in probe, before media_entity_pads_init() is
	 * called, which is what gives each pad its index.
	 */
	struct media_pad pads[SCAM_PAD_NUM];

	struct mtk_scam_frame_size size;

	bool streaming;
};

#define to_mtk_scam(sd) container_of(sd, struct mtk_scam, sd)

#endif /* _MTK_SCAM_H */