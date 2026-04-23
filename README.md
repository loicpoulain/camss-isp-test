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
  -R              Randomize params before each frame (implies -p)
  -h              Show this help

Supported input formats:  RGGB BGGR GBRG GRBG (8-bit plain Bayer)
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

The `-p` option enqueues a params buffer with default BT.601 tuning
(white balance, demosaic, chroma enhancement, colour correction).

`-R` randomises the white balance and colour correction matrix before
each frame, useful for testing parameter update paths.

Parameter block types are defined in `include/linux/camss-config.h`
(`CAMSS_PARAMS_WB_GAIN`, `CAMSS_PARAMS_CHROMA_ENHAN`,
`CAMSS_PARAMS_COLOR_CORRECT`). The format fourcc is
`V4L2_META_FMT_QCOM_ISP_PARAMS` (`QCIP`).

## Pipeline depth

`-d <depth>` controls how many input buffers are pre-queued to OPE.
With depth=1 (default), per-frame latency is accurate. With depth>1,
OPE can process the next frame while the previous output is being
handled, increasing throughput at the cost of latency accuracy.

With a live capture device (`-I`), the pipeline is asynchronous:
capture frames are fed to OPE as soon as they arrive, and OPE output
buffers are returned as soon as they are processed.

## Results

At the end of each run, a summary is printed:

```
Results:
  Throughput             600 frames   60.0 fps
  Cap latency            min=16.5 ms  max=17.1 ms  avg=16.7 ms
                         (60.6 fps)   (58.5 fps)   (59.9 fps)
  Proc latency           min=10.8 ms  max=11.2 ms  avg=11.0 ms
                         (92.6 fps)   (89.3 fps)   (91.0 fps)
  Out latency            min=0.1 ms   max=2.3 ms   avg=0.4 ms
                         (...)        (...)        (...)
```

| Row | Measures |
|-----|----------|
| **Throughput** | Wall-clock frames/second over the full run |
| **Cap latency** | Time from returning a capture buffer to receiving the next frame (camera inter-frame interval); only shown with `-I` |
| **Proc latency** | Time from submitting a frame to OPE input to receiving it from OPE output (pure ISP processing time) |
| **Out latency** | Time spent in output handling (GStreamer push, file write, etc.) after OPE finishes |

## Pipeline topology

The OPE pipeline exposes three video nodes:

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

Benchmark 1080p throughput for 30 seconds (targetting 1/240s per frame):
```sh
camss-isp-m2m -s 1920x1080 -T 30 -r 240
```

Test downscaling 1080p → 720p:
```sh
camss-isp-m2m -s 1920x1080 -S 1280x720 -n 10
```

Run with randomised ISP params and GStreamer fakesink fps counter:
```sh
camss-isp-m2m -I /dev/video0 -T 10 -R \
  -g "queue ! fpsdisplaysink video-sink=fakesink"
```

Encode output to MP4 (requires GStreamer with x264enc):
```sh
camss-isp-m2m -I /dev/video0 -T 30 \
  -g "videoconvert ! x264enc ! mp4mux ! filesink location=out.mp4"
```
