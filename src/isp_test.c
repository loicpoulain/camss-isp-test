// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-test: frame streaming test
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "isp_test.h"

extern volatile sig_atomic_t g_interrupted;
#ifdef HAVE_GSTREAMER
#include "gst_sink.h"
#endif
#include "media.h"
#include "params.h"
#include "params_ctrl.h"

#define MAX_PIPELINE_BUFS 3

/* ---- Live capture device context ------------------------------------ */

struct capture_ctx {
	int          fd;
	const char  *label;
	uint32_t     type;       /* V4L2_BUF_TYPE_VIDEO_CAPTURE or _MPLANE */
	uint32_t     fourcc;
	uint32_t     width;
	uint32_t     height;
	uint32_t     bytesperline;
	uint32_t     sizeimage;
	unsigned int buf_count;
	int          dmabuf_fds[MAX_PIPELINE_BUFS]; /* exported dmabuf fds */
};

static void capture_ctx_close(struct capture_ctx *c)
{
	if (c->fd < 0)
		return;
	for (unsigned int i = 0; i < c->buf_count; i++)
		if (c->dmabuf_fds[i] >= 0)
			close(c->dmabuf_fds[i]);
	close(c->fd);
	c->fd = -1;
}

static int capture_open(const char *devnode, uint32_t fourcc,
			uint32_t width, uint32_t height,
			unsigned int depth, struct capture_ctx *c)
{
	struct v4l2_format fmt = {};
	struct v4l2_requestbuffers req = {};
	int ret;

	memset(c, 0, sizeof(*c));
	c->fd = -1;
	for (unsigned int i = 0; i < MAX_PIPELINE_BUFS; i++)
		c->dmabuf_fds[i] = -1;

	c->fd = open(devnode, O_RDWR | O_CLOEXEC);
	if (c->fd < 0) {
		fprintf(stderr, "capture: open %s: %s\n", devnode, strerror(errno));
		return -1;
	}
	c->label = devnode;

	/* Try multiplanar first, fall back to singleplanar */
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) {
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		if (ioctl(c->fd, VIDIOC_G_FMT, &fmt) < 0) {
			fprintf(stderr, "capture: VIDIOC_G_FMT: %s\n", strerror(errno));
			goto err;
		}
	}
	c->type = fmt.type;

	/* Override format/size if caller specified them */
	if (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		if (fourcc) fmt.fmt.pix_mp.pixelformat = fourcc;
		if (width)  fmt.fmt.pix_mp.width        = width;
		if (height) fmt.fmt.pix_mp.height       = height;
		fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
		ret = ioctl(c->fd, VIDIOC_S_FMT, &fmt);
		if (ret < 0) {
			fprintf(stderr, "capture: VIDIOC_S_FMT: %s\n", strerror(errno));
			goto err;
		}
		c->fourcc       = fmt.fmt.pix_mp.pixelformat;
		c->width        = fmt.fmt.pix_mp.width;
		c->height       = fmt.fmt.pix_mp.height;
		c->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		c->sizeimage    = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
	} else {
		if (fourcc) fmt.fmt.pix.pixelformat = fourcc;
		if (width)  fmt.fmt.pix.width       = width;
		if (height) fmt.fmt.pix.height      = height;
		fmt.fmt.pix.field = V4L2_FIELD_NONE;
		ret = ioctl(c->fd, VIDIOC_S_FMT, &fmt);
		if (ret < 0) {
			fprintf(stderr, "capture: VIDIOC_S_FMT: %s\n", strerror(errno));
			goto err;
		}
		c->fourcc       = fmt.fmt.pix.pixelformat;
		c->width        = fmt.fmt.pix.width;
		c->height       = fmt.fmt.pix.height;
		c->bytesperline = fmt.fmt.pix.bytesperline;
		c->sizeimage    = fmt.fmt.pix.sizeimage;
	}

	printf("  %-6s  %s  %ux%u %.4s  bpl=%u  size=%u\n",
	       "Capture", devnode, c->width, c->height,
	       (char *)&c->fourcc, c->bytesperline, c->sizeimage);

	/* Allocate MMAP buffers so we can export them as dmabuf */
	req.type   = c->type;
	req.memory = V4L2_MEMORY_MMAP;
	req.count  = depth;
	ret = ioctl(c->fd, VIDIOC_REQBUFS, &req);
	if (ret < 0) {
		fprintf(stderr, "capture: VIDIOC_REQBUFS: %s\n", strerror(errno));
		goto err;
	}
	c->buf_count = req.count;

	/* Export each buffer as a dmabuf fd */
	printf("\nBuffers:\n");
	for (unsigned int i = 0; i < c->buf_count; i++) {
		struct v4l2_exportbuffer expbuf = {
			.type  = c->type,
			.index = i,
			.plane = 0,
			.flags = O_CLOEXEC,
		};
		ret = ioctl(c->fd, VIDIOC_EXPBUF, &expbuf);
		if (ret < 0) {
			fprintf(stderr, "capture: VIDIOC_EXPBUF %u: %s\n",
				i, strerror(errno));
			goto err;
		}
		c->dmabuf_fds[i] = expbuf.fd;

		/* Query offset for display */
		struct v4l2_buffer qbuf = { .type = c->type, .memory = V4L2_MEMORY_MMAP, .index = i };
		struct v4l2_plane planes[1] = {};
		if (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			qbuf.m.planes = planes;
			qbuf.length   = 1;
		}
		ioctl(c->fd, VIDIOC_QUERYBUF, &qbuf);
		uint32_t offset = (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			? planes[0].m.mem_offset : qbuf.m.offset;
		uint32_t length = (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
			? planes[0].length : qbuf.length;
		printf("  %-14s  buf[%u]  offset=0x%08x  length=%-8u  dmabuf_fd=%d\n",
		       devnode, i, offset, length, expbuf.fd);
	}

	/* Queue all buffers and stream on */
	for (unsigned int i = 0; i < c->buf_count; i++) {
		struct v4l2_buffer qbuf = { .type = c->type, .memory = V4L2_MEMORY_MMAP, .index = i };
		struct v4l2_plane planes[1] = {};
		if (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
			qbuf.m.planes = planes;
			qbuf.length   = 1;
		}
		ret = ioctl(c->fd, VIDIOC_QBUF, &qbuf);
		if (ret < 0) {
			fprintf(stderr, "capture: VIDIOC_QBUF %u: %s\n", i, strerror(errno));
			goto err;
		}
	}

	ret = ioctl(c->fd, VIDIOC_STREAMON, &c->type);
	if (ret < 0) {
		fprintf(stderr, "capture: VIDIOC_STREAMON: %s\n", strerror(errno));
		goto err;
	}

	return 0;
err:
	capture_ctx_close(c);
	return -1;
}

/* Dequeue from capture device, return buffer index */
static int capture_dqbuf(struct capture_ctx *c)
{
	struct v4l2_buffer buf = { .type = c->type, .memory = V4L2_MEMORY_MMAP };
	struct v4l2_plane planes[1] = {};
	if (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		buf.m.planes = planes;
		buf.length   = 1;
	}
	if (ioctl(c->fd, VIDIOC_DQBUF, &buf) < 0)
		return -1;
	return (int)buf.index;
}

/* Return a capture buffer back to the capture device */
static int capture_qbuf(struct capture_ctx *c, unsigned int idx)
{
	struct v4l2_buffer buf = { .type = c->type, .memory = V4L2_MEMORY_MMAP, .index = idx };
	struct v4l2_plane planes[1] = {};
	if (c->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
		buf.m.planes = planes;
		buf.length   = 1;
	}
	return ioctl(c->fd, VIDIOC_QBUF, &buf);
}

struct vnode_ctx {
	int      fd;
	const char *label;
	uint32_t type;       /* V4L2_BUF_TYPE_* */
	uint32_t fmt;        /* fourcc */
	uint32_t width;
	uint32_t height;
	uint32_t bytesperline;
	uint32_t sizeimage;
	void    *bufs[MAX_PIPELINE_BUFS];
	uint32_t buf_lengths[MAX_PIPELINE_BUFS];
};

static void vnode_ctx_close(struct vnode_ctx *v)
{
	if (v->fd < 0)
		return;
	for (int i = 0; i < MAX_PIPELINE_BUFS; i++) {
		if (v->bufs[i] && v->bufs[i] != MAP_FAILED)
			munmap(v->bufs[i], v->buf_lengths[i]);
	}
	close(v->fd);
	v->fd = -1;
}

/* Queue one dmabuf fd to the OPE input queue (V4L2_MEMORY_DMABUF) */
static int ope_in_qbuf_dmabuf(struct vnode_ctx *v, unsigned int idx, int dmabuf_fd)
{
	struct v4l2_buffer buf = {
		.type   = v->type,
		.memory = V4L2_MEMORY_DMABUF,
		.index  = idx,
	};
	struct v4l2_plane planes[1] = {{ .m.fd = dmabuf_fd,
					 .length   = v->sizeimage,
					 .bytesused = v->sizeimage }};
	buf.m.planes = planes;
	buf.length   = 1;
	return ioctl(v->fd, VIDIOC_QBUF, &buf);
}

static int vnode_open_and_set_fmt(struct vnode_ctx *v, const char *devnode,
				  uint32_t type, uint32_t fourcc,
				  uint32_t width, uint32_t height)
{
	struct v4l2_format fmt = {};
	int ret;

	v->label = devnode;
	v->fd = open(devnode, O_RDWR | O_CLOEXEC);
	if (v->fd < 0) {
		fprintf(stderr, "open %s: %s\n", devnode, strerror(errno));
		return -1;
	}

	v->type   = type;
	v->fmt    = fourcc;
	v->width  = width;
	v->height = height;

	fmt.type = type;
	fmt.fmt.pix_mp.pixelformat = fourcc;
	fmt.fmt.pix_mp.width       = width;
	fmt.fmt.pix_mp.height      = height;
	fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;

	ret = ioctl(v->fd, VIDIOC_S_FMT, &fmt);
	if (ret < 0) {
		fprintf(stderr, "VIDIOC_S_FMT on %s: %s\n", devnode, strerror(errno));
		return -1;
	}

	v->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	v->sizeimage    = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	printf("  %-6s  %s  %ux%u %.4s  bpl=%u  size=%u\n",
	       V4L2_TYPE_IS_OUTPUT(type) ? "Input" : "Output",
	       devnode, fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	       (char *)&fmt.fmt.pix_mp.pixelformat,
	       v->bytesperline, v->sizeimage);

	return 0;
}

static int vnode_alloc_bufs(struct vnode_ctx *v, unsigned int count)
{
	struct v4l2_requestbuffers req = {
		.type   = v->type,
		.memory = V4L2_MEMORY_MMAP,
		.count  = count,
	};

	if (ioctl(v->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}

	for (uint32_t i = 0; i < req.count; i++) {
		struct v4l2_buffer buf = {
			.type   = v->type,
			.memory = V4L2_MEMORY_MMAP,
			.index  = i,
		};
		struct v4l2_plane planes[1] = {};
		buf.m.planes = planes;
		buf.length   = 1;

		if (ioctl(v->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("VIDIOC_QUERYBUF");
			return -1;
		}

		uint32_t offset = V4L2_TYPE_IS_MULTIPLANAR(v->type)
			? planes[0].m.mem_offset : buf.m.offset;
		uint32_t length = V4L2_TYPE_IS_MULTIPLANAR(v->type)
			? planes[0].length : buf.length;

		v->bufs[i] = mmap(NULL, length, PROT_READ | PROT_WRITE,
				  MAP_SHARED, v->fd, offset);
		if (v->bufs[i] == MAP_FAILED) {
			perror("mmap");
			return -1;
		}

		printf("  %-14s  buf[%u]  offset=0x%08x  length=%-8u  VA=%p\n",
		       v->label ? v->label : "?", i, offset, length, v->bufs[i]);

		v->buf_lengths[i] = length;
	}

	return (int)req.count;
}

static int vnode_qbuf(struct vnode_ctx *v, uint32_t index)
{
	struct v4l2_buffer buf = {
		.type   = v->type,
		.memory = V4L2_MEMORY_MMAP,
		.index  = index,
	};
	struct v4l2_plane planes[1] = {};

	if (V4L2_TYPE_IS_MULTIPLANAR(v->type)) {
		planes[0].bytesused = v->sizeimage;
		buf.m.planes = planes;
		buf.length   = 1;
	} else {
		buf.bytesused = v->sizeimage;
	}

	return ioctl(v->fd, VIDIOC_QBUF, &buf);
}

static int vnode_dqbuf(struct vnode_ctx *v, uint32_t *index, uint32_t *sequence)
{
	struct v4l2_buffer buf = {
		.type   = v->type,
		.memory = V4L2_MEMORY_MMAP,
	};
	struct v4l2_plane planes[1] = {};

	if (V4L2_TYPE_IS_MULTIPLANAR(v->type)) {
		buf.m.planes = planes;
		buf.length   = 1;
	}

	if (ioctl(v->fd, VIDIOC_DQBUF, &buf) < 0)
		return -1;

	*index = buf.index;
	if (sequence)
		*sequence = buf.sequence;
	return 0;
}

/* Timing helpers */
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void print_latency_row(const char *label, const uint64_t *ns, uint32_t count)
{
	uint64_t total = 0, min = UINT64_MAX, max = 0;

	for (uint32_t i = 0; i < count; i++) {
		total += ns[i];
		if (ns[i] < min) min = ns[i];
		if (ns[i] > max) max = ns[i];
	}

	double min_ms = (double)min   / 1e6;
	double max_ms = (double)max   / 1e6;
	double avg_ms = (double)total / 1e6 / count;

	printf("  %-22s min=%.3f ms  max=%.3f ms  avg=%.3f ms\n",
	       label, min_ms, max_ms, avg_ms);
	printf("  %-22s (%.1f fps)     (%.1f fps)     (%.1f fps)\n",
	       "", 1e3 / min_ms, 1e3 / max_ms, 1e3 / avg_ms);
}

static void print_timing_summary(const uint64_t *frame_ns,
				  const uint64_t *capture_ns,
				  const uint64_t *out_ns,
				  uint32_t count, uint64_t wall_ns)
{
	printf("\nResults:\n");
	printf("  Throughput   %u frames   %.1f fps\n",
	       count, 1e9 * count / (double)wall_ns);
	if (capture_ns)
		print_latency_row("Cap latency", capture_ns, count);
	print_latency_row("Proc latency", frame_ns, count);
	print_latency_row("Out latency", out_ns, count);
}

/* Fill input buffer with a simple Bayer gradient pattern */
static void fill_bayer_pattern(void *buf, uint32_t width, uint32_t height,
				uint32_t bytesperline, uint32_t frame_num,
				uint32_t fourcc)
{
	uint8_t *p = buf;

	switch (fourcc) {
	case V4L2_PIX_FMT_SRGGB10P:
	case V4L2_PIX_FMT_SBGGR10P:
	case V4L2_PIX_FMT_SGBRG10P:
	case V4L2_PIX_FMT_SGRBG10P:
		/* MIPI packed 10-bit: 4 pixels -> 5 bytes */
		for (uint32_t y = 0; y < height; y++) {
			uint8_t *row = p + y * bytesperline;
			for (uint32_t x = 0; x < width; x += 4) {
				uint16_t v0 = (uint16_t)((x + 0 + y + frame_num * 4) & 0x3ff);
				uint16_t v1 = (uint16_t)((x + 1 + y + frame_num * 4) & 0x3ff);
				uint16_t v2 = (uint16_t)((x + 2 + y + frame_num * 4) & 0x3ff);
				uint16_t v3 = (uint16_t)((x + 3 + y + frame_num * 4) & 0x3ff);
				row[0] = (uint8_t)(v0 >> 2);
				row[1] = (uint8_t)(v1 >> 2);
				row[2] = (uint8_t)(v2 >> 2);
				row[3] = (uint8_t)(v3 >> 2);
				row[4] = (uint8_t)(((v0 & 3)) |
						   ((v1 & 3) << 2) |
						   ((v2 & 3) << 4) |
						   ((v3 & 3) << 6));
				row += 5;
			}
		}
		break;
	default:
		/* 8-bit plain */
		for (uint32_t y = 0; y < height; y++) {
			for (uint32_t x = 0; x < width; x++) {
				p[y * bytesperline + x] =
					(uint8_t)((x + y + frame_num * 4) & 0xff);
			}
		}
		break;
	}
}

static int vnode_set_framerate(struct vnode_ctx *v, unsigned int fps)
{
	struct v4l2_streamparm sp = {};

	sp.type = v->type;
	sp.parm.output.capability   = V4L2_CAP_TIMEPERFRAME;
	sp.parm.output.timeperframe.numerator   = 1;
	sp.parm.output.timeperframe.denominator = fps;

	if (ioctl(v->fd, VIDIOC_S_PARM, &sp) < 0) {
		fprintf(stderr, "VIDIOC_S_PARM: %s\n", strerror(errno));
		return -1;
	}

	printf("  frame rate set to %u/%u fps\n",
	       sp.parm.output.timeperframe.denominator,
	       sp.parm.output.timeperframe.numerator);
	return 0;
}

int isp_test_run(struct isp_pipeline *pipe, const struct frame_config *cfg)
{
	struct vnode_ctx in_ctx  = { .fd = -1 };
	struct vnode_ctx out_ctx = { .fd = -1 };
	struct capture_ctx cap_ctx = { .fd = -1 };
	int cap_buf_for_ope_slot[MAX_PIPELINE_BUFS]; /* capture buf idx for each OPE input slot */
	for (int _i = 0; _i < MAX_PIPELINE_BUFS; _i++) cap_buf_for_ope_slot[_i] = -1;
	struct isp_vnode *in_vn, *out_vn, *params_vn;
	struct params_ctx params_ctx = { .fd = -1 };
	struct params_config *params_cfgs = NULL;
	struct params_ctrl params_ctrl = {};
	int ret = -1;
	int out_file_fd = -1;
#ifdef HAVE_GSTREAMER
	struct gst_sink *gst = NULL;
#endif
	uint64_t submit_ns[MAX_PIPELINE_BUFS] = {};
	uint64_t *frame_ns = NULL;
	uint64_t *capture_ns = NULL;
	uint64_t *out_ns = NULL;

	uint32_t out_w = cfg->output_width  ? cfg->output_width  : cfg->width;
	uint32_t out_h = cfg->output_height ? cfg->output_height : cfg->height;
	if (cfg->duration_ms)
		printf("\nTest: %ux%u %.4s -> %ux%u %.4s  [%.1f s]\n",
		       cfg->width, cfg->height, (char *)&cfg->input_fmt,
		       out_w, out_h, (char *)&cfg->output_fmt,
		       cfg->duration_ms / 1000.0);
	else
		printf("\nTest: %ux%u %.4s -> %ux%u %.4s  [%u frames]\n",
		       cfg->width, cfg->height, (char *)&cfg->input_fmt,
		       out_w, out_h, (char *)&cfg->output_fmt,
		       cfg->num_frames);

	/* Find vnodes */
	in_vn     = media_find_vnode(pipe, "input");
	out_vn    = media_find_vnode(pipe, "output");
	params_vn = media_find_vnode(pipe, "params");

	if (!in_vn || !out_vn) {
		fprintf(stderr, "Could not find input or output vnode\n");
		return -1;
	}

	if (!in_vn->devnode[0] || !out_vn->devnode[0]) {
		fprintf(stderr, "Vnode device paths not resolved\n");
		return -1;
	}

	/* Open and configure input */
	if (vnode_open_and_set_fmt(&in_ctx, in_vn->devnode,
				   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				   cfg->input_fmt, cfg->width, cfg->height) < 0)
		goto out;

	if (cfg->framerate && vnode_set_framerate(&in_ctx, cfg->framerate) < 0)
		goto out;

	/* Open and configure output */
	if (vnode_open_and_set_fmt(&out_ctx, out_vn->devnode,
				   V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				   cfg->output_fmt,
				   cfg->output_width  ? cfg->output_width  : cfg->width,
				   cfg->output_height ? cfg->output_height : cfg->height) < 0)
		goto out;

	/* Allocate buffers: depth clamped to [1, MAX_PIPELINE_BUFS] */
	unsigned int depth = cfg->pipeline_depth ? cfg->pipeline_depth : 1;

#ifdef HAVE_GSTREAMER
	if (cfg->gst_pipeline) {
		gst = gst_sink_open(out_w, out_h, cfg->output_fmt,
				    cfg->framerate, cfg->gst_pipeline);
		if (!gst)
			goto out;
	}
#endif

	/* Open live capture device if requested */
	if (cfg->input_device) {
		if (capture_open(cfg->input_device,
				 cfg->input_fmt, cfg->width, cfg->height,
				 depth, &cap_ctx) < 0)
			goto out;

		/* Re-issue S_FMT on OPE input with capture device bytesperline
		 * to ensure stride matches (capture driver may align differently) */
		if (cap_ctx.bytesperline && cap_ctx.bytesperline != in_ctx.bytesperline) {
			struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
			fmt.fmt.pix_mp.pixelformat          = in_ctx.fmt;
			fmt.fmt.pix_mp.width                = in_ctx.width;
			fmt.fmt.pix_mp.height               = in_ctx.height;
			fmt.fmt.pix_mp.field                = V4L2_FIELD_NONE;
			fmt.fmt.pix_mp.plane_fmt[0].bytesperline = cap_ctx.bytesperline;
			if (ioctl(in_ctx.fd, VIDIOC_S_FMT, &fmt) == 0) {
				in_ctx.bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
				in_ctx.sizeimage    = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
				printf("  %-6s  %s  %ux%u %.4s  bpl=%u  size=%u  (stride adjusted to match capture)\n",
				       "Input", in_vn->devnode,
				       in_ctx.width, in_ctx.height,
				       (char *)&in_ctx.fmt,
				       in_ctx.bytesperline, in_ctx.sizeimage);
			}
		}
	}
	if (depth > MAX_PIPELINE_BUFS) depth = MAX_PIPELINE_BUFS;
	if (!cfg->duration_ms && depth > cfg->num_frames) depth = cfg->num_frames;

	/* Prepare params config (opened after Buffers: header below) */
	unsigned int params_count = 0;
	if (cfg->with_params && params_vn && params_vn->devnode[0]) {
		params_count = cfg->randomize_params ? depth : 1;
		params_cfgs = calloc(params_count, sizeof(*params_cfgs));
		if (!params_cfgs) goto out;
		for (unsigned int i = 0; i < params_count; i++) {
			params_config_default(&params_cfgs[i]);
			if (cfg->randomize_params)
				params_config_randomize(&params_cfgs[i]);
		}
	} else if (cfg->with_params) {
		fprintf(stderr, "Warning: params vnode not found, continuing without\n");
	}

	if (!cfg->input_device)
		printf("\nBuffers:\n");
	int in_nbufs = 0;
	if (!cfg->input_device) {
		in_nbufs = vnode_alloc_bufs(&in_ctx, depth);
		if (in_nbufs < 0)
			goto out;
	} else {
		/* OPE input uses DMABUF — just REQBUFS with count=depth, no mmap */
		struct v4l2_requestbuffers req = {
			.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			.memory = V4L2_MEMORY_DMABUF,
			.count  = depth,
		};
		if (ioctl(in_ctx.fd, VIDIOC_REQBUFS, &req) < 0) {
			perror("VIDIOC_REQBUFS input dmabuf");
			goto out;
		}
		in_nbufs = (int)req.count;
	}
	int out_nbufs = vnode_alloc_bufs(&out_ctx, depth);
	if (out_nbufs < 0)
		goto out;

	/* Open params vnode here so its buffers appear in the Buffers: section */
	if (params_count > 0) {
		if (params_open(params_vn->devnode, params_count, params_cfgs,
				&params_ctx) < 0)
			fprintf(stderr, "Warning: params open failed, continuing without\n");
		else
			params_ctrl_start(&params_ctrl, &params_cfgs[0]);
	}

	uint32_t frame_ns_cap = cfg->duration_ms ? 4096 : cfg->num_frames;
	frame_ns = calloc(frame_ns_cap, sizeof(*frame_ns));
	if (!frame_ns)
		goto out;
	if (cfg->input_device) {
		capture_ns = calloc(frame_ns_cap, sizeof(*capture_ns));
		if (!capture_ns)
			goto out;
	}
	out_ns = calloc(frame_ns_cap, sizeof(*out_ns));
	if (!out_ns)
		goto out;

	/* Load or generate input frames */
	int input_fd = -1;
	if (cfg->input_file) {
		input_fd = open(cfg->input_file, O_RDONLY);
		if (input_fd < 0) {
			fprintf(stderr, "open input %s: %s\n",
				cfg->input_file, strerror(errno));
			goto out;
		}
	}

	/* Open output file */
	if (cfg->output_file) {
		out_file_fd = open(cfg->output_file,
				   O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out_file_fd < 0) {
			fprintf(stderr, "open output %s: %s\n",
				cfg->output_file, strerror(errno));
			goto out;
		}
	}

	/* Pre-queue input buffers.
	 * Capture path: submit one capture frame to OPE before STREAMON;
	 * remaining capture buffers stay queued to the capture device and
	 * are fed to OPE asynchronously in the hot loop.
	 * Non-capture path: fill and queue all depth buffers as before. */
	for (int i = 0; i < (int)depth; i++) {
		if (cfg->input_device) {
			if (i == 0) {
				/* Submit first capture frame to OPE synchronously */
				int cidx = capture_dqbuf(&cap_ctx);
				if (cidx < 0) {
					perror("capture: VIDIOC_DQBUF pre-queue");
					goto out;
				}
				if (ope_in_qbuf_dmabuf(&in_ctx, 0, cap_ctx.dmabuf_fds[cidx]) < 0) {
					perror("VIDIOC_QBUF input dmabuf");
					goto out;
				}
				cap_buf_for_ope_slot[0] = cidx;
			}
			/* depth>1 slots: capture buffers stay in capture device queue,
			 * fed to OPE asynchronously when capture frames arrive. */
		} else {
			if (input_fd >= 0) {
				lseek(input_fd, 0, SEEK_SET);
				ssize_t n = read(input_fd, in_ctx.bufs[i],
						 in_ctx.sizeimage);
				if (n < (ssize_t)in_ctx.sizeimage)
					fill_bayer_pattern(in_ctx.bufs[i],
							   in_ctx.width, in_ctx.height,
							   in_ctx.bytesperline, i,
							   in_ctx.fmt);
			} else {
				fill_bayer_pattern(in_ctx.bufs[i],
						   in_ctx.width, in_ctx.height,
						   in_ctx.bytesperline, i,
						   in_ctx.fmt);
			}
			if (vnode_qbuf(&in_ctx, i) < 0) {
				perror("VIDIOC_QBUF input");
				goto out;
			}
		}
	}

	/* Queue all output buffers */
	for (int i = 0; i < out_nbufs; i++) {
		if (vnode_qbuf(&out_ctx, i) < 0) {
			perror("VIDIOC_QBUF output");
			goto out;
		}
	}

	/* Stream on — output first, then input */
	int type;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(out_ctx.fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON output");
		goto out;
	}
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(in_ctx.fd, VIDIOC_STREAMON, &type) < 0) {
		perror("VIDIOC_STREAMON input");
		goto out;
	}
	uint64_t streamon_ns = now_ns();
	/* For capture path, submit_ns[0] set in pre-queue; others set in hot loop.
	 * For non-capture path, all buffers submitted at STREAMON. */
	if (!cfg->input_device) {
		for (unsigned int i = 0; i < depth; i++)
			submit_ns[i] = streamon_ns;
	} else {
		submit_ns[0] = streamon_ns; /* first capture frame submitted just before STREAMON */
	}

	if (cfg->duration_ms)
		printf("\nStreaming  %.1f s...\n", cfg->duration_ms / 1000.0);
	else
		printf("\nStreaming  %u frames...\n", cfg->num_frames);

	uint32_t frames_done = 0;
	uint32_t frame_num   = 0;
	int      stall_warned = 0;

	uint64_t deadline_ns = cfg->duration_ms
		? streamon_ns + (uint64_t)cfg->duration_ms * 1000000ULL
		: 0;

	while (!g_interrupted &&
	       (cfg->duration_ms ? now_ns() < deadline_ns
	                         : frames_done < cfg->num_frames)) {
		/* Poll: capture fd (when using capture device) + OPE output fd */
		struct pollfd pfds[2] = {
			{ .fd = out_ctx.fd,  .events = POLLIN },
			{ .fd = cfg->input_device ? cap_ctx.fd : -1, .events = POLLIN },
		};
		int nfds = cfg->input_device ? 2 : 1;
		int pret = poll(pfds, nfds, 2000);
		if (pret < 0) {
			if (errno == EINTR)
				break;
			perror("poll");
			goto out_streamoff;
		}
		if (pret == 0) {
			if (!stall_warned) {
				fprintf(stderr,
					"Warning: OPE output timeout — no output buffer available.\n"
					"  Try increasing pipeline depth with -d (current: %u).\n",
					depth);
				stall_warned = 1;
			}
			goto out_streamoff;
		}

		/* --- Capture fd ready: feed next frame to OPE input --- */
		if (cfg->input_device && (pfds[1].revents & POLLIN)) {
			uint64_t cap_qbuf_ns = now_ns();
			int cidx = capture_dqbuf(&cap_ctx);
			if (cidx < 0) {
				perror("capture: VIDIOC_DQBUF");
				goto out_streamoff;
			}
			/* Find a free OPE input slot (one that was just returned by dqbuf(in)) */
			/* We use in_idx_pending set below when OPE output was dequeued */
			/* For now feed into the slot that was freed by the last OPE cycle */
			/* This is handled below after dqbuf(in) */
			(void)cap_qbuf_ns; /* used below */
			/* Store cidx temporarily — consumed when we have a free OPE slot */
			/* Simple approach: process capture event only when OPE output also ready */
			/* If only capture is ready (no OPE output yet), just note it */
			(void)cidx;
			/* Re-queue capture buf immediately if OPE output not ready yet */
			if (!(pfds[0].revents & POLLIN)) {
				capture_qbuf(&cap_ctx, cidx);
				continue;
			}
			/* Both ready — fall through to OPE output handling below,
			 * using this cidx for the requeue */
			/* Store for use in the OPE output section */
			cap_buf_for_ope_slot[0] = cidx; /* temporary, overwritten below */
		}

		/* --- OPE output fd ready --- */
		if (!(pfds[0].revents & POLLIN))
			continue;

		int params_buf = -1;

		/* Dequeue OPE output */
		uint32_t out_idx;
		uint32_t out_seq;
		if (vnode_dqbuf(&out_ctx, &out_idx, &out_seq) < 0) {
			perror("VIDIOC_DQBUF output");
			goto out_streamoff;
		}
		uint64_t done_ns = now_ns();

		/* Dequeue OPE input (returns the slot that just finished) */
		uint32_t in_idx;
		if (vnode_dqbuf(&in_ctx, &in_idx, NULL) < 0) {
			perror("VIDIOC_DQBUF input");
			goto out_streamoff;
		}

		/* Grow timing arrays if needed */
		if (frames_done >= frame_ns_cap) {
			uint32_t new_cap = frame_ns_cap * 2;
			uint64_t *tmp = realloc(frame_ns, new_cap * sizeof(*frame_ns));
			if (tmp) { frame_ns = tmp; frame_ns_cap = new_cap; }
			if (capture_ns) {
				tmp = realloc(capture_ns, new_cap * sizeof(*capture_ns));
				if (tmp) capture_ns = tmp;
			}
			if (out_ns) {
				tmp = realloc(out_ns, new_cap * sizeof(*out_ns));
				if (tmp) out_ns = tmp;
			}
		}
		if (frames_done < frame_ns_cap)
			frame_ns[frames_done] = done_ns - submit_ns[in_idx];
		frames_done++;

		/* Update params:
		 * -R: cycle every frame with a new random config (stress test)
		 * -p: only update when interactive control sends a new config */
		if (params_ctx.fd >= 0) {
			if (cfg->randomize_params) {
				struct params_config new_cfg;
				params_config_default(&new_cfg);
				params_config_randomize(&new_cfg);
				params_buf = params_cycle(&params_ctx, &new_cfg);
			} else {
				struct params_config new_cfg;
				if (params_ctrl_get(&params_ctrl, &new_cfg))
					params_buf = params_cycle(&params_ctx, &new_cfg);
				/* else: buffer stays queued, no DQBUF/QBUF overhead */
			}
		}

		if (!cfg->duration_ms) {
			double frame_ms = (double)frame_ns[frames_done - 1] / 1e6;
			if (params_buf >= 0)
				printf("  seq=%-5u  buf: in=%u out=%u params=%d   %.3f ms  (%.1f fps)\n",
				       out_seq, in_idx, out_idx, params_buf,
				       frame_ms, 1000.0 / frame_ms);
			else
				printf("  seq=%-5u  buf: in=%u out=%u   %.3f ms  (%.1f fps)\n",
				       out_seq, in_idx, out_idx,
				       frame_ms, 1000.0 / frame_ms);
		}

#ifdef HAVE_GSTREAMER
		if (gst)
			gst_sink_push(gst, out_ctx.bufs[out_idx],
				      out_ctx.sizeimage, done_ns);
#endif

		/* Save output frame */
		if (out_file_fd >= 0 && frames_done == 1) {
			ssize_t n = write(out_file_fd, out_ctx.bufs[out_idx],
					  out_ctx.sizeimage);
			if (n < (ssize_t)out_ctx.sizeimage)
				fprintf(stderr, "Warning: short write to output file\n");
			else
				printf("  saved output frame to %s\n",
				       cfg->output_file);
		}

		if (out_ns && frames_done - 1 < frame_ns_cap)
			out_ns[frames_done - 1] = now_ns() - done_ns;

		/* Re-queue output buffer to OPE immediately */
		if (vnode_qbuf(&out_ctx, out_idx) < 0) {
			perror("VIDIOC_QBUF output requeue");
			goto out_streamoff;
		}

		if (!cfg->duration_ms && frames_done >= cfg->num_frames)
			break;

		frame_num++;
		/* Re-queue input: for capture path, get next camera frame and feed OPE;
		 * for non-capture path, requeue the same buffer. */
		if (cfg->input_device) {
			/* Return the capture buffer that was just used by OPE */
			int prev_cap = cap_buf_for_ope_slot[in_idx];
			if (prev_cap >= 0)
				capture_qbuf(&cap_ctx, prev_cap);
			/* Get next camera frame (may already be ready from the poll above) */
			uint64_t cap_qbuf_ns = now_ns();
			int cidx;
			if (pfds[1].revents & POLLIN) {
				/* Already dequeued above — use cap_buf_for_ope_slot[0] as temp */
				cidx = cap_buf_for_ope_slot[0];
			} else {
				/* Need to wait for next capture frame */
				cidx = capture_dqbuf(&cap_ctx);
				if (cidx < 0) {
					perror("capture: VIDIOC_DQBUF requeue");
					goto out_streamoff;
				}
			}
			submit_ns[in_idx] = now_ns();
			if (capture_ns && frames_done - 1 < frame_ns_cap)
				capture_ns[frames_done - 1] = submit_ns[in_idx] - cap_qbuf_ns;
			if (ope_in_qbuf_dmabuf(&in_ctx, in_idx, cap_ctx.dmabuf_fds[cidx]) < 0) {
				perror("VIDIOC_QBUF input dmabuf requeue");
				goto out_streamoff;
			}
			cap_buf_for_ope_slot[in_idx] = cidx;
		} else {
			submit_ns[in_idx] = now_ns();
			if (vnode_qbuf(&in_ctx, in_idx) < 0) {
				perror("VIDIOC_QBUF input requeue");
				goto out_streamoff;
			}
		}
	}

	ret = 0;
	uint64_t streamoff_ns = now_ns();
	print_timing_summary(frame_ns, capture_ns, out_ns, frames_done, streamoff_ns - streamon_ns);

out_streamoff:
	if (cfg->input_device) {
		ioctl(cap_ctx.fd, VIDIOC_STREAMOFF, &cap_ctx.type);
	}
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(in_ctx.fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(out_ctx.fd, VIDIOC_STREAMOFF, &type);

out:
	if (input_fd >= 0)    close(input_fd);
	if (out_file_fd >= 0) close(out_file_fd);

#ifdef HAVE_GSTREAMER
	gst_sink_close(gst);
#endif
	params_ctrl_stop(&params_ctrl);
	params_close(&params_ctx);
	free(params_cfgs);

	capture_ctx_close(&cap_ctx);
	vnode_ctx_close(&in_ctx);
	vnode_ctx_close(&out_ctx);
	free(frame_ns);
	free(capture_ns);
	free(out_ns);
	return ret;
}

void isp_test_enum_formats(struct isp_pipeline *pipe)
{
	printf("\n=== Supported formats per vnode ===\n");

	for (int i = 0; i < pipe->num_vnodes; i++) {
		struct isp_vnode *vn = &pipe->vnodes[i];

		if (!vn->devnode[0])
			continue;

		int fd = open(vn->devnode, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		printf("\n  %s (%s):\n", vn->name, vn->devnode);

		if (vn->is_meta) {
			struct v4l2_fmtdesc f = {
				.type = vn->is_output
					? V4L2_BUF_TYPE_META_OUTPUT
					: V4L2_BUF_TYPE_META_CAPTURE,
			};
			while (ioctl(fd, VIDIOC_ENUM_FMT, &f) == 0) {
				printf("    [%u] %.4s  %s\n",
				       f.index, (char *)&f.pixelformat,
				       f.description);
				f.index++;
			}
		} else {
			struct v4l2_fmtdesc f = {
				.type = vn->is_output
					? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
					: V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			};
			while (ioctl(fd, VIDIOC_ENUM_FMT, &f) == 0) {
				printf("    [%u] %.4s  %s\n",
				       f.index, (char *)&f.pixelformat,
				       f.description);
				f.index++;
			}
		}

		close(fd);
	}
	printf("\n");
}
