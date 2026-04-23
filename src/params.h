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
#define PARAMS_CE_LUMA_V0_DEFAULT  0x04d  /* R->Y  0.299 BT.601 12sQ8 */
#define PARAMS_CE_LUMA_V1_DEFAULT  0x096  /* G->Y  0.587 BT.601 12sQ8 */
#define PARAMS_CE_LUMA_V2_DEFAULT  0x01d  /* B->Y  0.114 BT.601 12sQ8 */
#define PARAMS_CE_LUMA_K_DEFAULT   0
#define PARAMS_CE_COEFF_AP_DEFAULT 0x0e6  /* Cb  0.886 BT.601 12sQ8 */
#define PARAMS_CE_COEFF_AM_DEFAULT 0x0e6
#define PARAMS_CE_COEFF_CP_DEFAULT 0x0b3  /* Cr  0.701 BT.601 12sQ8 */
#define PARAMS_CE_COEFF_CM_DEFAULT 0x0b3
#define PARAMS_CE_COEFF_DP_DEFAULT 0xfb3  /* Cb -0.338 BT.601 12sQ8 */
#define PARAMS_CE_COEFF_DM_DEFAULT 0xfb3
#define PARAMS_CE_KCB_DEFAULT      128
#define PARAMS_CE_KCR_DEFAULT      128

/* Color correction matrix defaults (identity, Q3.8: 1.0 = 0x100) */
#define PARAMS_CC_A_DEFAULT  { 0x100, 0x000, 0x000 }
#define PARAMS_CC_B_DEFAULT  { 0x000, 0x100, 0x000 }
#define PARAMS_CC_C_DEFAULT  { 0x000, 0x000, 0x100 }
#define PARAMS_CC_K_DEFAULT  { 0, 0, 0 }
#define PARAMS_CC_M_DEFAULT  0


/**
 * struct params_config - tuning values for one params buffer
 */
struct params_config {
	/* White balance */
	uint16_t wb_g_gain;
	uint16_t wb_b_gain;
	uint16_t wb_r_gain;

	/* Black level subtraction */

	/* Which blocks to include (all enabled by default) */
	int include_wb;

	/* Color correction matrix (BT.601 defaults) */
	int16_t ce_luma_v0;
	int16_t ce_luma_v1;
	int16_t ce_luma_v2;
	int16_t ce_luma_k;
	int16_t ce_coeff_ap;
	int16_t ce_coeff_am;
	int16_t ce_coeff_cp;
	int16_t ce_coeff_cm;
	int16_t ce_coeff_dp;
	int16_t ce_coeff_dm;
	int16_t ce_kcb;
	int16_t ce_kcr;
	int include_ce;

	/* Color correction matrix */
	int16_t cc_a[3];
	int16_t cc_b[3];
	int16_t cc_c[3];
	int16_t cc_k[3];
	int16_t cc_m;
	int include_cc;
};

/**
 * params_config_default - fill @cfg with driver default values
 */
void params_config_default(struct params_config *cfg);

/**
 * params_config_randomize - randomize tuning values in @cfg
 *
 * Randomizes WB gains and CC matrix coefficients within plausible
 * ranges. Demosaic and chroma enhancement are left at defaults.
 */
void params_config_randomize(struct params_config *cfg);

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
 * struct params_ctx - multi-buffer params context
 *
 * @fd:      open file descriptor for the params vnode
 * @bufs:    mmap'd pointers, one per buffer slot
 * @lengths: length of each mapped buffer
 * @count:   number of buffer slots
 */
#define PARAMS_MAX_BUFS 4
struct params_ctx {
	int    fd;
	void  *bufs[PARAMS_MAX_BUFS];
	size_t lengths[PARAMS_MAX_BUFS];
	unsigned int count;
};

/**
 * params_open - open params vnode, allocate @count buffers, fill and queue all
 *
 * Each buffer slot i is filled with cfgs[i] (or cfgs[0] if cfgs is NULL,
 * using default values). All buffers are queued before STREAMON.
 *
 * Returns 0 on success, -1 on error.
 */
int params_open(const char *devnode, unsigned int count,
		const struct params_config *cfgs,
		struct params_ctx *ctx);

/**
 * params_cycle - dequeue the next done params buffer, optionally refill, requeue
 *
 * If @cfg is non-NULL the returned buffer is refilled before requeueing.
 * If @cfg is NULL the buffer is requeued as-is (content unchanged).
 *
 * Returns the dequeued buffer index (>= 0) on success, -1 on error.
 */
int params_cycle(struct params_ctx *ctx, const struct params_config *cfg);

/**
 * params_close - streamoff, unmap all buffers, close fd
 */
void params_close(struct params_ctx *ctx);


#endif /* CAMSS_TEST_PARAMS_H */
