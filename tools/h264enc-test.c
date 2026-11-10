// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me>
/*
 * Minimal stateful V4L2 H.264 encoder client for rk3399-vpu-dkms.
 *
 * Reads raw frames (NV12 by default) from a file, encodes them through a
 * V4L2 mem2mem encoder node and writes the Annex-B stream to a file. It
 * uses only the standard stateful encoder interface, MMAP buffers on both
 * queues, and drains with V4L2_ENC_CMD_STOP.
 *
 * Copyright (C) 2026 rk3399-vpu-dkms contributors
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <linux/videodev2.h>

#define MAX_BUFS 8

struct buf {
	void *addr[VIDEO_MAX_PLANES];
	size_t len[VIDEO_MAX_PLANES];
	bool queued;
};

static int fd;
static struct v4l2_format out_fmt, cap_fmt;
static struct buf out_bufs[MAX_BUFS], cap_bufs[MAX_BUFS];
static unsigned int n_out, n_cap;

static void die(const char *what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	exit(1);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int set_ctrl(uint32_t id, int32_t value, const char *name)
{
	struct v4l2_ext_control ctrl = { .id = id, .value = value };
	struct v4l2_ext_controls ctrls = {
		.which = V4L2_CTRL_WHICH_CUR_VAL,
		.count = 1,
		.controls = &ctrl,
	};

	if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls)) {
		fprintf(stderr, "warning: %s = %d: %s\n", name, value,
			strerror(errno));
		return -1;
	}
	return 0;
}

static unsigned int alloc_bufs(enum v4l2_buf_type type, struct buf *bufs,
			       unsigned int count)
{
	struct v4l2_requestbuffers req = {
		.count = count, .type = type, .memory = V4L2_MEMORY_MMAP,
	};
	unsigned int i, p;

	if (ioctl(fd, VIDIOC_REQBUFS, &req))
		die("VIDIOC_REQBUFS");
	if (req.count > MAX_BUFS)
		req.count = MAX_BUFS;

	for (i = 0; i < req.count; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
		struct v4l2_buffer b = {
			.type = type, .memory = V4L2_MEMORY_MMAP, .index = i,
			.length = VIDEO_MAX_PLANES, .m.planes = planes,
		};

		if (ioctl(fd, VIDIOC_QUERYBUF, &b))
			die("VIDIOC_QUERYBUF");
		for (p = 0; p < b.length; p++) {
			bufs[i].len[p] = planes[p].length;
			bufs[i].addr[p] = mmap(NULL, planes[p].length,
					       PROT_READ | PROT_WRITE, MAP_SHARED,
					       fd, planes[p].m.mem_offset);
			if (bufs[i].addr[p] == MAP_FAILED)
				die("mmap");
		}
	}
	return req.count;
}

static int queue(enum v4l2_buf_type type, unsigned int index,
		 const uint32_t *bytesused, unsigned int nplanes,
		 uint64_t ts_us)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
	struct v4l2_buffer b = {
		.type = type, .memory = V4L2_MEMORY_MMAP, .index = index,
		.length = nplanes, .m.planes = planes,
		.timestamp.tv_sec = ts_us / 1000000,
		.timestamp.tv_usec = ts_us % 1000000,
	};
	unsigned int p;

	for (p = 0; p < nplanes; p++)
		planes[p].bytesused = bytesused ? bytesused[p] : 0;
	return ioctl(fd, VIDIOC_QBUF, &b);
}

/*
 * Copy one frame from a tightly packed file layout into the driver's
 * layout (bytesperline, macroblock-aligned height), repeating the last
 * line into the padding.
 */
static bool read_frame(FILE *in, unsigned int w, unsigned int h,
		       struct buf *b, uint32_t *used)
{
	const struct v4l2_pix_format_mplane *f = &out_fmt.fmt.pix_mp;
	uint32_t fourcc = f->pixelformat;
	bool nv12 = fourcc == V4L2_PIX_FMT_NV12 || fourcc == V4L2_PIX_FMT_NV12M;
	bool yuv420 = fourcc == V4L2_PIX_FMT_YUV420 || fourcc == V4L2_PIX_FMT_YUV420M;
	unsigned int bpl = f->plane_fmt[0].bytesperline;
	unsigned int ph = f->height;
	unsigned int nplanes = nv12 ? 2 : yuv420 ? 3 : 1;
	unsigned int p, y;

