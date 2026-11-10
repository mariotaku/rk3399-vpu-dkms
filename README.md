# rk3399-vpu-dkms

Hardware H.264 encoding on the Rockchip RK3399 for Debian's stock kernel.

Mainline's hantro driver (`drivers/media/platform/verisilicon`, module
`hantro-vpu`) drives the RK3399 VPU but exposes its encoder (VEPU2) for JPEG
only. This package ships a patched copy of Debian 6.12's hantro-vpu, built by
DKMS against `linux-headers-arm64` and installed into
`/lib/modules/<version>/updates/dkms`, where depmod prefers it over the
in-tree module. The patched driver keeps everything the stock one does (JPEG
encoder, VP8 and MPEG-2 decoders, every SoC the driver supports) and adds a
stateful V4L2 mem2mem H.264 encoder on RK3399's VEPU2.

## Design

- Interface: the existing stateful encoder uAPI
  (`Documentation/userspace-api/media/v4l/dev-encoder.rst`). `V4L2_PIX_FMT_H264`
  on the capture queue, NV12 on the output queue, `V4L2_ENC_CMD_STOP` drain, and
  the standard `V4L2_CID_MPEG_VIDEO_*` controls used by FFmpeg's `h264_v4l2m2m`
  and GStreamer's `v4l2h264enc`. No new uAPI; `videodev` and `v4l2-mem2mem` stay
  Debian's own modules.
- Division of work: the driver writes SPS and PPS (Annex-B, before every
  keyframe), the hardware writes the slice. Frame-level rate control runs in the
  kernel.
- Rate control, per frame (the hardware runs one QP for the whole frame):
  - Frame rate control off: constant QP from `V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP`
    and `..._P_FRAME_QP`.
  - `V4L2_MPEG_VIDEO_BITRATE_MODE_CQ`: QP from `V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY`
    (Paul Kocialkowski's mapping, inverted so that 100 is best).
  - `V4L2_MPEG_VIDEO_BITRATE_MODE_CBR`: the driver's own leaky bucket in
    `hantro_h264_enc.c`, plugged into the shared rate control as driver
    operations. The series' CBR moves QP by at most 3 per frame from the
    last few frames' sizes and has no buffer model; on natural video it
    overshot the bitrate by 40-160%. The leaky bucket keeps a virtual buffer
    of one second of bitrate near half full, models each frame type's
    complexity (size scaled to QP 0, halving every 6 QP) from the larger of
    the last and the average frame, splits the budget of a GOP between the
    I frame and the P frames by their measured size ratio, lets QP fall by
    at most 2 per frame and no more than 4 below the running P-frame
    average, and caps credit at half a second and debt at one second.
    Without look-ahead a scene change still costs up to about twice the
    bitrate for that second, followed by high QP while the debt is repaid.
  - VBR is not offered.
- The module keeps the name `hantro-vpu`. The in-tree module owns the whole
  `rockchip,rk3399-vpu` node (encoder and decoders share one register window,
  IOMMU and power domain), so the replacement must carry the whole driver.
- The Debian kernel headers package has no `drivers/` sources, so the full
  driver source is carried here, taken from `linux-source-6.12` of the same
  Debian version as the target kernel. `BUILD_EXCLUSIVE_KERNEL` limits DKMS to
  6.12 kernels.

## Hardware facts

Measured on a ROCK Pi 4 (RK3399) with this driver; the test method is in the
next section.

- The reconstruction buffer, which VEPU2 reads back as the reference of the
  next P-frame, is not a raster NV12 image although its size is: luma is
  stored as 16-byte-wide columns of the full coded height, chroma (CbCr
  interleaved) as 8-byte-wide columns of half that height, both with the
  bytes of each 64-bit word reversed. Only the hardware reads it, so the
  driver just allocates `ALIGN(w, 16) * ALIGN(h, 16) * 3 / 2` bytes per
  buffer, like Bootlin and MPP, and keeps two (current and reference).
- Inter frames need at least 8x4 macroblocks. At 6-7 macroblock columns
  P-frame output varied between runs with identical input and drifted from
  the decoder's picture; at 3 rows it drifted; at 2 rows the hardware
  stopped with its own timeout (interrupt status 0x441) and the next frame
  had to be an IDR. The H.264 capture format therefore starts at 116x50,
  the smallest visible size whose coded size is 128x64.
- The stream after the driver's SPS/PPS continues at an 8-byte-aligned
  address with the 0-7 bytes already written passed in the
  `STR_HDR_REM` registers and a bit offset; the hardware reports the
  stream length in bits from that aligned address.

## Interface details

- Raw (OUTPUT) formats: VEPU2 reads whole macroblock rows even when it
  crops; with planes sized exactly to 1920x1080 it faulted in the IOMMU
  (read past the buffer, after which the IOMMU needed a module reload).
  Single-buffer formats (NV12, YUV420, YUYV, UYVY, RGB) therefore keep the
  height the client asked for, with planes at `bytesperline * height` as
  V4L2 defines them, and pad the line stride to 16 pixels and `sizeimage`
  to the macroblock-aligned frame. Multi-buffer formats (NV12M, YUV420M)
  report the macroblock-aligned height, since that is the only way a client
  sizes each plane buffer for it. Both FFmpeg and GStreamer depend on this:
  FFmpeg copies `height` lines from its frame (a rounded-up height made it
  read past its own buffer and crash), and GStreamer places the second
  NV12M plane by `bytesperline * height`.
- The size a client asks for on OUTPUT is checked again when the coded
  format changes. FFmpeg and GStreamer set OUTPUT before CAPTURE, while the
  JPEG default (16-line steps) is still selected.
- Visible size: the CAPTURE format or the OUTPUT crop selection, as in the
  stock driver's JPEG encoder, as long as it lies inside the last macroblock
  column and row; it goes into the SPS frame cropping and the overfill
  registers.
- Frame interval: `VIDIOC_S_PARM` on OUTPUT; CAPTURE has none (no
  `V4L2_FMT_FLAG_ENC_CAP_FRAME_INTERVAL`).
- Drain: `V4L2_ENC_CMD_STOP`, last buffer flagged `V4L2_BUF_FLAG_LAST`, and
  `V4L2_EVENT_EOS` for clients that subscribe (the stock driver queued the
  event but refused the subscription).
- `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME` applies to the next frame the
  hardware encodes, which can be a frame already queued: neatvnc asking for
  a key frame before frame 30 got it at frame 29.
- RGB input: the hardware converts with full-range BT.709 coefficients, and
  the VUI says so (full range, BT.709 matrix, primaries and transfer of the
  RGB colorspace, sRGB by default). Each of the eight RGB fourccs was
  checked against FFmpeg's pixel format with the same byte order (RGB PSNR
  45/47/44 dB).
