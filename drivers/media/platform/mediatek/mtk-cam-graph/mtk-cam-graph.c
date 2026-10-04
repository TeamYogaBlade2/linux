// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 camera media-graph owner
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * WHAT THIS DRIVER IS FOR
 * -----------------------
 * The three blocks in the camera chain -- mtk-csi2-rx, mtk-scam and mtk-cam --
 * each register a subdev, and the dtsi wires them together with
 * "remote-endpoint" properties.  That is not enough to build a media graph.
 * Something has to own:
 *
 *   - a struct media_device, which is where media_device_register_entity()
 *     files the entities and where media-ctl reports them;
 *   - a struct v4l2_device, because the async notifier only binds subdevs
 *     when the root notifier has one;
 *   - a struct v4l2_async_notifier, which is the mechanism that actually
 *     pulls a subdev into a v4l2_device, and therefore the mechanism that
 *     registers the entities into the media_device;
 *   - the media_links themselves, derived from the DT endpoints.
 *
 * Before this driver none of that existed anywhere on the camera path.  The
 * subdevs registered as async subdevs that nothing was waiting for, so no
 * entity joined a media_device and no media_link was ever created.  The
 * "receiver -> SCAM -> CAM resolves" claim in the three drivers' comments was
 * not established by anything in the tree; the pads existed and the DT
 * endpoints pointed at each other, but a media_link is a runtime object that
 * has to be created, not something the DT creates on its own.
 *
 * WHY A SEPARATE DRIVER RATHER THAN ONE OF THE THREE
 * --------------------------------------------------
 * The notifier must be owned by a device that is not itself one of the three
 * blocks, for two reasons:
 *
 *  1. The notifier has to be re-registered (v4l2_async_nf_register() retries
 *     every waiting match against the subdevs already present) every time a
 *     new subdev shows up.  Binding the notifier to one of the hardware
 *     blocks would make that block's probe order decide when the rest of the
 *     chain joins the graph.
 *
 *  2. On unbind, v4l2_async_nf_unregister() plus media_device_unregister()
 *     take the whole graph down.  If one of the three blocks owned them,
 *     unbinding that one block -- for instance a suspend or a driver
 *     unbind -- would tear down links the other two still need.
 *
 * So this driver owns nothing but the graph.  It is pure plumbing.
 *
 * HOW THE LINKS ARE CREATED
 * -------------------------
 * From the DT, via the endpoints.  v4l2_async_nf_add_fwnode_remote() is given
 * each of this driver's own local endpoints; for each one it resolves the
 * remote endpoint in the DT and waits for the subdev that owns it.  When that
 * subdev registers, .bound() fires with the matched subdev and calls
 * v4l2_create_fwnode_links_to_pad(), which walks the subdev's own endpoints
 * with fwnode_graph_for_each_endpoint(), resolves each to a source pad via
 * media_entity_get_fwnode_pad(), and creates the media_link.  No link is ever
 * hardcoded here: the wiring is exactly what the dtsi says.
 *
 * The receiver's SINK pad is deliberately not handled and must not be.  There
 * is no sensor node and no D-PHY node in the tree, so that pad has no peer:
 * its remote endpoint does not resolve, v4l2_create_fwnode_links_to_pad()
 * finds no source endpoint for it and simply creates nothing.  This is
 * documented rather than treated as an error, because it is the documented
 * state of the DT (see the long note on the receiver node in
 * mt6589-lenovo-blade-camera.dtsi).  When a sensor lands, its source endpoint
 * will resolve and the link will be created with no change here.
 *
 * WHY CAM's MISSING pad_ops AND VIDEO NODE DO NOT BREAK LINK CREATION
 * ------------------------------------------------------------------
 * They do not, and it is worth being explicit because it looks like it might:
 *
 *  - media_create_pad_link() checks only the pad direction flags
 *    (MEDIA_PAD_FL_SOURCE on the source, MEDIA_PAD_FL_SINK on the sink).  It
 *    does not consult pad_ops, and it does not require a video node.
 *  - media_entity_get_fwnode_pad() needs entity->ops->get_fwnode_pad only if
 *    the entity set one; CAM sets none, so it falls back to "the first pad
 *    with the requested direction", which is the single sink pad.  That is
 *    correct for CAM precisely because it has exactly one pad of that
 *    direction.
 *  - The .complete() callback calls v4l2_device_register_subdev_nodes(),
 *    which creates /dev/v4l-subdevN nodes.  Each of the three subdevs must
 *    therefore set V4L2_SUBDEV_FL_HAS_DEVNODE to get one.  None of them does,
 *    so today this call registers nothing; it is kept because it is the
 *    correct place for it and it becomes meaningful the moment a subdev opts
 *    in.  It is not an error either way.
 *
 * See README.md for what this graph does and does not deliver.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-mc.h>
