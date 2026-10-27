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

## Provenance and licences

Everything in this repository is GPL-2.0-only, like the kernel driver it
derives from.

| part | origin |
|---|---|
| `src/` stock files | `drivers/media/platform/verisilicon/` from Debian `linux-source-6.12` 6.12.113-1 (Linux 6.12.113), unchanged unless listed below |
| `src/Kbuild` | the upstream `Makefile` of that directory, with `obj-m` |

Rockchip's MPP library (Apache-2.0) is read to understand hardware behaviour
only; none of its code is included, because Apache-2.0 is not compatible with
GPLv2.

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
