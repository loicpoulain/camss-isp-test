// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-test: GStreamer appsrc sink
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <linux/videodev2.h>

#include "gst_sink.h"

struct gst_sink {
	GstElement  *pipeline;
	GstAppSrc   *appsrc;
	unsigned int fps;
	uint64_t     last_pts_ns;
};

static const char *fourcc_to_gst(uint32_t fourcc)
{
	switch (fourcc) {
	case V4L2_PIX_FMT_NV12: return "NV12";
	case V4L2_PIX_FMT_NV21: return "NV21";
	case V4L2_PIX_FMT_NV16: return "NV16";
	case V4L2_PIX_FMT_NV61: return "NV61";
	case V4L2_PIX_FMT_NV24: return "NV24";
	case V4L2_PIX_FMT_GREY: return "GRAY8";
	default:                 return NULL;
	}
}

struct gst_sink *gst_sink_open(unsigned int width, unsigned int height,
			       uint32_t v4l2_fourcc, unsigned int fps,
			       const char *pipeline_str)
{
	const char *fmt = fourcc_to_gst(v4l2_fourcc);

	if (!fmt) {
		fprintf(stderr, "gst_sink: unsupported fourcc %.4s\n",
			(char *)&v4l2_fourcc);
		return NULL;
	}

	struct gst_sink *sink = calloc(1, sizeof(*sink));
	if (!sink)
		return NULL;

	sink->fps = fps;

	char caps[256];
	if (fps)
		snprintf(caps, sizeof(caps),
			 "video/x-raw,format=%s,width=%u,height=%u,framerate=%u/1",
			 fmt, width, height, fps);
	else
		snprintf(caps, sizeof(caps),
			 "video/x-raw,format=%s,width=%u,height=%u,framerate=0/1",
			 fmt, width, height);

	char *full_pipeline;
	if (asprintf(&full_pipeline,
		     "appsrc name=src caps=\"%s\" format=time is-live=true do-timestamp=true ! %s",
		     caps, pipeline_str) < 0) {
		free(sink);
		return NULL;
	}

	printf("  GStreamer pipeline: %s\n", full_pipeline);

	GError *err = NULL;
	sink->pipeline = gst_parse_launch(full_pipeline, &err);
	free(full_pipeline);

	if (!sink->pipeline) {
		fprintf(stderr, "gst_sink: failed to create pipeline: %s\n",
			err ? err->message : "unknown");
		if (err) g_error_free(err);
		free(sink);
		return NULL;
	}

	GstElement *appsrc_elem = gst_bin_get_by_name(GST_BIN(sink->pipeline), "src");
	if (!appsrc_elem) {
		fprintf(stderr, "gst_sink: could not find appsrc element\n");
		gst_object_unref(sink->pipeline);
		free(sink);
		return NULL;
	}
	sink->appsrc = GST_APP_SRC(appsrc_elem);

	g_object_set(G_OBJECT(sink->appsrc), "do-timestamp", TRUE, NULL);

	if (gst_element_set_state(sink->pipeline, GST_STATE_PLAYING) ==
	    GST_STATE_CHANGE_FAILURE) {
		fprintf(stderr, "gst_sink: failed to start pipeline\n");
		gst_object_unref(sink->appsrc);
		gst_object_unref(sink->pipeline);
		free(sink);
		return NULL;
	}

	return sink;
}

int gst_sink_push(struct gst_sink *sink, const void *data, size_t size,
		  uint64_t pts_ns)
{
	GstBuffer *buf = gst_buffer_new_allocate(NULL, size, NULL);
	if (!buf)
		return -1;

	gst_buffer_fill(buf, 0, data, size);

	GstClockTime duration;
	if (sink->fps > 0) {
		duration = GST_SECOND / sink->fps;
	} else if (sink->last_pts_ns > 0) {
		duration = pts_ns - sink->last_pts_ns;
	} else {
		duration = GST_CLOCK_TIME_NONE;
	}
	sink->last_pts_ns = pts_ns;

	GST_BUFFER_DURATION(buf) = duration;

	GstFlowReturn ret = gst_app_src_push_buffer(sink->appsrc, buf);
	return (ret == GST_FLOW_OK) ? 0 : -1;
}

void gst_sink_close(struct gst_sink *sink)
{
	if (!sink)
		return;

	gst_app_src_end_of_stream(sink->appsrc);

	GstBus *bus = gst_element_get_bus(sink->pipeline);
	GstMessage *msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
						     GST_MESSAGE_EOS |
						     GST_MESSAGE_ERROR);
	if (msg)
		gst_message_unref(msg);
	gst_object_unref(bus);

	gst_element_set_state(sink->pipeline, GST_STATE_NULL);
	gst_object_unref(sink->appsrc);
	gst_object_unref(sink->pipeline);
	free(sink);
}
