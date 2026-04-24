# camss-isp-m2m

Test and benchmark tool for Qualcomm CAMSS mem-to-mem ISP engines (OPE).

Discovers the OPE media device, sets up the V4L2 pipeline, and streams
frames through the ISP while measuring per-stage latency and throughput.

## Build

Native build on the target:
```sh
meson setup build
ninja -C build
```

Cross-compile:
```sh
meson setup build --cross-file <your-aarch64-cross-file>
ninja -C build
```

**GStreamer output** is optional. If `gstreamer-1.0` and `gstreamer-app-1.0`
are found at configure time, the `-g` option is enabled automatically:
```sh
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
meson setup build && ninja -C build
```

## Usage

```
camss-isp-m2m [options]

  -e              Enumerate formats on all vnodes and exit
  -t              Print media topology and exit
  -i <file>       Input raw Bayer frame file
  -I <device>     Live V4L2 capture device as input (e.g. /dev/video0)
  -o <file>       Output raw YUV frame file
  -g <pipeline>   GStreamer sink pipeline (e.g. "videoconvert ! autovideosink")
  -s <WxH>        Input size  (default: 640x480)
  -S <WxH>        Output size for scaling (default: same as input)
  -f <fourcc>     Input fourcc  (default: RGGB)
  -F <fourcc>     Output fourcc (default: NV12)
  -n <count>      Number of frames (default: 1)
  -T <seconds>    Run for a duration instead of frame count
  -d <depth>      Pipeline depth 1..3 (default: 1)
  -r <fps>        Frame rate in fps via VIDIOC_S_PARM
  -p              Enqueue params buffer with default ISP tuning values
  -R              Randomize params every frame (implies -p)
  -h              Show this help

Supported input formats:  RGGB BGGR GBRG GRBG (8-bit)
                          RG10P BG10P GB10P GR10P (10-bit packed MIPI)
Supported output formats: NV12 NV21 NV16 NV61 NV24 GREY
```

## Input sources

| Option | Description |
|--------|-------------|
| *(none)* | Generated Bayer gradient pattern |
| `-i <file>` | Raw Bayer frame loaded from file, reused every frame |
| `-I <device>` | Live V4L2 capture device; frames fed to OPE via dmabuf zero-copy |

When using `-I`, the capture device format is queried automatically.
Use `-f`/`-s` to override the format or resolution on the capture device.
The tool automatically adjusts the OPE input stride to match the capture
device's actual bytesperline (important for 10-bit packed formats).

## Output sinks

| Option | Description |
|--------|-------------|
| *(none)* | Discard output (throughput/latency measurement only) |
| `-o <file>` | Save first output frame to a raw YUV file |
| `-g <pipeline>` | Push every frame into a GStreamer pipeline via `appsrc` |

The GStreamer pipeline string is appended after `appsrc`. Examples:
```sh
-g "videoconvert ! autovideosink"
-g "videoconvert ! fpsdisplaysink video-sink=fakesink"
-g "x264enc ! mp4mux ! filesink location=out.mp4"
-g "waylandsink"
```

## Scaling

OPE supports downscaling. Specify different input and output sizes:
```sh
camss-isp-m2m -s 1920x1080 -S 1280x720 -n 10
```

## ISP parameters

### Static / interactive (`-p`)

Enqueues a **single** params buffer with default BT.601 tuning. The buffer
stays permanently queued — no per-frame overhead. While streaming, type
`block.field=value` on stdin to update parameters interactively:

```
wb_gain.g_gain=1200        # WB green gain (Q5.10, 1024=1.0)
wb_gain.b_gain=1500        # WB blue gain
wb_gain.r_gain=1500        # WB red gain
chroma_enhan.luma_v0=0x4d  # R->Y coefficient (12sQ8)
color_correct.a[0]=256     # CC matrix A[0]
color_correct.m=1          # CC Q mode (0-3)
reset                      # Reload defaults
help                       # Show all available commands
```

The change is applied on the next frame with a single DQBUF→refill→QBUF
cycle — no flickering.

### Randomize (`-R`)

Allocates `depth` params buffers (matching the pipeline depth) and cycles
through them every frame with a new random config. Useful for stress-testing
the params update path.

### Parameter block types

Defined in `include/linux/camss-config.h` (`CAMSS_PARAMS_WB_GAIN`,
`CAMSS_PARAMS_CHROMA_ENHAN`, `CAMSS_PARAMS_COLOR_CORRECT`). The format
fourcc is `V4L2_META_FMT_QCOM_ISP_PARAMS` (`QCIP`). When the header adds
new blocks, update `src/params_ctrl.c` only.

## Pipeline depth

`-d <depth>` controls how many input/output buffers are pre-queued to OPE.
With depth=1 (default), per-frame latency is accurate. With depth>1,
OPE can process the next frame while the previous output is being handled.

With a live capture device (`-I`), the pipeline is asynchronous:
capture frames are fed to OPE as soon as they arrive, and OPE output
buffers are returned as soon as they are processed.

## Results

```
Results:
  Throughput             600 frames   60.0 fps
  Cap latency            min=16.5 ms  max=17.1 ms  avg=16.7 ms
  Proc latency           min=10.8 ms  max=11.2 ms  avg=11.0 ms
  Out latency            min=0.1 ms   max=2.3 ms   avg=0.4 ms
```

| Row | Measures |
|-----|----------|
| **Throughput** | Wall-clock frames/second over the full run |
| **Cap latency** | Camera inter-frame interval (only with `-I`) |
| **Proc latency** | Pure OPE processing time (qbuf-input → dqbuf-output) |
| **Out latency** | Output handling time (GStreamer push, file write) |

## Pipeline topology

| Node | Direction | Type | Description |
|------|-----------|------|-------------|
| `frame-input` | output | video | Raw Bayer input |
| `frame-output` | capture | video | Processed YUV output |
| `params` | output | meta | Per-frame ISP tuning parameters |

## Examples

Print the pipeline topology:
```sh
camss-isp-m2m -t
```

Process 10 frames of a 640×480 RGGB pattern to NV12:
```sh
camss-isp-m2m -n 10
```

Process a raw file and save the output:
```sh
camss-isp-m2m -s 1920x1080 -i frame.raw -o out.yuv
```

Run for 10 seconds from a live camera, display via GStreamer:
```sh
camss-isp-m2m -I /dev/video0 -T 10 -g "videoconvert ! autovideosink"
```

Benchmark 1080p throughput for 30 seconds:
```sh
camss-isp-m2m -s 1920x1080 -T 30
```

Test downscaling 1080p → 720p:
```sh
camss-isp-m2m -s 1920x1080 -S 1280x720 -n 10
```

Interactive ISP tuning while streaming:
```sh
camss-isp-m2m -I /dev/video0 -T 60 -p -g "videoconvert ! autovideosink"
# then type: wb_gain.g_gain=1500
```

Stress-test params update path with randomized configs:
```sh
camss-isp-m2m -s 1920x1080 -T 10 -R -d 3
```

Encode output to MP4 (requires GStreamer with x264enc):
```sh
camss-isp-m2m -I /dev/video0 -T 30 \
  -g "videoconvert ! x264enc ! mp4mux ! filesink location=out.mp4"
```
