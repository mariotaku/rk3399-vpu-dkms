// SPDX-License-Identifier: GPL-2.0
/*
 * Drives neatvnc 0.9.1's V4L2 M2M H.264 backend (src/enc/h264/v4l2m2m-impl.c,
 * compiled in unchanged apart from v4l2m2m-fixes.patch) with GBM buffer
 * objects, the way neatvnc's Open H.264 encoder does, and writes the
 * resulting Annex-B stream to a file.
 *
 * usage: neatvnc-harness out.h264 [frames] [width height] [quality]
 * quality is neatvnc's internal value (1 best .. 51 worst, default 18,
 * which is what client quality 6 gives).
 *
 * The frames are SMPTE-like colour bars with a moving white square, so
 * the decoded colours can be checked against known RGB values.
 *
 * Copyright (C) 2026 rk3399-vpu-dkms contributors
 */

#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include "enc/h264-encoder.h"
#include "fb.h"
#include "aml.h"

extern struct h264_encoder_impl h264_encoder_v4l2m2m_impl;

/* aml stand-in */
static struct aml_handler *the_handler;

struct aml_handler *aml_handler_new(int fd, aml_callback_fn cb, void *userdata,
				    void *free_fn)
{
	struct aml_handler *h = calloc(1, sizeof(*h));

	(void)free_fn;
	h->fd = fd;
	h->cb = cb;
	h->userdata = userdata;
	the_handler = h;
	return h;
}

void *aml_get_userdata(const void *obj)
{
	return ((const struct aml_handler *)obj)->userdata;
}

void aml_set_event_mask(struct aml_handler *h, int mask) { (void)h; (void)mask; }
struct aml *aml_get_default(void) { return NULL; }
int aml_start(struct aml *aml, void *obj) { (void)aml; (void)obj; return 0; }
int aml_stop(struct aml *aml, void *obj) { (void)aml; (void)obj; return 0; }
void aml_unref(void *obj) { if (obj == the_handler) the_handler = NULL; free(obj); }

/* nvnc_fb stand-in */
void nvnc_fb_ref(struct nvnc_fb *fb) { fb->ref++; }
void nvnc_fb_unref(struct nvnc_fb *fb) { fb->ref--; }
void nvnc_fb_hold(struct nvnc_fb *fb) { fb->hold++; }
void nvnc_fb_release(struct nvnc_fb *fb) { fb->hold--; }
int nvnc_fb_map(struct nvnc_fb *fb) { (void)fb; return 0; }
void nvnc_fb_unmap(struct nvnc_fb *fb) { (void)fb; }
struct gbm_bo *nvnc_fb_get_gbm_bo(const struct nvnc_fb *fb) { return fb->bo; }

static FILE *out;
static unsigned int packets;
static size_t total;

static void on_packet(const void *payload, size_t size, uint64_t pts,
		      void *userdata)
{
	(void)userdata;
	fwrite(payload, 1, size, out);
	fprintf(stderr, "packet %u: %zu bytes, pts %llu\n", packets, size,
		(unsigned long long)pts);
	packets++;
	total += size;
}

/* 75% bars: white, yellow, cyan, green, magenta, red, blue (R, G, B). */
static const uint8_t bars[7][3] = {
	{ 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
	{ 191, 0, 191 }, { 191, 0, 0 }, { 0, 0, 191 },
};

/* DRM_FORMAT_XBGR8888: bytes R, G, B, X. */
static void draw(struct gbm_bo *bo, unsigned int w, unsigned int h,
		 unsigned int frame)
{
	uint32_t stride;
	void *map_data = NULL;
	uint8_t *p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride,
				&map_data);
	unsigned int x, y, sq = h / 8, sx = (frame * 8) % (w - sq), sy = h / 2;

	if (!p) {
		fprintf(stderr, "gbm_bo_map failed\n");
		exit(1);
	}
	for (y = 0; y < h; y++) {
		uint8_t *row = p + (size_t)y * stride;

		for (x = 0; x < w; x++) {
			const uint8_t *c = bars[x * 7 / w];
			bool white = x >= sx && x < sx + sq && y >= sy && y < sy + sq;

			row[4 * x + 0] = white ? 255 : c[0];
			row[4 * x + 1] = white ? 255 : c[1];
			row[4 * x + 2] = white ? 255 : c[2];
			row[4 * x + 3] = 0xff;
		}
	}
	gbm_bo_unmap(bo, map_data);
}