- `V4L2_CID_MPEG_VIDEO_HEADER_MODE` offers only `JOINED_WITH_1ST_FRAME`;
  FFmpeg's request for SEPARATE fails and FFmpeg carries on.
- `V4L2_CID_MIN_BUFFERS_FOR_OUTPUT` reads 1.
- In constant-QP and CQ modes P-frames at QP 48 came out about five times
  larger than at QP 16 on mostly static content. The QP-indexed skip, intra
  and inter tables are the same as MPP's, so this is the hardware's mode
  decision, not a porting error.

## Client compatibility

Measured on the board (Debian 13, FFmpeg 7.1 with v4l2-request, GStreamer
1.26, v4l-utils 1.30.1):

- FFmpeg `h264_v4l2m2m`: NV12 and YUV420P input at 640x360, 1280x720,
  1364x766 and 1920x1080; 2 Mbit/s requested, 1.90 Mbit/s produced at
  720p. FFmpeg's wrapper accepts only YUV input, so RGB goes through the
  other clients.
- GStreamer `v4l2h264enc`: NV12 and I420 (it picks NV12M and YUV420M) at
  640x360, 1364x766 and 1920x1080; 4 Mbit/s requested, 4.07 Mbit/s
  produced; decoded output matches the source frame for frame.
- neatvnc 0.9.1 (`tools/neatvnc/`, see below): RGB DMABUF input at
  1920x1080, CQ mode with quality 1, 10, 18, 30, 51 mapping to I/P QPs
  12/16 up to 44/48, forced key frames, colour bars within 3 levels of the
  source after decoding.
