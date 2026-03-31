/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-isp-test: ISP parameter buffer helpers
 */
#ifndef CAMSS_TEST_PARAMS_H
#define CAMSS_TEST_PARAMS_H

#include <stddef.h>
#include <stdint.h>

/* Default tuning values matching ope_default_config in the driver */
#define PARAMS_WB_G_GAIN_DEFAULT  ((1 << 10) / 1)   /* 1.0  Q5.10 */
#define PARAMS_WB_B_GAIN_DEFAULT  ((3 << 10) / 2)   /* 1.5  Q5.10 */
#define PARAMS_WB_R_GAIN_DEFAULT  ((3 << 10) / 2)   /* 1.5  Q5.10 */
#define PARAMS_DEMO_LAMBDA_G_DEFAULT  128
#define PARAMS_DEMO_LAMBDA_RB_DEFAULT 0
#define PARAMS_DEMO_AK_DEFAULT        128
#define PARAMS_DEMO_WK_DEFAULT        102

/**
 * struct params_config - tuning values for one params buffer
 */
struct params_config {
	/* White balance */
	uint16_t wb_g_gain;
	uint16_t wb_b_gain;
	uint16_t wb_r_gain;
	int32_t  wb_g_offset;
	int32_t  wb_b_offset;
	int32_t  wb_r_offset;

	/* Black level subtraction */

	/* Demosaic */
	uint8_t  demo_lambda_g;
	uint8_t  demo_lambda_rb;
	uint16_t demo_a_k;
	uint16_t demo_w_k;

	/* Which blocks to include (all enabled by default) */
	int include_wb;
	int include_demo;
};

/**
 * params_config_default - fill @cfg with driver default values
 */
void params_config_default(struct params_config *cfg);

/**
 * params_build - serialise @cfg into a v4l2_isp_params_buffer
 *
 * @buf:     destination buffer (must be >= params_buffer_size() bytes)
 * @bufsize: size of @buf
 *
 * Returns the number of bytes written, or -1 on error.
 */
ssize_t params_build(void *buf, size_t bufsize,
		     const struct params_config *cfg);

/**
 * params_buffer_size - minimum buffer size for a full params buffer
 */
size_t params_buffer_size(void);

/**
 * params_enqueue - open params vnode, allocate one buffer, fill and queue it
 *
 * @devnode:  /dev/videoN for the params-input endpoint
 * @cfg:      tuning values to encode
 *
 * Returns the open fd (caller must close + streamoff), or -1 on error.
 */
int params_enqueue(const char *devnode, const struct params_config *cfg);

#endif /* CAMSS_TEST_PARAMS_H */