#include <media/v4l2-subdev.h>

#define MTK_CAM_GRAPH_DRV_NAME	"mtk-cam-graph"

/*
 * A connection descriptor.  v4l2_async_connection must be the first member:
 * the core allocates a struct of this size and treats it as one of those.
 */
struct mtk_cam_graph_conn {
	struct v4l2_async_connection asc;
};

struct mtk_cam_graph {
	struct device *dev;
	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;

	/*
	 * Number of local endpoints we asked the notifier to wait for.  Kept
	 * so the DT scan and the teardown can agree; there is no per-connection
	 * array to walk because the core owns the waiting_list.
	 */
	unsigned int num_eps;
};

/*
 * A subdev bound to one of our endpoints.
 *
 * Nothing has to be created here.  By the time this runs,
 * v4l2_async_match_notify() has already called
 * __v4l2_device_register_subdev(), which is what registers this subdev's
 * entity into the media_device and creates the gobjs for its pads.  So the
 * subdev is a full participant in the graph by this point; the only thing
 * left is the links, and those are created once, in .complete(), from the DT.
 *
 * Returning 0 is therefore correct and complete for this callback.
 */
static int mtk_cam_graph_bound(struct v4l2_async_notifier *notifier,
			       struct v4l2_subdev *sd,
			       struct v4l2_async_connection *asc)
{
	struct mtk_cam_graph *cam =
		container_of(notifier, struct mtk_cam_graph, notifier);

	/*
	 * The receiver's sink pad has no peer -- there is no sensor node and no
	 * D-PHY node in the tree -- so an endpoint may well resolve to a subdev
	 * whose own remote endpoint does not resolve back to anything.  That is
	 * the documented state of the DT, not a failure: report it and move on,
	 * because the links are derived from the DT in .complete() and a missing
	 * peer simply yields no link.
	 */
	if (list_empty(&sd->asc_list))
		dev_dbg(cam->dev, "%s bound with no connections\n", sd->name);

	return 0;
}

/*
 * Create the media links for the whole graph, from the DT.
 *
 * This runs once, after every endpoint's peer has bound, which is the only
 * point at which all the entities are registered into the media_device and a
 * link between any two of them can be created.
 *
 * The links come from the DT, not from a table here.  For every ordered pair
 * of bound subdevs (source candidate A, sink candidate B) and for each of B's
 * sink pads, v4l2_create_fwnode_links_to_pad(A, sink, 0) walks A's own
 * endpoints with fwnode_graph_for_each_endpoint(), resolves each to a source
 * pad on A, resolves that endpoint's remote in the DT, and creates the
 * media_link when the remote turns out to be B's sink pad.  So what gets
 * created is exactly the wiring the dtsi declares:
 *
 *	seninf_out -> scam_in
 *	scam_out  -> cam_in
 *
 * with nothing hardcoded.
 *
 * The double loop is O(n^2) in the number of bound subdevs, which is three
 * here, and it is how this is done for graphs of this size elsewhere (the
 * per-pair form is what amlogic/c3/isp and nxp/imx8mq-mipi-csi2 use).  It is
 * also idempotent: the core skips a link that already exists, so A/B and B/A
 * both being tried does not create anything twice.
 *
 * A source whose remote endpoint does not resolve -- the receiver's unconnected
 * sink pad is the one that exists today -- simply yields no link.  That is
 * documented in the file header and is not an error.
 *
 * CAM's missing pad_ops and missing video node do not affect any of this.
 * See the file header for why.
 */