- v4l2-compliance 1.30.1 on the encoder node: 43 of 49 pass, 49 of 55 with
  streaming (`-s`, all four MMAP streaming tests pass). The failures:
  - `VIDIOC_QUERYCAP` (twice): the node encodes JPEG and H.264, and the
    tool allows one codec type per node. The stock driver's single encoder
    node serves every encoder format; splitting it is out of scope.
  - `VIDIOC_ENUM_FMT/FRAMESIZES`: the tool wants `VIDIOC_ENUM_FRAMESIZES`
    on raw formats from a stateful encoder. The specification makes those
    sizes depend on the coded format on CAPTURE, and GStreamer probes raw
    caps before it selects one; answering with the JPEG default's 16-pixel
    steps stopped `v4l2h264enc` from negotiating 360 or 766 lines. The
    driver keeps the stock answer (`ENOTTY`), which the specification
    allows ("the client may use"). The tool then stops collecting formats,
    so `G_FMT`, `TRY_FMT` and `S_FMT` fail on YUV420M as a consequence;
    with the frame sizes answered they passed.
  - With the frame sizes answered, `S_FMT` also failed on the JPEG raw
    colorspace check; the stock module fails it too.
- Stock functions, against the stock module on the same inputs: VP8 and
  MPEG-2 decode through FFmpeg `-hwaccel v4l2request` (the decoder's
  interrupt count rose by one per frame) and JPEG encode through GStreamer
  `v4l2jpegenc` give identical output hashes.

## Measurements

ROCK Pi 4 (RK3399, Debian 13, kernel 6.12.113), nothing else running, input
clips in RAM. Two 150-frame clips at 30 fps: natural footage with a scene
change (`nat`) and a UI screen recording with bursts of scrolling between
static stretches (`ui`), each at 1280x720 and 1920x1080. GOP 30, CBR.

Throughput and CPU (`tools/h264enc-test`, MMAP buffers filled by `memcpy`;
FFmpeg 7.1 `h264_v4l2m2m`; libx264 `ultrafast` on all six cores for scale):

| | 720p | 1080p |
|---|---|---|
| h264enc-test | 115 fps, 4.4 ms CPU per frame (13% of a core at 30 fps) | 60 fps, 9.1 ms per frame (27% of a core) |
| FFmpeg `h264_v4l2m2m` | 56-65 fps, 9-10 ms per frame (26-30%) | 39 fps, 14 ms per frame (41%) |
| libx264 ultrafast | 78-80 fps, 42 ms per frame | 48-50 fps, 85-88 ms per frame (2.6 cores at 30 fps) |

CBR accuracy, whole clip and per second (as a fraction of the target):

| clip | target | produced | per second |
|---|---|---|---|
| nat720 | 1 / 2 / 4 Mbit/s | -2.4% / -1.3% / -7.5% | 0.34-2.01 / 0.40-2.08 / 0.73-1.11 |
| ui720 | 1 / 2 / 4 Mbit/s | -4.5% / -11.4% / -14.3% | 0.79-1.08 / 0.76-1.01 / 0.70-1.09 |
| nat1080 | 2 / 4 / 8 Mbit/s | -4.9% / +1.9% / -6.8% | 0.39-2.00 / 0.40-2.23 / 0.74-1.16 |
| ui1080 | 2 / 4 / 8 Mbit/s | -3.8% / -13.3% / -15.4% | 0.79-1.10 / 0.73-0.99 / 0.69-1.01 |

The second at 2.0-2.2x the target is the scene change; the next seconds
repay it. The UI clip falls short at higher rates because its static
stretches cannot use the bits, and the encoder writes no filler.

Quality against libx264 at the same bitrate target (luma PSNR in dB / SSIM;
libx264 with `-maxrate` and `-bufsize` equal to the target):

