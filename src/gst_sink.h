/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-isp-m2m: GStreamer appsrc sink — async push thread
 */
#ifndef CAMSS_TEST_GST_SINK_H
#define CAMSS_TEST_GST_SINK_H

#include <stddef.h>
#include <stdint.h>

struct gst_sink;

/**
 * gst_sink_open - create and start a GStreamer pipeline fed by appsrc
 *
 * Frames are copied and pushed asynchronously from a dedicated thread,
 * so gst_sink_push returns immediately and the caller can requeue the
 * output buffer to OPE without waiting for GStreamer.
 */
struct gst_sink *gst_sink_open(unsigned int width, unsigned int height,
			       uint32_t v4l2_fourcc, unsigned int fps,
			       const char *pipeline_str);

/**
 * gst_sink_push - copy and enqueue a frame for asynchronous GStreamer push
 *
 * Copies @data synchronously, then returns immediately.
 * The caller may requeue the output buffer to OPE right after this call.
 */
int gst_sink_push(struct gst_sink *sink, const void *data, size_t size,
		  uint64_t pts_ns);

/**
 * gst_sink_close - flush pending frames, send EOS, drain, destroy
 */
void gst_sink_close(struct gst_sink *sink);

#endif /* CAMSS_TEST_GST_SINK_H */