static int mtk_cam_graph_create_links(struct mtk_cam_graph *cam)
{
	struct v4l2_subdev *src, *sink_sd;
	int ret;

	/*
	 * Walk the v4l2_device's subdev list, not our own connections: a subdev
	 * may have reached us through an endpoint, or we may have reached it,
	 * and only bound subdevs are on this list.  sd->v4l2_dev is our
	 * v4l2_dev for every subdev on it.
	 */
	list_for_each_entry(src, &cam->v4l2_dev.subdevs, list) {
		list_for_each_entry(sink_sd, &cam->v4l2_dev.subdevs, list) {
			unsigned int i;

			for (i = 0; i < sink_sd->entity.num_pads; i++) {
				struct media_pad *sink =
					&sink_sd->entity.pads[i];

				if (!(sink->flags & MEDIA_PAD_FL_SINK))
					continue;

				ret = v4l2_create_fwnode_links_to_pad(src,
								      sink, 0);
				if (ret)
					return ret;
			}
		}
	}

	return 0;
}

/*
 * Every waiting subdev has bound.  Create the links, then register the
 * /dev/v4l-subdevN nodes.
 */
static int mtk_cam_graph_complete(struct v4l2_async_notifier *notifier)
{
	struct mtk_cam_graph *cam =
		container_of(notifier, struct mtk_cam_graph, notifier);
	int ret;

	ret = mtk_cam_graph_create_links(cam);
	if (ret)
		return ret;

	/*
	 * Registers nothing today, because none of the three subdevs sets
	 * V4L2_SUBDEV_FL_HAS_DEVNODE.  This is the correct and only place for the
	 * call, and it becomes meaningful the moment one of them opts in.
	 */
	return v4l2_device_register_subdev_nodes(&cam->v4l2_dev);
}

static const struct v4l2_async_notifier_operations mtk_cam_graph_notifier_ops = {
	.bound = mtk_cam_graph_bound,
	.complete = mtk_cam_graph_complete,
};

/*
 * Walk this node's own endpoints and ask the notifier to wait for whatever
 * each one points at.
 *
 * Returning -ENOTCONN from v4l2_async_nf_add_fwnode_remote() for an endpoint
 * whose remote does not resolve is expected here and must not fail probe: it
 * is how the receiver's unconnected sink pad shows up, and the DT documents
 * that the sensor step is deliberately absent.  So count what resolved and
 * carry on.
 */
static int mtk_cam_graph_add_endpoints(struct mtk_cam_graph *cam)
{
	struct fwnode_handle *fwnode = dev_fwnode(cam->dev);
	struct fwnode_handle *endpoint;
	int ret = 0;

	fwnode_graph_for_each_endpoint(fwnode, endpoint) {
		struct mtk_cam_graph_conn *conn;

		conn = v4l2_async_nf_add_fwnode_remote(&cam->notifier, endpoint,
						      struct mtk_cam_graph_conn);
		if (IS_ERR(conn)) {
			/*
			 * -ENOTCONN: the endpoint's remote is not in the DT.
			 * Not fatal, and not even noteworthy at probe time --
			 * the file header explains why.
			 */
			if (PTR_ERR(conn) != -ENOTCONN)
				return PTR_ERR(conn);

			dev_dbg(cam->dev,
				"endpoint %pfw has no remote; not waiting on it\n",
				endpoint);
			continue;
		}

		cam->num_eps++;
	}

	return ret;
}

