// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-test: ISP parameter buffer helpers
 */
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

/* Use our local copies of the uapi headers */
#include <linux/media/v4l2-isp.h>
#include <linux/camss-config.h>

#include "params.h"

void params_config_default(struct params_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->wb_g_gain      = PARAMS_WB_G_GAIN_DEFAULT;
	cfg->wb_b_gain      = PARAMS_WB_B_GAIN_DEFAULT;
	cfg->wb_r_gain      = PARAMS_WB_R_GAIN_DEFAULT;
	cfg->include_wb     = 1;
	cfg->wb_enabled     = 1;

	cfg->ce_luma_v0  = PARAMS_CE_LUMA_V0_DEFAULT;
	cfg->ce_luma_v1  = PARAMS_CE_LUMA_V1_DEFAULT;
	cfg->ce_luma_v2  = PARAMS_CE_LUMA_V2_DEFAULT;
	cfg->ce_luma_k   = PARAMS_CE_LUMA_K_DEFAULT;
	cfg->ce_coeff_ap = PARAMS_CE_COEFF_AP_DEFAULT;
	cfg->ce_coeff_am = PARAMS_CE_COEFF_AM_DEFAULT;
	cfg->ce_coeff_cp = PARAMS_CE_COEFF_CP_DEFAULT;
	cfg->ce_coeff_cm = PARAMS_CE_COEFF_CM_DEFAULT;
	cfg->ce_coeff_dp = PARAMS_CE_COEFF_DP_DEFAULT;
	cfg->ce_coeff_dm = PARAMS_CE_COEFF_DM_DEFAULT;
	cfg->ce_kcb      = PARAMS_CE_KCB_DEFAULT;
	cfg->ce_kcr      = PARAMS_CE_KCR_DEFAULT;
	cfg->include_ce  = 1;
	cfg->ce_enabled  = 1;

	/* Color correction: identity matrix */
	int16_t cc_a[] = PARAMS_CC_A_DEFAULT;
	int16_t cc_b[] = PARAMS_CC_B_DEFAULT;
	int16_t cc_c[] = PARAMS_CC_C_DEFAULT;
	int16_t cc_k[] = PARAMS_CC_K_DEFAULT;
	memcpy(cfg->cc_a, cc_a, sizeof(cfg->cc_a));
	memcpy(cfg->cc_b, cc_b, sizeof(cfg->cc_b));
	memcpy(cfg->cc_c, cc_c, sizeof(cfg->cc_c));
	memcpy(cfg->cc_k, cc_k, sizeof(cfg->cc_k));
	cfg->cc_m       = PARAMS_CC_M_DEFAULT;
	cfg->include_cc = 1;
	cfg->cc_enabled = 1;
}

void params_config_randomize(struct params_config *cfg)
{
	int i;

	/* Randomize WB gains in [0.5, 2.0] Q5.10 range */
	cfg->wb_g_gain = (uint16_t)(512 + rand() % 1537);
	cfg->wb_b_gain = (uint16_t)(512 + rand() % 1537);
	cfg->wb_r_gain = (uint16_t)(512 + rand() % 1537);

	/* Randomize CC matrix: diagonal ±0.5 around identity (Q3.8) */
	for (i = 0; i < 3; i++) {
		cfg->cc_a[i] = (int16_t)((i == 0 ? 0x100 : 0) + (rand() % 129) - 64);
		cfg->cc_b[i] = (int16_t)((i == 1 ? 0x100 : 0) + (rand() % 129) - 64);
		cfg->cc_c[i] = (int16_t)((i == 2 ? 0x100 : 0) + (rand() % 129) - 64);
		cfg->cc_k[i] = (int16_t)((rand() % 33) - 16);
	}

}

size_t params_buffer_size(void)
{
	return offsetof(struct v4l2_isp_params_buffer, data) +
	       CAMSS_PARAMS_MAX_PAYLOAD;
}