int main(int argc, char **argv)
{
	unsigned int frames = argc > 2 ? atoi(argv[2]) : 60;
	unsigned int w = argc > 4 ? atoi(argv[3]) : 1920;
	unsigned int h = argc > 4 ? atoi(argv[4]) : 1080;
	int quality = argc > 5 ? atoi(argv[5]) : 18;
	struct nvnc_fb fbs[6] = { 0 };
	struct gbm_device *gbm;
	struct h264_encoder *enc;
	unsigned int i, f;
	int drm;

	if (argc < 2) {
		fprintf(stderr, "usage: %s out.h264 [frames] [width height] [quality]\n",
			argv[0]);
		return 2;
	}
	out = fopen(argv[1], "wb");
	drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (!out || drm < 0) {
		perror("open");
		return 1;
	}
	gbm = gbm_create_device(drm);

	/*
	 * neatvnc queues stride * ALIGN_UP(height, 16) bytes per plane, so
	 * the buffer object must cover the macroblock-aligned height.
	 */
	for (i = 0; i < 6; i++) {
		fbs[i].bo = gbm_bo_create(gbm, w, (h + 15) & ~15U,
					  GBM_FORMAT_XBGR8888,
					  GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
		if (!fbs[i].bo) {
			fprintf(stderr, "gbm_bo_create failed\n");
			return 1;
		}
	}

	enc = h264_encoder_v4l2m2m_impl.create(w, h, DRM_FORMAT_XBGR8888, quality);
	if (!enc) {
		fprintf(stderr, "neatvnc's v4l2m2m backend found no capable device\n");
		return 1;
	}
	enc->on_packet_ready = on_packet;
	enc->next_frame_should_be_keyframe = true;

	for (f = 0; f < frames; f++) {
		struct nvnc_fb *fb = &fbs[f % 6];

		/*
		 * Keep at most two frames in flight: neatvnc has three source
		 * buffers and releases finished ones at the start of feed().
		 */
		while (packets + 2 < f) {
			struct pollfd pfd = { .fd = the_handler->fd, .events = POLLIN };

			if (poll(&pfd, 1, 3000) <= 0) {
				fprintf(stderr, "timeout\n");
				return 1;
			}
			the_handler->cb(the_handler);
		}
		if (fb->hold) {
			fprintf(stderr, "buffer object %u still held\n", f % 6);
			return 1;
		}
		draw(fb->bo, w, h, f);
		fb->pts = (uint64_t)f * 1000000 / 30;
		if (f == frames / 2)
			enc->next_frame_should_be_keyframe = true;
		h264_encoder_v4l2m2m_impl.feed(enc, fb);
	}

	/* Collect the remaining packets. */
	while (packets < frames) {
		struct pollfd pfd = { .fd = the_handler->fd, .events = POLLIN };

		if (poll(&pfd, 1, 3000) <= 0) {
			fprintf(stderr, "timeout waiting for packets\n");
			break;
		}
		the_handler->cb(the_handler);
	}

	h264_encoder_v4l2m2m_impl.destroy(enc);
	fprintf(stderr, "%u frames fed, %u packets, %zu bytes\n", frames,
		packets, total);
	for (i = 0; i < 6; i++)
		gbm_bo_destroy(fbs[i].bo);
	gbm_device_destroy(gbm);
	close(drm);
	fclose(out);
	return packets == frames ? 0 : 1;
}
