/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-isp-test: GStreamer appsrc sink
 */
#ifndef CAMSS_TEST_GST_SINK_H
#define CAMSS_TEST_GST_SINK_H

#include <stddef.h>
#include <stdint.h>

struct gst_sink;

struct gst_sink *gst_sink_open(unsigned int width, unsigned int height,
			       uint32_t v4l2_fourcc, unsigned int fps,
			       const char *pipeline_str);

int gst_sink_push(struct gst_sink *sink, const void *data, size_t size,
		  uint64_t pts_ns);

void gst_sink_close(struct gst_sink *sink);

#endif /* CAMSS_TEST_GST_SINK_H */
