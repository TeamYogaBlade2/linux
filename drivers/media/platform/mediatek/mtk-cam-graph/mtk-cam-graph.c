// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6589 camera media-graph owner
 *
 * Copyright (c) 2026 Lenovo Linux Team
 *
 * The three camera blocks (mtk-csi2-rx, mtk-scam, mtk-cam) each register a
 * subdev and the dtsi wires them with "remote-endpoint" properties, but that
 * alone does not produce a media graph: something has to own the
 * media_device, the v4l2_device and the async notifier that actually pulls
 * each subdev in and registers its entity.
 *
 * This driver owns those three objects for the camera chain.  It is not a
 * subdev and has no video node of its own; it waits for the chain's endpoints
 * to bind, then creates the links the DT describes.
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
	 * D-PHY node in the tree -- so the receiver binds with a connection (to
	 * seninf_out_ep) yet has an endpoint whose remote does not resolve.  That
	 * is the documented state of the DT, not a failure: report it and move
	 * on, because the links are derived from the DT in .complete() and a
	 * missing peer simply yields no link.
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
 * CAM is the sink side of the scam_out -> cam_in pair and needs nothing extra
 * here: scam has a SOURCE pad (scam_out) and CAM has a SINK pad (cam_in), so
 * the pair (src = scam, sink_sd = cam) is visited by this loop, CAM's pad 0
 * passes the MEDIA_PAD_FL_SINK test above, and
 * v4l2_create_fwnode_links_to_pad(scam, cam_in_pad) resolves scam_out_ep's
 * remote to cam_in_ep and calls media_create_pad_link().  CAM having no
 * pad_ops does not interfere: media_entity_get_fwnode_pad() falls back to
 * "first pad with this direction", and pad 0 is CAM's only sink.  The one
 * thing CAM did need was to be ON this list, which is exactly what the graph
 * node's own cam_in_ep endpoint buys; see the file header.
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
	 *
	 * The inner loop is v4l2_create_fwnode_links(), which is precisely
	 * "for every sink pad of sink_sd, create the links the DT describes"
	 * - including the MEDIA_PAD_FL_SINK test and the already-exists skip.
	 * Doing it by hand here meant a hand-written scan of every pad plus a
	 * call per pad, which is the same work twice.
	 */
	list_for_each_entry(src, &cam->v4l2_dev.subdevs, list) {
		list_for_each_entry(sink_sd, &cam->v4l2_dev.subdevs, list) {
			if (src == sink_sd)
				continue;

			ret = v4l2_create_fwnode_links(src, sink_sd);
			if (ret)
				return ret;
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
 * These are OUR OWN endpoints, and this scan is the ONLY thing that puts a
 * match descriptor into notifier->waiting_list.  So a block that has no
 * endpoint here is a block that never binds, regardless of what its own DT
 * says its remote is: naming cam_in_ep as scam_out_ep's remote does not pull
 * CAM in, because v4l2_async_find_match() never looks at scam_out_ep.  The
 * dtsi therefore declares one endpoint per block here, CAM included.
 *
 * Returning -ENOTCONN from v4l2_async_nf_add_fwnode_remote() for an endpoint
 * whose remote does not resolve is expected here and must not fail probe,
 * and the DT currently declares no such endpoint -- all three remotes
 * resolve.  The path is kept because it is what lets a future
 * graph_to_sensor endpoint be added while the sensor node is still absent,
 * and because an unresolvable remote means "not present", not "broken".
 * Count what resolved and carry on.
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