ssize_t params_build(void *buf, size_t bufsize,
		     const struct params_config *cfg)
{
	struct v4l2_isp_params_buffer *pbuf = buf;
	uint8_t *data = pbuf->data;
	uint32_t data_size = 0;

	if (bufsize < params_buffer_size()) {
		fprintf(stderr, "params_build: buffer too small (%zu < %zu)\n",
			bufsize, params_buffer_size());
		return -1;
	}

	memset(buf, 0, bufsize);
	pbuf->version = 1; /* V4L2_ISP_PARAMS_VERSION_V1 */

	/* WB_GAIN block */
	if (cfg->include_wb) {
		struct camss_params_wb_gain *wb =
			(struct camss_params_wb_gain *)(data + data_size);
		wb->header.type  = CAMSS_PARAMS_WB_GAIN;
		wb->header.size  = sizeof(*wb);
		wb->header.flags = cfg->wb_enabled
			? V4L2_ISP_PARAMS_FL_BLOCK_ENABLE
			: V4L2_ISP_PARAMS_FL_BLOCK_DISABLE;
		wb->g_gain       = cfg->wb_g_gain;
		wb->b_gain       = cfg->wb_b_gain;
		wb->r_gain       = cfg->wb_r_gain;
		data_size += sizeof(*wb);
	}

	/* COLOR_CORRECT block */
	if (cfg->include_ce) {
		struct camss_params_chroma_enhan *cc =
			(struct camss_params_chroma_enhan *)(data + data_size);
		cc->header.type  = CAMSS_PARAMS_CHROMA_ENHAN;
		cc->header.size  = sizeof(*cc);
		cc->header.flags = cfg->ce_enabled
			? V4L2_ISP_PARAMS_FL_BLOCK_ENABLE
			: V4L2_ISP_PARAMS_FL_BLOCK_DISABLE;
		cc->luma_v0  = cfg->ce_luma_v0;
		cc->luma_v1  = cfg->ce_luma_v1;
		cc->luma_v2  = cfg->ce_luma_v2;
		cc->luma_k   = cfg->ce_luma_k;
		cc->coeff_ap = cfg->ce_coeff_ap;
		cc->coeff_am = cfg->ce_coeff_am;
		cc->coeff_cp = cfg->ce_coeff_cp;
		cc->coeff_cm = cfg->ce_coeff_cm;
		cc->coeff_dp = cfg->ce_coeff_dp;
		cc->coeff_dm = cfg->ce_coeff_dm;
		cc->kcb      = cfg->ce_kcb;
		cc->kcr      = cfg->ce_kcr;
		data_size += sizeof(*cc);
	}

	/* COLOR_CORRECT block */
	if (cfg->include_cc) {
		struct camss_params_color_correct *cc =
			(struct camss_params_color_correct *)(data + data_size);
		cc->header.type  = CAMSS_PARAMS_COLOR_CORRECT;
		cc->header.size  = sizeof(*cc);
		cc->header.flags = cfg->cc_enabled
			? V4L2_ISP_PARAMS_FL_BLOCK_ENABLE
			: V4L2_ISP_PARAMS_FL_BLOCK_DISABLE;
		memcpy(cc->a, cfg->cc_a, sizeof(cc->a));
		memcpy(cc->b, cfg->cc_b, sizeof(cc->b));
		memcpy(cc->c, cfg->cc_c, sizeof(cc->c));
		memcpy(cc->k, cfg->cc_k, sizeof(cc->k));
		cc->m = cfg->cc_m;
		data_size += sizeof(*cc);
	}

	pbuf->data_size = data_size;

	return (ssize_t)(offsetof(struct v4l2_isp_params_buffer, data) +
			 data_size);
}

int params_open(const char *devnode, unsigned int count,
		const struct params_config *cfgs,
		struct params_ctx *ctx)
{
	struct v4l2_format fmt = {};
	struct v4l2_requestbuffers req = {};
	size_t bufsz = params_buffer_size();
	int ret;

	if (count == 0 || count > PARAMS_MAX_BUFS) {
		fprintf(stderr, "params: invalid count %u\n", count);
		return -1;
	}

	memset(ctx, 0, sizeof(*ctx));
	ctx->fd = -1;

	ctx->fd = open(devnode, O_RDWR | O_CLOEXEC);
	if (ctx->fd < 0) {
		fprintf(stderr, "params: open %s: %s\n", devnode, strerror(errno));
		return -1;
	}

