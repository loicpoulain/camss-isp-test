// SPDX-License-Identifier: GPL-2.0
/*
 * camss-isp-test: Qualcomm CAMSS OPE ISP pipeline test tool
 *
 * Usage:
 *   camss-isp-test [options]
 *
 * Options:
 *   -e              Enumerate formats on all pipeline vnodes and exit
 *   -t              Print media topology and exit
 *   -i <file>       Input raw Bayer frame file (default: generated pattern)
 *   -o <file>       Output raw YUV frame file (default: discard)
 *   -s <WxH>        Input size  (default: 640x480)
 *   -S <WxH>        Output size (default: same as input)
 *   -f <fourcc>     Input format fourcc (default: RGGB = V4L2_PIX_FMT_SRGGB8)
 *   -F <fourcc>     Output format fourcc (default: NV12)
 *   -n <count>      Number of frames to process (default: 1)
 *   -T <seconds>    Run for a duration instead of frame count
 *   -p              Enqueue a params buffer (BLS + WB + DEMO defaults)
 *   -P <wb_g_gain>  White balance green gain Q5.10 (default: 1024)
 *   -h              Show this help
 *   -d <depth>      Input pipeline depth 1..3 (default: 1)
 *   -r <fps>        Frame rate in fps, sets VIDIOC_S_PARM (default: driver default)
  -R              Randomize params before each frame (implies -p)
 *   -R              Randomize params buffer before each frame (implies -p)
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/videodev2.h>

#include "media.h"
#include "isp_test.h"
#include "params.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"\n"
		"  -e              Enumerate formats on all vnodes and exit\n"
		"  -t              Print media topology and exit\n"
		"  -i <file>       Input raw Bayer frame file\n"
		"  -o <file>       Output raw YUV frame file\n"
		"  -s <WxH>        Input size  (default: 640x480)\n"
		"  -S <WxH>        Output size (default: same as input)\n"
		"  -f <fourcc>     Input fourcc  (default: RGGB)\n"
		"  -F <fourcc>     Output fourcc (default: NV12)\n"
		"  -n <count>      Number of frames (default: 1)\n"
		"  -T <seconds>    Run for a duration instead of frame count\n"
		"  -d <depth>      Pipeline depth 1..3 (default: 1)\n"
		"  -r <fps>        Frame rate in fps via VIDIOC_S_PARM\n"
		"  -p              Enqueue params buffer with default values\n"
		"  -R              Randomize params before each frame (implies -p)\n"
		"  -h              Show this help\n"
		"\n"
		"Supported input formats:  RGGB BGGR GBRG GRBG (8-bit plain)\n"
		"Supported output formats: NV12 NV21 NV16 NV61 NV24 GREY\n",
		prog);
}

static uint32_t parse_fourcc(const char *s)
{
	if (strlen(s) != 4) {
		fprintf(stderr, "Invalid fourcc '%s' (must be 4 chars)\n", s);
		return 0;
	}
	return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}

int main(int argc, char *argv[])
{
	struct isp_pipeline pipe;
	struct frame_config cfg = {
		.input_fmt  = V4L2_PIX_FMT_SRGGB8,
		.output_fmt = V4L2_PIX_FMT_NV12,
		.width      = 640,
		.height     = 480,
		.num_frames = 1,
		.with_params = 0,
		.pipeline_depth = 1,
		.framerate = 0,
		.randomize_params = 0,
	};
	int do_enum     = 0;
	int do_topology = 0;
	int opt;

	while ((opt = getopt(argc, argv, "eti:o:s:S:f:F:n:T:d:r:pP:Rh")) != -1) {
		switch (opt) {
		case 'e': do_enum     = 1; break;
		case 't': do_topology = 1; break;
		case 'i': cfg.input_file  = optarg; break;
		case 'o': cfg.output_file = optarg; break;
		case 's': {
			unsigned int w = 0, h = 0;
			if (sscanf(optarg, "%ux%u", &w, &h) == 2) {
				cfg.width  = w;
				cfg.height = h;
			} else {
				fprintf(stderr, "Invalid input size '%s', use WxH\n", optarg);
				return 1;
			}
			break;
		}
		case 'S': {
			unsigned int w = 0, h = 0;
			if (sscanf(optarg, "%ux%u", &w, &h) == 2) {
				cfg.output_width  = w;
				cfg.output_height = h;
			} else {
				fprintf(stderr, "Invalid output size '%s', use WxH\n", optarg);
				return 1;
			}
			break;
		}
		case 'f': cfg.input_fmt   = parse_fourcc(optarg); break;
		case 'F': cfg.output_fmt  = parse_fourcc(optarg); break;
		case 'n': cfg.num_frames  = (uint32_t)atoi(optarg); break;
		case 'T': cfg.duration_ms = (uint32_t)(atof(optarg) * 1000); break;
		case 'd': cfg.pipeline_depth = (unsigned int)atoi(optarg); break;
		case 'r': cfg.framerate      = (unsigned int)atoi(optarg); break;
		case 'R': cfg.randomize_params = 1; cfg.with_params = 1;   break;
		case 'p': cfg.with_params = 1; break;
		case 'P':
			/* Custom WB green gain implies -p */
			cfg.with_params = 1;
			/* Store in output_file slot temporarily — handled below */
			/* Actually just set with_params; full params API via params.h */
			(void)atoi(optarg); /* TODO: expose full params_config via CLI */
			cfg.with_params = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (!cfg.input_fmt || !cfg.output_fmt) {
		fprintf(stderr, "Invalid format\n");
		return 1;
	}

	/* Discover the OPE pipeline */
	if (media_find_ope_pipeline(&pipe) < 0)
		return 1;

	if (do_topology || do_enum) {
		media_pipeline_print(&pipe);
		if (do_enum)
			isp_test_enum_formats(&pipe);
		media_pipeline_close(&pipe);
		return 0;
	}

	/* Always print topology for context */
	media_pipeline_print(&pipe);

	/* Run the streaming test */
	int ret = isp_test_run(&pipe, &cfg);

	media_pipeline_close(&pipe);
	return ret < 0 ? 1 : 0;
}
