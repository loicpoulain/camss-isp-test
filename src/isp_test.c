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
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "isp_test.h"
#include "media.h"
#include "params.h"

#define MAX_PIPELINE_BUFS 3

struct vnode_ctx {
	int      fd;
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

static int vnode_open_and_set_fmt(struct vnode_ctx *v, const char *devnode,
				  uint32_t type, uint32_t fourcc,
				  uint32_t width, uint32_t height)
{
	struct v4l2_format fmt = {};
	int ret;

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

	printf("  %s: %ux%u %.4s bpl=%u size=%u\n",
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

static int vnode_dqbuf(struct vnode_ctx *v, uint32_t *index)
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
	return 0;
}

/* Timing helpers */
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void print_timing_summary(const uint64_t *frame_ns, uint32_t count)
{
	uint64_t total = 0, min = UINT64_MAX, max = 0;

	for (uint32_t i = 0; i < count; i++) {
		total += frame_ns[i];
		if (frame_ns[i] < min) min = frame_ns[i];
		if (frame_ns[i] > max) max = frame_ns[i];
	}

	printf("\n--- Frame processing time (qbuf-input -> dqbuf-output) ---\n");
	printf("  frames : %u\n", count);
	printf("  min    : %.3f ms  (%.1f fps)\n", (double)min   / 1e6, 1e9 / (double)min);
	printf("  max    : %.3f ms  (%.1f fps)\n", (double)max   / 1e6, 1e9 / (double)max);
	printf("  avg    : %.3f ms  (%.1f fps)\n", (double)total / 1e6 / count, 1e9 * count / (double)total);
	printf("  total  : %.3f ms\n", (double)total / 1e6);
}

/* Fill input buffer with a simple Bayer gradient pattern */
static void fill_bayer_pattern(void *buf, uint32_t width, uint32_t height,
				uint32_t bytesperline, uint32_t frame_num)
{
	uint8_t *p = buf;

	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			/* Simple gradient with frame offset for animation */
			p[y * bytesperline + x] =
				(uint8_t)((x + y + frame_num * 4) & 0xff);
		}
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
	struct isp_vnode *in_vn, *out_vn, *params_vn;
	int params_fd = -1;
	int ret = -1;
	int out_file_fd = -1;
	uint64_t submit_ns[MAX_PIPELINE_BUFS] = {};
	uint64_t *frame_ns = NULL;

	printf("\n=== ISP test: %ux%u %.4s -> %.4s (%u frames) ===\n",
	       cfg->width, cfg->height,
	       (char *)&cfg->input_fmt, (char *)&cfg->output_fmt,
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
				   cfg->output_fmt, cfg->width, cfg->height) < 0)
		goto out;

	/* Open params vnode last so it joins the shared context */
	if (cfg->with_params) {
		if (!params_vn || !params_vn->devnode[0]) {
			fprintf(stderr, "Warning: params vnode not found, continuing without\n");
		} else {
		struct params_config pcfg;
		params_config_default(&pcfg);
		params_fd = params_enqueue(params_vn->devnode, &pcfg);
		if (params_fd < 0)
			fprintf(stderr, "Warning: params enqueue failed, continuing without\n");
		}
	}

	/* Allocate buffers: depth clamped to [1, MAX_PIPELINE_BUFS] and num_frames */
	unsigned int depth = cfg->pipeline_depth ? cfg->pipeline_depth : 1;
	if (depth > MAX_PIPELINE_BUFS) depth = MAX_PIPELINE_BUFS;
	if (depth > cfg->num_frames)   depth = cfg->num_frames;
	int in_nbufs  = vnode_alloc_bufs(&in_ctx,  depth);
	int out_nbufs = vnode_alloc_bufs(&out_ctx, depth);
	if (in_nbufs < 0 || out_nbufs < 0)
		goto out;

	frame_ns = calloc(cfg->num_frames, sizeof(*frame_ns));
	if (!frame_ns)
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