	fmt.type = V4L2_BUF_TYPE_META_OUTPUT;
	fmt.fmt.meta.dataformat = V4L2_META_FMT_QCOM_ISP_PARAMS;
	fmt.fmt.meta.buffersize = (uint32_t)bufsz;
	ret = ioctl(ctx->fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_S_FMT: %s\n", strerror(errno));
		goto err_close;
	}
	bufsz = fmt.fmt.meta.buffersize;

	req.type   = V4L2_BUF_TYPE_META_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	req.count  = count;
	ret = ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_REQBUFS: %s\n", strerror(errno));
		goto err_close;
	}
	ctx->count = req.count;


	for (unsigned int i = 0; i < ctx->count; i++) {
		struct v4l2_buffer vbuf = {
			.type   = V4L2_BUF_TYPE_META_OUTPUT,
			.memory = V4L2_MEMORY_MMAP,
			.index  = i,
		};
		ret = ioctl(ctx->fd, VIDIOC_QUERYBUF, &vbuf);
		if (ret < 0) {
			fprintf(stderr, "params: VIDIOC_QUERYBUF %u: %s\n", i, strerror(errno));
			goto err_unmap;
		}

		ctx->bufs[i] = mmap(NULL, vbuf.length, PROT_READ | PROT_WRITE,
				    MAP_SHARED, ctx->fd, vbuf.m.offset);
		if (ctx->bufs[i] == MAP_FAILED) {
			ctx->bufs[i] = NULL;
			fprintf(stderr, "params: mmap %u: %s\n", i, strerror(errno));
			goto err_unmap;
		}
		ctx->lengths[i] = vbuf.length;

		printf("  %-14s  buf[%u]  offset=0x%08x  length=%-8u  VA=%p\n",
		       devnode, i, vbuf.m.offset, vbuf.length, ctx->bufs[i]);

		const struct params_config *cfg = cfgs ? &cfgs[i] : NULL;
		struct params_config def;
		if (!cfg) {
			params_config_default(&def);
			cfg = &def;
		}

		ssize_t written = params_build(ctx->bufs[i], vbuf.length, cfg);
		if (written < 0)
			goto err_unmap;

		vbuf.bytesused = (uint32_t)written;
		ret = ioctl(ctx->fd, VIDIOC_QBUF, &vbuf);
		if (ret < 0) {
			fprintf(stderr, "params: VIDIOC_QBUF %u: %s\n", i, strerror(errno));
			goto err_unmap;
		}
	}

	int type = V4L2_BUF_TYPE_META_OUTPUT;
	ret = ioctl(ctx->fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_STREAMON: %s\n", strerror(errno));
		goto err_unmap;
	}

	return 0;

err_unmap:
	for (unsigned int i = 0; i < ctx->count; i++)
		if (ctx->bufs[i])
			munmap(ctx->bufs[i], ctx->lengths[i]);
err_close:
	close(ctx->fd);
	ctx->fd = -1;
	return -1;
}

int params_cycle(struct params_ctx *ctx, const struct params_config *cfg)
{
	struct v4l2_buffer vbuf = {
		.type   = V4L2_BUF_TYPE_META_OUTPUT,
		.memory = V4L2_MEMORY_MMAP,
	};
	int ret;

	ret = ioctl(ctx->fd, VIDIOC_DQBUF, &vbuf);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_DQBUF: %s\n", strerror(errno));
		return -1;
	}

	if (cfg) {
		ssize_t written = params_build(ctx->bufs[vbuf.index],
					       ctx->lengths[vbuf.index], cfg);
		if (written < 0)
			return -1;
		vbuf.bytesused = (uint32_t)written;
	}

	ret = ioctl(ctx->fd, VIDIOC_QBUF, &vbuf);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_QBUF: %s\n", strerror(errno));
		return -1;
	}

	return (int)vbuf.index;
}

void params_close(struct params_ctx *ctx)
{
	if (ctx->fd < 0)
		return;

	int type = V4L2_BUF_TYPE_META_OUTPUT;
	ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

	for (unsigned int i = 0; i < ctx->count; i++)
		if (ctx->bufs[i])
			munmap(ctx->bufs[i], ctx->lengths[i]);

	close(ctx->fd);
	ctx->fd = -1;
}
