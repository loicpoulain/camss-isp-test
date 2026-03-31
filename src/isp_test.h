/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-isp-test: frame streaming test
 */
#ifndef CAMSS_TEST_ISP_TEST_H
#define CAMSS_TEST_ISP_TEST_H

#include <stdint.h>
#include "media.h"

/**
 * struct frame_config - configuration for one streaming test
 * @input_fmt:    V4L2 fourcc for the input (Bayer) frame
 * @output_fmt:   V4L2 fourcc for the output (YUV) frame
 * @width:        frame width in pixels
 * @height:       frame height in pixels
 * @num_frames:   number of frames to process
 * @input_file:   path to raw input frame file (NULL = generate pattern)
 * @output_file:  path to write output frame (NULL = discard)
 * @with_params:     enqueue a params buffer before streaming
 * @pipeline_depth:  input buffers to keep queued ahead (1..MAX_PIPELINE_BUFS,
 *                   default 1). Higher values increase throughput at the cost
 *                   of timing accuracy and extra frames on streamoff.
 * @framerate:        nominal frame rate in fps (0 = do not set, driver default).
 *                   Passed to the input vnode via VIDIOC_S_PARM.
 */
struct frame_config {
	uint32_t    input_fmt;
	uint32_t    output_fmt;
	uint32_t    width;
	uint32_t    height;
	uint32_t    num_frames;
	const char *input_file;
	const char *output_file;
	int          with_params;
	unsigned int pipeline_depth;
	unsigned int framerate;
};

/**
 * isp_test_run - run a full input→output streaming test
 *
 * Opens the input and output vnodes, sets formats, allocates buffers,
 * streams @cfg->num_frames frames, and optionally saves the output.
 *
 * Returns 0 on success, -1 on failure.
 */
int isp_test_run(struct isp_pipeline *pipe, const struct frame_config *cfg);

/**
 * isp_test_enum_formats - enumerate and print supported formats on all vnodes
 */
void isp_test_enum_formats(struct isp_pipeline *pipe);

#endif /* CAMSS_TEST_ISP_TEST_H */