| clip | target | VEPU2 | x264 veryfast, Baseline | x264 medium, High |
|---|---|---|---|---|
| nat720 | 1 Mbit/s | 29.40 / 0.877 | 34.17 / 0.954 | 36.45 / 0.973 |
| nat720 | 2 Mbit/s | 33.84 / 0.945 | 37.25 / 0.977 | 40.13 / 0.988 |
| nat720 | 4 Mbit/s | 38.47 / 0.978 | 40.07 / 0.988 | 43.82 / 0.995 |
| ui720 | 1 Mbit/s | 28.46 / 0.881 | 30.01 / 0.924 | 33.65 / 0.962 |
| ui720 | 2 Mbit/s | 32.36 / 0.940 | 35.25 / 0.970 | 38.06 / 0.983 |
| ui720 | 4 Mbit/s | 36.81 / 0.972 | 40.97 / 0.990 | 42.40 / 0.992 |
| nat1080 | 2 Mbit/s | 30.41 / 0.896 | 35.35 / 0.961 | 38.18 / 0.980 |
| nat1080 | 4 Mbit/s | 33.97 / 0.950 | 39.30 / 0.984 | 42.66 / 0.992 |
| nat1080 | 8 Mbit/s | 39.15 / 0.981 | 42.63 / 0.992 | 46.60 / 0.996 |
| ui1080 | 2 Mbit/s | 28.35 / 0.876 | 33.01 / 0.951 | 33.25 / 0.958 |
| ui1080 | 4 Mbit/s | 32.64 / 0.940 | 38.28 / 0.981 | 38.42 / 0.984 |
| ui1080 | 8 Mbit/s | 38.24 / 0.977 | 43.77 / 0.993 | 44.07 / 0.994 |

VEPU2 is 1.5-5.6 dB behind libx264 veryfast with the Baseline profile.
Part of that is the encoder (one reference, one QP per frame, no rate
distortion optimisation), part the frame-level rate control: the scene
change is followed by about a second at high QP, and the UI clip spends
less than its budget.

## Testing

`tools/h264enc-test` encodes raw frames through the stateful interface
(MMAP on both queues, `V4L2_ENC_CMD_STOP` drain). P-frames were validated
by comparing the hardware's reconstruction buffers, copied out after each
frame by a temporary debug build, with FFmpeg's decode of the stream
(decoder cropping off): they were bit-identical for every frame of 1280x720
(64 frames, one IDR), 1920x1080, 1364x766 and 116x50 (30 frames each). A
drifting encoder diverges from the decoder within a few frames, so this
check is stricter than PSNR against the source. Over 150 frames of
`testsrc2` at 720p, QP 28, one IDR, luma PSNR stays at 45.3 dB while Cb
falls from 43.8 to 37.3 dB; with the reconstruction bit-exact, that is the
encoder's own chroma coding loss, not drift (libx264 holds Cb at about
46 dB on the same input). On mostly static content (`testsrc`, QCIF, QP
26) P-frames are 270-370 bytes against 2850 for the IDR.

## Provenance and licences