	for (p = 0; p < nplanes; p++) {
		unsigned int rows = p ? (h + 1) / 2 : h;
		unsigned int prow = p ? ph / 2 : ph;
		unsigned int cols, stride;
		uint8_t *dst;

		if (!nv12 && !yuv420)
			cols = w * 4;	/* 32-bit RGB */
		else if (p == 0 || nv12)
			cols = w;
		else
			cols = (w + 1) / 2;

		if (f->num_planes > 1) {
			dst = b->addr[p];
			stride = f->plane_fmt[p].bytesperline;
		} else {
			stride = p ? (nv12 ? bpl : bpl / 2) : bpl;
			dst = (uint8_t *)b->addr[0] + (p ? bpl * ph : 0) +
			      (p == 2 ? bpl * ph / 4 : 0);
		}

		for (y = 0; y < prow; y++) {
			uint8_t *row = dst + (size_t)y * stride;

			if (y < rows) {
				if (fread(row, 1, cols, in) != cols)
					return false;
			} else {
				memcpy(row, row - stride, stride);
			}
		}
		if (f->num_planes > 1)
			used[p] = f->plane_fmt[p].sizeimage;
	}
	if (f->num_planes == 1)
		used[0] = f->plane_fmt[0].sizeimage;
	return true;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s -d /dev/videoN -s WxH -i in.yuv -o out.h264 [options]\n"
		"  -f fourcc   OUTPUT format (NV12, NM12, YU12, XB24...; default NV12)\n"
		"  -n frames   frames to encode (default: whole file)\n"
		"  -r fps      frame rate (default 30)\n"
		"  -b bitrate  bits per second, CBR (default 2000000)\n"
		"  -q qp       constant QP (frame rate control off)\n"
		"  -Q quality  constant quality 1..100\n"
		"  -g gop      IDR period (default 30, 0 = first frame only)\n"
		"  -k list     force key frames at these frame numbers (a,b,c)\n",
		argv0);
	exit(2);
}

