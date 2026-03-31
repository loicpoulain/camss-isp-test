// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-test: ISP parameter buffer helpers
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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
	cfg->demo_lambda_g  = PARAMS_DEMO_LAMBDA_G_DEFAULT;
	cfg->demo_lambda_rb = PARAMS_DEMO_LAMBDA_RB_DEFAULT;
	cfg->demo_a_k       = PARAMS_DEMO_AK_DEFAULT;
	cfg->demo_w_k       = PARAMS_DEMO_WK_DEFAULT;
	cfg->include_wb     = 1;
	cfg->include_demo   = 1;
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
		wb->header.flags = 0;
		wb->g_gain       = cfg->wb_g_gain;
		wb->b_gain       = cfg->wb_b_gain;
		wb->r_gain       = cfg->wb_r_gain;
		wb->g_offset     = cfg->wb_g_offset;
		wb->b_offset     = cfg->wb_b_offset;
		wb->r_offset     = cfg->wb_r_offset;
		data_size += sizeof(*wb);
	}

	/* DEMO block */
	if (cfg->include_demo) {
		struct camss_params_demo *demo =
			(struct camss_params_demo *)(data + data_size);
		demo->header.type  = CAMSS_PARAMS_DEMO;
		demo->header.size  = sizeof(*demo);
		demo->header.flags = 0;
		demo->lambda_g     = cfg->demo_lambda_g;
		demo->lambda_rb    = cfg->demo_lambda_rb;
		demo->a_k          = cfg->demo_a_k;
		demo->w_k          = cfg->demo_w_k;
		data_size += sizeof(*demo);
	}

	pbuf->data_size = data_size;

	return (ssize_t)(offsetof(struct v4l2_isp_params_buffer, data) +
			 data_size);
}

int params_enqueue(const char *devnode, const struct params_config *cfg)
{
	int fd;
	struct v4l2_format fmt = {};
	struct v4l2_requestbuffers req = {};
	struct v4l2_buffer vbuf = {};
	void *mapped;
	size_t bufsz = params_buffer_size();
	ssize_t written;
	int ret;

	fd = open(devnode, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "params: open %s: %s\n", devnode, strerror(errno));
		return -1;
	}

	/* Set meta format */
	fmt.type = V4L2_BUF_TYPE_META_OUTPUT;
	fmt.fmt.meta.dataformat = V4L2_META_FMT_CAMSS_PARAMS;
	fmt.fmt.meta.buffersize = (uint32_t)bufsz;
	ret = ioctl(fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_S_FMT: %s\n", strerror(errno));
		goto err_close;
	}

	printf("params: format set buffersize=%u\n", fmt.fmt.meta.buffersize);
	bufsz = fmt.fmt.meta.buffersize;

	/* Request one buffer */
	req.type   = V4L2_BUF_TYPE_META_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	req.count  = 1;
	ret = ioctl(fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_REQBUFS: %s\n", strerror(errno));
		goto err_close;
	}

	/* Query and map */
	vbuf.type   = V4L2_BUF_TYPE_META_OUTPUT;
	vbuf.memory = V4L2_MEMORY_MMAP;
	vbuf.index  = 0;
	ret = ioctl(fd, VIDIOC_QUERYBUF, &vbuf);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_QUERYBUF: %s\n", strerror(errno));
		goto err_close;
	}

	mapped = mmap(NULL, vbuf.length, PROT_READ | PROT_WRITE,
		      MAP_SHARED, fd, vbuf.m.offset);
	if (mapped == MAP_FAILED) {
		fprintf(stderr, "params: mmap: %s\n", strerror(errno));
		goto err_close;
	}

	/* Build and write params */
	written = params_build(mapped, vbuf.length, cfg);
	if (written < 0) {
		munmap(mapped, vbuf.length);
		goto err_close;
	}

	printf("params: built %zd bytes (WB=%d DEMO=%d)\n",
	       written, cfg->include_wb, cfg->include_demo);

	munmap(mapped, vbuf.length);

	/* Queue the buffer */
	vbuf.bytesused = (uint32_t)written;
	ret = ioctl(fd, VIDIOC_QBUF, &vbuf);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_QBUF: %s\n", strerror(errno));
		goto err_close;
	}

	/* Stream on */
	int type = V4L2_BUF_TYPE_META_OUTPUT;
	ret = ioctl(fd, VIDIOC_STREAMON, &type);
	if (ret < 0) {
		fprintf(stderr, "params: VIDIOC_STREAMON: %s\n", strerror(errno));
		goto err_close;
	}

	printf("params: streaming on %s\n", devnode);
	return fd;

err_close:
	close(fd);
	return -1;
}
