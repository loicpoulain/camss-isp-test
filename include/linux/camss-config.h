/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Qualcomm CAMSS ISP parameters UAPI
 *
 * Uses the generic V4L2 extensible ISP parameters buffer format defined in
 * <uapi/linux/media/v4l2-isp.h>.
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef _UAPI_LINUX_CAMSS_CONFIG_H
#define _UAPI_LINUX_CAMSS_CONFIG_H

#include <linux/types.h>
#include <linux/media/v4l2-isp.h>

/*
 * V4L2 meta-output format for CAMSS ISP parameter buffers.
 * TODO: move to videodev2.h once stabilised.
 */
#define V4L2_META_FMT_CAMSS_PARAMS	v4l2_fourcc('Q', 'C', 'A', 'P')

/**
 * enum camss_params_block_type - CAMSS ISP parameter block identifiers
 *
 * Each value identifies one ISP processing block.  The value is placed in
 * the @type field of &struct v4l2_isp_params_block_header.
 */
enum camss_params_block_type {
	CAMSS_PARAMS_WB_GAIN = 1,
	CAMSS_PARAMS_DEMO    = 2,
	CAMSS_PARAMS_MAX,
};


/**
 * struct camss_params_wb_gain - White Balance gains
 *
 * @header:   generic block header; @header.type = CAMSS_PARAMS_WB_GAIN
 * @g_gain:   green channel gain (Q5.10)
 * @b_gain:   blue channel gain (Q5.10)
 * @r_gain:   red channel gain (Q5.10)
 * @g_offset: green channel offset
 * @b_offset: blue channel offset
 * @r_offset: red channel offset
 */
struct camss_params_wb_gain {
	struct v4l2_isp_params_block_header header;
	__u16 g_gain;
	__u16 b_gain;
	__u16 r_gain;
	__u16 _pad;
	__s32 g_offset;
	__s32 b_offset;
	__s32 r_offset;
} __attribute__((aligned(8)));

/**
 * struct camss_params_demo - Demosaic coefficients
 *
 * @header:    generic block header; @header.type = CAMSS_PARAMS_DEMO
 * @lambda_rb: blue/red interpolation coefficient (Q8)
 * @lambda_g:  green interpolation coefficient (Q8)
 * @a_k:       edge detection noise offset (Q0)
 * @w_k:       edge detection weight (Q10)
 */
struct camss_params_demo {
	struct v4l2_isp_params_block_header header;
	__u8  lambda_rb;
	__u8  lambda_g;
	__u16 a_k;
	__u16 w_k;
	__u16 _pad;
} __attribute__((aligned(8)));

#define CAMSS_PARAMS_MAX_PAYLOAD		\
	(sizeof(struct camss_params_wb_gain)	+\
	 sizeof(struct camss_params_demo))

#endif /* _UAPI_LINUX_CAMSS_CONFIG_H */
