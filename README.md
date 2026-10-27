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
taken from Paul Kocialkowski's series keep their GPL-2.0-or-later notice.
`debian/copyright` lists each file's licence.

| part | origin |
|---|---|
| `src/` stock files | `drivers/media/platform/verisilicon/` from Debian `linux-source-6.12` 6.12.113-1 (Linux 6.12.113), unchanged unless listed below |
| `src/Kbuild` | the upstream `Makefile` of that directory, with `obj-m` |
| `src/v4l2-h264-enc-rbsp.[ch]`, `src/v4l2-h264-enc-rc.[ch]`, `src/v4l2-h264-enc.h`, `src/v4l2-h264-enc-defs.h` | Paul Kocialkowski's v4l2-h264-enc series (linux-media, 2026-05-22, message-id `20260522101653.2565125-1-paulk@sys-base.io`), GPL-2.0-or-later, built into the module rather than exported. Changes: POC type 2 accepted; SPS/PPS `nal_ref_idc` 3 instead of 0; PPS High-profile tail written only when used; 64-bit rate-control products; `CONSTANT_QUALITY` mapping inverted to the spec's "100 is best"; `v4l2-h264-enc.h` trimmed to the state the writers use |
| `src/rockchip_vpu2_hw_h264_enc.c` | register programming from Bootlin's `rk3399_vpu_hw_h264_enc.c` (github.com/bootlin/linux, branch `hantro/h264-encoding-v5.11`), reworked for a stateful encoder |
| `src/hantro_h264_enc.c` | new: stateful front end (controls, GOP, SPS/PPS, references, rate control) |
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
