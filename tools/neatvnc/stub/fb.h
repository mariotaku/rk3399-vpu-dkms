/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me> */
#pragma once
#include <stdint.h>
struct gbm_bo;
struct nvnc_fb {
	uint64_t pts;
	struct gbm_bo *bo;
	int ref;
	int hold;
};
void nvnc_fb_ref(struct nvnc_fb *fb);
void nvnc_fb_unref(struct nvnc_fb *fb);
void nvnc_fb_hold(struct nvnc_fb *fb);
void nvnc_fb_release(struct nvnc_fb *fb);
int nvnc_fb_map(struct nvnc_fb *fb);
void nvnc_fb_unmap(struct nvnc_fb *fb);
struct gbm_bo *nvnc_fb_get_gbm_bo(const struct nvnc_fb *fb);