static int mtk_cam_graph_probe(struct platform_device *pdev)
{
	struct mtk_cam_graph *cam;
	int ret;

	cam = devm_kzalloc(&pdev->dev, sizeof(*cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	cam->dev = &pdev->dev;

	/*
	 * The media device.  media_device_init() sets up the graph_mutex and the
	 * entity/pad/link lists that media_gobj_create() files objects into, and
	 * it is not optional: media_create_pad_link() ends in media_gobj_create(),
	 * which does BUG_ON(!mdev).  So link creation cannot happen at all
	 * without this, which is the whole reason this driver exists.
	 */
	strscpy(cam->mdev.model, MTK_CAM_GRAPH_DRV_NAME,
		sizeof(cam->mdev.model));
	cam->mdev.dev = cam->dev;
	media_device_init(&cam->mdev);

	/*
	 * The v4l2 device.  The notifier will not bind anything without one:
	 * v4l2_async_nf_try_all_subdevs() returns early if
	 * v4l2_async_nf_find_v4l2_dev() is NULL, so a notifier with no v4l2_dev
	 * would silently match nothing and the graph would stay empty.
	 */
	cam->v4l2_dev.mdev = &cam->mdev;
	strscpy(cam->v4l2_dev.name, MTK_CAM_GRAPH_DRV_NAME,
		sizeof(cam->v4l2_dev.name));

	ret = v4l2_device_register(cam->dev, &cam->v4l2_dev);
	if (ret)
		goto err_media_cleanup;

	ret = media_device_register(&cam->mdev);
	if (ret) {
		dev_err(cam->dev, "failed to register media device: %d\n", ret);
		goto err_v4l2_unregister;
	}

	/* The notifier, then the endpoints it should wait on. */
	v4l2_async_nf_init(&cam->notifier, &cam->v4l2_dev);
	cam->notifier.ops = &mtk_cam_graph_notifier_ops;

	ret = mtk_cam_graph_add_endpoints(cam);
	if (ret) {
		dev_err(cam->dev, "failed to add endpoints: %d\n", ret);
		goto err_nf_cleanup;
	}

	if (!cam->num_eps)
		dev_warn(cam->dev,
			 "no endpoints resolved; the camera graph will be empty\n");

	/*
	 * Registering the notifier is what binds the subdevs that are already
	 * present, so it must come after both the v4l2_device and the endpoint
	 * list are in place.
	 */
	ret = v4l2_async_nf_register(&cam->notifier);
	if (ret) {
		dev_err(cam->dev, "failed to register notifier: %d\n", ret);
		goto err_nf_cleanup;
	}

	platform_set_drvdata(pdev, cam);

	dev_info(cam->dev,
		 "camera graph owner registered, waiting on %u endpoint(s)\n",
		 cam->num_eps);

	return 0;

err_nf_cleanup:
	v4l2_async_nf_cleanup(&cam->notifier);
err_v4l2_unregister:
	v4l2_device_unregister(&cam->v4l2_dev);
err_media_cleanup:
	media_device_cleanup(&cam->mdev);

	return ret;
}

static void mtk_cam_graph_remove(struct platform_device *pdev)
{
	struct mtk_cam_graph *cam = platform_get_drvdata(pdev);

	/*
	 * Unbind first: v4l2_async_nf_unregister() detaches every subdev this
	 * notifier holds, which unregisters their entities from the media_device
	 * and drops the links between them.  Only then is it safe to take the
	 * media_device and v4l2_device away.
	 *
	 * Order matters.  Unregistering the media_device while the subdevs are
	 * still bound would leave entities pointing at a freed mdev.
	 */
	v4l2_async_nf_unregister(&cam->notifier);
	v4l2_async_nf_cleanup(&cam->notifier);

	media_device_unregister(&cam->mdev);
	v4l2_device_unregister(&cam->v4l2_dev);
	media_device_cleanup(&cam->mdev);
}

static const struct of_device_id mtk_cam_graph_of_match[] = {
	{ .compatible = "mediatek,mt6589-cam-graph" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, mtk_cam_graph_of_match);

static struct platform_driver mtk_cam_graph_driver = {
	.probe = mtk_cam_graph_probe,
	.remove = mtk_cam_graph_remove,
	.driver = {
		.name = "mtk-cam-graph",
		.of_match_table = mtk_cam_graph_of_match,
	},
};
module_platform_driver(mtk_cam_graph_driver);

MODULE_DESCRIPTION("MediaTek MT6589 camera media-graph owner");
MODULE_AUTHOR("Lenovo Linux Team");
MODULE_LICENSE("GPL");