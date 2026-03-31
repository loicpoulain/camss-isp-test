# camss-isp-test

Test tool for the Open Source Qualcomm CAMSS ISP.

Exercises the media-controller pipeline by:
- Discovering the OPE media device and its video nodes
- Enumerating supported formats on each endpoint
- Streaming raw Bayer frames through the ISP (input → output)
- Optionally injecting ISP tuning parameters (BLS, white balance, demosaic)

## Build

```sh
meson setup build --cross-file <your-aarch64-cross-file>
ninja -C build
```

Or natively on the target:
```sh
meson setup build
ninja -C build
```

## Usage

```
camss-isp-test [options]

  -t              Print media topology and exit
  -e              Enumerate formats on all vnodes and exit
  -i <file>       Input raw Bayer frame file (default: generated gradient)
  -o <file>       Output raw YUV frame file  (default: discard)
  -W <width>      Frame width  (default: 640)
  -H <height>     Frame height (default: 480)
  -f <fourcc>     Input fourcc  (default: RGGB = SRGGB8)
  -F <fourcc>     Output fourcc (default: NV12)
  -n <count>      Number of frames to process (default: 1)
  -p              Enqueue a params buffer with default tuning values
  -h              Show help
```

## Examples

Print the pipeline topology:
```sh
camss-isp-test -t
```

Enumerate supported formats:
```sh
camss-isp-test -e
```

Process one 1920×1080 RGGB8 frame to NV12, save output:
```sh
camss-isp-test -W 1920 -H 1080 -f RGGB -F NV12 -i frame.raw -o out.yuv
```

Process 10 frames with ISP parameters (BLS + WB + demosaic defaults):
```sh
camss-isp-test -W 640 -H 480 -n 10 -p
```

## Pipeline topology

The OPE pipeline exposes three video nodes:

| Node              | Direction | Type  | Description                    |
|-------------------|-----------|-------|--------------------------------|
| ope-frame-input   | output    | video | Raw Bayer input frame          |
| ope-frame-output  | capture   | video | Processed YUV output frame     |
| ope-params-input  | output    | meta  | Per-frame ISP tuning params    |

The params buffer uses the generic `v4l2_isp_params_buffer` format
(`V4L2_META_FMT_CAMSS_PARAMS` / `QCAP`). See
`include/linux/camss-config.h` for the block definitions.

## Notes

- The driver uses a shared context policy: all file handles share one
  processing context. This is a temporary workaround until V4L2/media
  gains a proper bind mechanism.
- The params endpoint is optional; streaming works without it using
  driver default tuning values.
- Output dimensions may differ from input when the output endpoint has
  `scaling = true` (OPE supports downscaling).