	/* Pre-queue `depth` input buffers. depth=1 gives accurate per-frame
	 * timing; higher values increase throughput but timestamps reflect
	 * STREAMON time for all pre-queued buffers. */
	for (int i = 0; i < (int)depth; i++) {
		if (input_fd >= 0) {
			ssize_t n = read(input_fd, in_ctx.bufs[i],
					 in_ctx.sizeimage);
			if (n < (ssize_t)in_ctx.sizeimage)
				fill_bayer_pattern(in_ctx.bufs[i],
						   in_ctx.width, in_ctx.height,
						   in_ctx.bytesperline, i);
		} else {
			fill_bayer_pattern(in_ctx.bufs[i],
					   in_ctx.width, in_ctx.height,
					   in_ctx.bytesperline, i);
		}
		if (vnode_qbuf(&in_ctx, i) < 0) {
			perror("VIDIOC_QBUF input");
			goto out;
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
	/* All pre-queued input buffers start at STREAMON time */
	uint64_t streamon_ns = now_ns();
	for (unsigned int i = 0; i < depth; i++)
		submit_ns[i] = streamon_ns;

	printf("Streaming started, processing %u frames...\n", cfg->num_frames);

	uint32_t frames_done = 0;
	uint32_t frame_num   = 0;

	while (frames_done < cfg->num_frames) {
		/* Poll for output buffer ready */
		struct pollfd pfd = {
			.fd     = out_ctx.fd,
			.events = POLLIN,
		};
		int pret = poll(&pfd, 1, 2000);
		if (pret < 0) {
			perror("poll");
			goto out_streamoff;
		}
		if (pret == 0) {
			fprintf(stderr, "Timeout waiting for output frame %u\n",
				frames_done);
			goto out_streamoff;
		}

		/* Dequeue output */
		uint32_t out_idx;
		if (vnode_dqbuf(&out_ctx, &out_idx) < 0) {
			perror("VIDIOC_DQBUF output");
			goto out_streamoff;
		}
		uint64_t done_ns = now_ns();

		/* Dequeue input */
		uint32_t in_idx;
		if (vnode_dqbuf(&in_ctx, &in_idx) < 0) {
			perror("VIDIOC_DQBUF input");
			goto out_streamoff;
		}

		frame_ns[frames_done] = done_ns - submit_ns[in_idx];
		frames_done++;
		double frame_ms = (double)frame_ns[frames_done - 1] / 1e6;
		printf("  frame %u/%u done (in_buf=%u out_buf=%u) %.3f ms (%.1f fps)\n",
		       frames_done, cfg->num_frames, in_idx, out_idx,
		       frame_ms, 1000.0 / frame_ms);

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

		if (frames_done >= cfg->num_frames)
			break;

		/* Re-fill and re-queue input */
		frame_num++;
		if (input_fd < 0)
			fill_bayer_pattern(in_ctx.bufs[in_idx],
					   in_ctx.width, in_ctx.height,
					   in_ctx.bytesperline, frame_num);
		submit_ns[in_idx] = now_ns();
		if (vnode_qbuf(&in_ctx, in_idx) < 0) {
			perror("VIDIOC_QBUF input requeue");
			goto out_streamoff;
		}
		if (vnode_qbuf(&out_ctx, out_idx) < 0) {
			perror("VIDIOC_QBUF output requeue");
			goto out_streamoff;
		}
	}

	ret = 0;
	print_timing_summary(frame_ns, frames_done);
	printf("Test complete: %u frames processed\n", frames_done);

out_streamoff:
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(in_ctx.fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(out_ctx.fd, VIDIOC_STREAMOFF, &type);

out:
	if (input_fd >= 0)    close(input_fd);
	if (out_file_fd >= 0) close(out_file_fd);

	if (params_fd >= 0) {
		int t = V4L2_BUF_TYPE_META_OUTPUT;
		ioctl(params_fd, VIDIOC_STREAMOFF, &t);
		close(params_fd);
	}

	vnode_ctx_close(&in_ctx);
	vnode_ctx_close(&out_ctx);
	free(frame_ns);
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