int main(int argc, char **argv)
{
	const char *dev = NULL, *in_path = NULL, *out_path = NULL;
	char *keys = NULL;
	uint32_t fourcc = V4L2_PIX_FMT_NV12;
	unsigned int w = 0, h = 0, fps = 30, frames = ~0U, gop = 30, i;
	int bitrate = 2000000, qp = -1, quality = -1, opt;
	unsigned int sent = 0, got = 0, keyframes = 0;
	uint64_t bytes = 0;
	bool eof = false, draining = false, done = false;
	double t0, t1;
	FILE *in, *out;

	while ((opt = getopt(argc, argv, "d:s:i:o:f:n:r:b:q:Q:g:k:")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 's': sscanf(optarg, "%ux%u", &w, &h); break;
		case 'i': in_path = optarg; break;
		case 'o': out_path = optarg; break;
		case 'f':
			if (strlen(optarg) != 4)
				usage(argv[0]);
			fourcc = v4l2_fourcc(optarg[0], optarg[1], optarg[2], optarg[3]);
			break;
		case 'n': frames = atoi(optarg); break;
		case 'r': fps = atoi(optarg); break;
		case 'b': bitrate = atoi(optarg); break;
		case 'q': qp = atoi(optarg); break;
		case 'Q': quality = atoi(optarg); break;
		case 'g': gop = atoi(optarg); break;
		case 'k': keys = optarg; break;
		default: usage(argv[0]);
		}
	}
	if (!dev || !in_path || !out_path || !w || !h)
		usage(argv[0]);

	in = fopen(in_path, "rb");
	out = fopen(out_path, "wb");
	if (!in || !out)
		die("fopen");

	fd = open(dev, O_RDWR | O_NONBLOCK);
	if (fd < 0)
		die("open");

	/* CAPTURE first, as dev-encoder.rst describes. */
	cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cap_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
	cap_fmt.fmt.pix_mp.width = w;
	cap_fmt.fmt.pix_mp.height = h;
	cap_fmt.fmt.pix_mp.num_planes = 1;
	if (ioctl(fd, VIDIOC_S_FMT, &cap_fmt))
		die("VIDIOC_S_FMT capture");
	if (cap_fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_H264) {
		fprintf(stderr, "no H.264 on this node\n");
		return 1;
	}

	out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	out_fmt.fmt.pix_mp.pixelformat = fourcc;
	out_fmt.fmt.pix_mp.width = w;
	out_fmt.fmt.pix_mp.height = h;
	if (ioctl(fd, VIDIOC_S_FMT, &out_fmt))
		die("VIDIOC_S_FMT output");
	if (out_fmt.fmt.pix_mp.pixelformat != fourcc) {
		fprintf(stderr, "OUTPUT format %.4s not accepted\n", (char *)&fourcc);
		return 1;
	}
	if (ioctl(fd, VIDIOC_G_FMT, &cap_fmt))
		die("VIDIOC_G_FMT capture");
	fprintf(stderr, "OUTPUT %.4s %ux%u bpl %u, CAPTURE %ux%u sizeimage %u\n",
		(char *)&out_fmt.fmt.pix_mp.pixelformat,
		out_fmt.fmt.pix_mp.width, out_fmt.fmt.pix_mp.height,
		out_fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
		cap_fmt.fmt.pix_mp.width, cap_fmt.fmt.pix_mp.height,
		cap_fmt.fmt.pix_mp.plane_fmt[0].sizeimage);

	struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
	parm.parm.output.timeperframe.numerator = 1;
	parm.parm.output.timeperframe.denominator = fps;
	if (ioctl(fd, VIDIOC_S_PARM, &parm))
		fprintf(stderr, "warning: VIDIOC_S_PARM: %s\n", strerror(errno));

	set_ctrl(V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop");
	if (qp >= 0) {
		set_ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0, "rc enable");
		set_ctrl(V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, qp, "i qp");
		set_ctrl(V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, qp, "p qp");
	} else if (quality >= 0) {
		set_ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "rc enable");
		set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			 V4L2_MPEG_VIDEO_BITRATE_MODE_CQ, "bitrate mode");
		set_ctrl(V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, quality, "quality");
	} else {
		set_ctrl(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1, "rc enable");
		set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			 V4L2_MPEG_VIDEO_BITRATE_MODE_CBR, "bitrate mode");
		set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate");
	}

	n_out = alloc_bufs(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, out_bufs, 4);
	n_cap = alloc_bufs(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, cap_bufs, 4);
	for (i = 0; i < n_cap; i++)
		if (queue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, i, NULL, 1, 0))
			die("VIDIOC_QBUF capture");

	int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type))
		die("VIDIOC_STREAMON output");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (ioctl(fd, VIDIOC_STREAMON, &type))
		die("VIDIOC_STREAMON capture");

	t0 = now();
	while (!done) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };

		/* Fill free OUTPUT buffers. */
		for (i = 0; i < n_out && !eof; i++) {
			uint32_t used[VIDEO_MAX_PLANES] = { 0 };

			if (out_bufs[i].queued)
				continue;
			if (sent >= frames ||
			    !read_frame(in, w, h, &out_bufs[i], used)) {
				eof = true;
				break;
			}
			if (keys) {
				char *k = keys;

				while (k && *k) {
					if ((unsigned int)atoi(k) == sent)
						set_ctrl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
							 1, "force key frame");
					k = strchr(k, ',');
					if (k)
						k++;
				}
			}
			if (queue(V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, i, used,
				  out_fmt.fmt.pix_mp.num_planes,
				  (uint64_t)sent * 1000000 / fps))
				die("VIDIOC_QBUF output");
			out_bufs[i].queued = true;
			sent++;
		}

		if (eof && !draining) {
			struct v4l2_encoder_cmd cmd = { .cmd = V4L2_ENC_CMD_STOP };

			if (ioctl(fd, VIDIOC_ENCODER_CMD, &cmd))
				die("VIDIOC_ENCODER_CMD");
			draining = true;
		}

		if (poll(&pfd, 1, 3000) <= 0) {
			fprintf(stderr, "timeout waiting for the encoder\n");
			return 1;
		}

		for (;;) {
			struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
			struct v4l2_buffer b = {
				.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
				.memory = V4L2_MEMORY_MMAP,
				.length = VIDEO_MAX_PLANES, .m.planes = planes,
			};

			if (ioctl(fd, VIDIOC_DQBUF, &b))
				break;
			out_bufs[b.index].queued = false;
		}

		for (;;) {
			struct v4l2_plane plane = { 0 };
			struct v4l2_buffer b = {
				.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
				.memory = V4L2_MEMORY_MMAP,
				.length = 1, .m.planes = &plane,
			};

			if (ioctl(fd, VIDIOC_DQBUF, &b)) {
				if (errno == EPIPE)
					done = true;
				break;
			}
			if (b.flags & V4L2_BUF_FLAG_ERROR)
				fprintf(stderr, "frame %u: error flag\n", got);
			if (plane.bytesused) {
				fwrite(cap_bufs[b.index].addr[0], 1,
				       plane.bytesused, out);
				bytes += plane.bytesused;
				if (b.flags & V4L2_BUF_FLAG_KEYFRAME)
					keyframes++;
				fprintf(stderr, "frame %u: %u bytes%s ts %ld.%06ld\n",
					got, plane.bytesused,
					(b.flags & V4L2_BUF_FLAG_KEYFRAME) ? " key" : "",
					(long)b.timestamp.tv_sec,
					(long)b.timestamp.tv_usec);
				got++;
			}
			if (b.flags & V4L2_BUF_FLAG_LAST) {
				done = true;
				break;
			}
			if (queue(V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.index,
				  NULL, 1, 0))
				die("VIDIOC_QBUF capture");
		}
	}
	t1 = now();

	fprintf(stderr,
		"encoded %u of %u frames, %u key, %llu bytes, %.1f fps, %.0f kbit/s at %u fps\n",
		got, sent, keyframes, (unsigned long long)bytes,
		got / (t1 - t0), got ? bytes * 8.0 * fps / got / 1000 : 0, fps);

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	ioctl(fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	ioctl(fd, VIDIOC_STREAMOFF, &type);
	close(fd);
	fclose(out);
	fclose(in);
	return got == sent ? 0 : 1;
}