The driver is GPL-2.0-only, like the kernel driver it derives from; the files
taken from Paul Kocialkowski's series keep their GPL-2.0-or-later notice,
and the stock files that the kernel ships under another licence keep it
(`hantro_jpeg.[ch]` GPL-2.0-or-later, `rockchip_av1_entropymode.c`
BSD-2-Clause, `rockchip_av1_filmgrain.c` GPL-2.0-only or Apache-2.0). The
repository follows the [REUSE](https://reuse.software/) specification:
`REUSE.toml` and the files' SPDX headers give each file's licence, and the
texts are in `LICENSES/` (GPL-2.0 also as `LICENSE`). `debian/copyright` says the same in Debian's
format.

| part | origin |
|---|---|
| `src/` stock files | `drivers/media/platform/verisilicon/` from Debian `linux-source-6.12` 6.12.113-1 (Linux 6.12.113), unchanged unless listed below |
| `src/Kbuild` | the upstream `Makefile` of that directory, with `obj-m` |
| `src/v4l2-h264-enc-rbsp.[ch]`, `src/v4l2-h264-enc-rc.[ch]`, `src/v4l2-h264-enc.h`, `src/v4l2-h264-enc-defs.h` | Paul Kocialkowski's v4l2-h264-enc series (linux-media, 2026-05-22, message-id `20260522101653.2565125-1-paulk@sys-base.io`), GPL-2.0-or-later, built into the module rather than exported. Changes: POC type 2 accepted; SPS/PPS `nal_ref_idc` 3 instead of 0; PPS High-profile tail written only when used; 64-bit rate-control products; `CONSTANT_QUALITY` mapping inverted to the spec's "100 is best"; `v4l2-h264-enc.h` trimmed to the state the writers use |
| `src/rockchip_vpu2_hw_h264_enc.c` | register programming from Bootlin's `rk3399_vpu_hw_h264_enc.c` (github.com/bootlin/linux, branch `hantro/h264-encoding-v5.11`), reworked for a stateful encoder |
| `src/hantro_h264_enc.c` | new: stateful front end (controls, GOP, SPS/PPS, references, CBR rate control) |
| `tools/neatvnc/stub/enc/h264-encoder.h` | neatvnc 0.9.1, ISC, copied unchanged so the test harness can build neatvnc's backend |

Rockchip's MPP library (Apache-2.0) is read to understand hardware behaviour
only; none of its code is included, because Apache-2.0 is not compatible with
GPLv2. loltt89/rk3318-vepu2 was read as a reference; none of its code is
included.

## neatvnc

neatvnc 0.9.1's V4L2 mem2mem H.264 backend (`src/enc/h264/v4l2m2m-impl.c`)
is one of the acceptance checks. Two of its behaviours contradict the V4L2
specification. The driver follows the specification; the test harness in
`tools/neatvnc/` builds neatvnc's backend with `v4l2m2m-fixes.patch`:

- Pixel format (`v4l2m2m-impl.c:159-193`, `v4l2_format_from_drm()`): the
  DRM-to-V4L2 mapping pairs formats by name, which reverses the byte order.
  DRM fourccs name components from the most significant bit of a
  little-endian word, V4L2's `XBGR32`-style names from the lowest byte, so
  `DRM_FORMAT_XRGB8888` and `V4L2_PIX_FMT_XBGR32` are the same code (`XR24`)
  and the same layout, whereas neatvnc maps `DRM_FORMAT_XRGB8888` to
  `V4L2_PIX_FMT_XRGB32` (`BX24`, bytes X, R, G, B). On a correct driver the
  stock mapping swaps red and blue. The patch first tries the DRM fourcc
  itself as the V4L2 fourcc.
- Quality (`v4l2m2m-impl.c:536-538`, fed from `open-h264.c:136`): neatvnc
  passes `51 - round(50/9 * quality)`, a QP-like value where lower is
  better, straight into `V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY`, whose range
  is 1..100 with 100 the best quality. The patch maps it as
  `100 - (q - 1) * 99 / 50`.

neatvnc queues `stride * ALIGN(height, 16)` bytes from each source DMABUF,
so the buffer objects it is given must cover the macroblock-aligned height
(1088 rows for 1080p); vb2 rejects a smaller buffer.

`tools/neatvnc/harness.c` drives that backend without a VNC server: it
allocates linear XBGR8888 GBM buffer objects on the render node, draws 75%
colour bars with a moving square, feeds them as neatvnc's encoder would
(at most two frames in flight, key frames requested at the start and in
the middle) and writes the packets to a file. Build it on the board with
`make NEATVNC_SRC=<neatvnc-0.9.1 tree with v4l2m2m-fixes.patch applied>`.

## Building and installing

On the board (Debian 13, arm64):

    sudo apt install dkms linux-headers-arm64
    sudo apt install ./rk3399-vpu-dkms_<version>_all.deb
    sudo modprobe -r hantro-vpu && sudo modprobe hantro-vpu
    modinfo -n hantro-vpu    # .../updates/dkms/hantro-vpu.ko.xz

Building the package (any architecture; it is `Architecture: all`) needs
`debhelper` and `dh-dkms`:

    dpkg-buildpackage -us -uc -b    # the .deb lands in the parent directory

Building the module by hand against the running kernel: `make -C src`.
To cross-build, pass `KDIR=<arm64 headers> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-`.
Debian's arm64 headers link `scripts/` to the arm64 `linux-kbuild` package; on
an x86 host extract the amd64 `linux-kbuild-<version>` of the same version
next to them instead.

## CI

`.github/workflows/dkms.yml` runs `ci/dkms.sh` on every push to `main`, on
pull requests and on manual runs, in a `debian:trixie` container on GitHub's
arm64 runner: it builds the package with `dpkg-buildpackage`, installs it
with `linux-headers-arm64`, has DKMS build and install the module for that
headers package's kernel (not the runner's own), and checks the module with
`modinfo`: vermagic, the RK3399 aliases, the parameters and the encoder's
symbols. A manual run (Actions, DKMS, "Run workflow") keeps the `.deb` as
the run's `rk3399-vpu-dkms` artifact. Pushing a tag `v<version>`, where the
version is the top entry of `debian/changelog`, publishes the `.deb` as a
GitHub release with that entry as its notes; the run fails if the two
differ. The same script runs locally in an arm64 container:

    docker run --rm --platform linux/arm64 -v "$PWD:/src" -w /src \
        debian:trixie ci/dkms.sh dist
