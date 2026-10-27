/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Encoder state used by the rate control, reduced from the
 * include/media/v4l2-h264-enc.h of Paul Kocialkowski's 2026 series
 * "[PATCH 00/14] media: Add V4L2 H.264 stateless encode and VC8000E support".
 * Only the fields the RBSP writer and the rate control read are kept; the
 * series' stateless encoder core itself is not part of this module.
 *
 * Copyright (C) 2025-2026 Paul Kocialkowski <paulk@sys-base.io>
 */

#ifndef _HANTRO_V4L2_H264_ENC_H
#define _HANTRO_V4L2_H264_ENC_H

#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include "v4l2-h264-enc-defs.h"
#include "v4l2-h264-enc-rbsp.h"
#include "v4l2-h264-enc-rc.h"

struct v4l2_h264_enc_state {
	struct v4l2_ctrl_h264_sps sps;
	struct v4l2_h264_sps_video sps_video;
	struct v4l2_ctrl_h264_pps pps;
	struct v4l2_ctrl_h264_encode_params encode;

	struct v4l2_fract timeperframe;

	unsigned int width_mbs;
	unsigned int height_mbs;

	bool frame_rc_enable;

	unsigned int qp_min;
	unsigned int qp_max;
	unsigned int qp_i;
	unsigned int qp_p;
	unsigned int qp_b;

	int bitrate_mode;
	unsigned int quality;
	unsigned int quality_min;
	unsigned int quality_max;
	unsigned int bitrate;
	unsigned int bitrate_peak;
};

#endif
