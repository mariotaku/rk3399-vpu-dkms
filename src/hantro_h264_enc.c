// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me>
/*
 * Hantro VPU codec driver: stateful H.264 encoder front end.
 *
 * Turns the stateful encoder interface (V4L2_PIX_FMT_H264 with the
 * V4L2_CID_MPEG_VIDEO_* controls) into per-frame encoder parameters:
 * frame type and GOP, SPS and PPS (written into the CAPTURE buffer before
 * every IDR frame), the reconstruction/reference buffer pair and the
 * frame-level rate control. The register programming is in
 * rockchip_vpu2_hw_h264_enc.c.
 *
 * The SPS/PPS writer and the rate control are taken from Paul
 * Kocialkowski's v4l2-h264-enc series (v4l2-h264-enc-rbsp.c and
 * v4l2-h264-enc-rc.c in this directory).
 *
 * Copyright (C) 2026 rk3399-vpu-dkms contributors
 */

#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_hw.h"

/* VEPU2 writes a 16-bit frame_num and has no POC LSB in its slice header. */
#define H264E_LOG2_MAX_FRAME_NUM_MINUS4	12
#define H264E_POC_TYPE			2

#define H264E_QP_DEFAULT_I		26
#define H264E_QP_DEFAULT_P		28

/* Table A-1 of the H.264 specification, levels 1 to 4.2 (VEPU2 limit). */
static const struct {
	u8 v4l2;
	u8 level_idc;
	u32 max_mbps;
	u32 max_fs;
	u32 max_br;	/* kbit/s, Baseline and Main */
} h264e_levels[] = {
	{ V4L2_MPEG_VIDEO_H264_LEVEL_1_0, 10, 1485, 99, 64 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_1_1, 11, 3000, 396, 192 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_1_2, 12, 6000, 396, 384 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_1_3, 13, 11880, 396, 768 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_2_0, 20, 11880, 396, 2000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_2_1, 21, 19800, 792, 4000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_2_2, 22, 20250, 1620, 4000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_3_0, 30, 40500, 1620, 10000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_3_1, 31, 108000, 3600, 14000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_3_2, 32, 216000, 5120, 20000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_4_0, 40, 245760, 8192, 20000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_4_1, 41, 245760, 8192, 50000 },
	{ V4L2_MPEG_VIDEO_H264_LEVEL_4_2, 42, 522240, 8704, 50000 },
};

static unsigned int h264e_mb_width(struct hantro_ctx *ctx)
{
	return MB_WIDTH(ctx->src_fmt.width);
}

static unsigned int h264e_mb_height(struct hantro_ctx *ctx)
{
	return MB_HEIGHT(ctx->src_fmt.height);
}

/*
 * The visible size: the CAPTURE format, which the OUTPUT crop selection
 * also sets, as long as it lies inside the last macroblock column and row
 * of the OUTPUT frame (the hardware crops at most 12 columns and 15 rows);
 * the whole OUTPUT frame otherwise.
 */
void hantro_h264_enc_visible(struct hantro_ctx *ctx, u32 *width, u32 *height)
{
	u32 w = ctx->dst_fmt.width, h = ctx->dst_fmt.height;
	u32 src_w = ctx->src_fmt.width, src_h = ctx->src_fmt.height;

	if (w > src_w || MB_WIDTH(w) != MB_WIDTH(src_w))
		w = src_w;
	if (h > src_h || MB_HEIGHT(h) != MB_HEIGHT(src_h))
		h = src_h;

	*width = w;
	*height = h;
}

size_t hantro_h264_enc_rec_luma_size(struct hantro_ctx *ctx)
{
	return h264e_mb_width(ctx) * MB_DIM * h264e_mb_height(ctx) * MB_DIM;
}

/*
 * The level the user asked for, raised to the lowest one that allows the
 * frame size, macroblock rate and bitrate of the stream.
 */
static u8 h264e_level_idc(struct hantro_ctx *ctx)
{
	struct hantro_h264_enc_cfg *cfg = &ctx->h264_enc_cfg;
	struct v4l2_fract *tpf = &cfg->timeperframe;
	u32 fs = h264e_mb_width(ctx) * h264e_mb_height(ctx);
	u64 mbps = div_u64((u64)fs * tpf->denominator, max(tpf->numerator, 1U));
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(h264e_levels); i++) {
		if (h264e_levels[i].v4l2 < cfg->level)
			continue;
		if (fs <= h264e_levels[i].max_fs &&
		    mbps <= h264e_levels[i].max_mbps &&
		    cfg->bitrate / 1000 <= h264e_levels[i].max_br)
			return h264e_levels[i].level_idc;
	}

	return h264e_levels[ARRAY_SIZE(h264e_levels) - 1].level_idc;
}

static void h264e_fill_vui(struct hantro_ctx *ctx,
			   struct v4l2_h264_sps_video *video)
{
	struct hantro_h264_enc_cfg *cfg = &ctx->h264_enc_cfg;
	struct v4l2_pix_format_mplane *fmt = &ctx->src_fmt;
	u8 primaries = 0, transfer = 0, matrix = 0;

	const struct v4l2_format_info *info = v4l2_format_info(fmt->pixelformat);

	video->flags |= V4L2_H264_SPS_VIDEO_FLAG_VUI_PARAMETERS_PRESENT;

	/*
	 * RGB input goes through the encoder's colour conversion, which
	 * produces full-range BT.709 Y'CbCr (rockchip_vpu2_hw_h264_enc.c), so
	 * the stream says exactly that; primaries and transfer function stay
	 * those of the RGB source (sRGB unless userspace says otherwise).
	 */
	if (info && info->pixel_enc == V4L2_PIXEL_ENC_RGB) {
		switch (fmt->colorspace) {
		case V4L2_COLORSPACE_SMPTE170M:
			primaries = V4L2_H264_VUI_COLOUR_SMPTE170M;
			transfer = V4L2_H264_VUI_TRANSFER_SMPTE170M;
			break;
		case V4L2_COLORSPACE_BT2020:
			primaries = V4L2_H264_VUI_COLOUR_BT2020;
			transfer = V4L2_H264_VUI_TRANSFER_BT709;
			break;
		case V4L2_COLORSPACE_REC709:
			primaries = V4L2_H264_VUI_COLOUR_BT709;
			transfer = V4L2_H264_VUI_TRANSFER_BT709;
			break;
		default:
			primaries = V4L2_H264_VUI_COLOUR_BT709;
			transfer = V4L2_H264_VUI_TRANSFER_SRGB;
			break;
		}
		video->flags |= V4L2_H264_SPS_VIDEO_FLAG_VUI_VIDEO_SIGNAL_TYPE_PRESENT |
				V4L2_H264_SPS_VIDEO_FLAG_VUI_COLOUR_DESCRIPTION_PRESENT |
				V4L2_H264_SPS_VIDEO_FLAG_VUI_VIDEO_FULL_RANGE;
		video->video_format = V4L2_H264_VUI_VIDEO_UNSPECIFIED;
		video->colour_primaries = primaries;
		video->transfer_characteristics = transfer;
		video->matrix_coefficients = V4L2_H264_VUI_MATRIX_BT709;
		goto timing;
	}

	/*
	 * Colorimetry only when userspace set one; the hantro default
	 * (V4L2_COLORSPACE_JPEG) is a leftover of the JPEG encoder.
	 */
	switch (fmt->colorspace) {
	case V4L2_COLORSPACE_REC709:
		primaries = V4L2_H264_VUI_COLOUR_BT709;
		transfer = V4L2_H264_VUI_TRANSFER_BT709;
		matrix = V4L2_H264_VUI_MATRIX_BT709;
		break;
	case V4L2_COLORSPACE_SMPTE170M:
		primaries = V4L2_H264_VUI_COLOUR_SMPTE170M;
		transfer = V4L2_H264_VUI_TRANSFER_SMPTE170M;
		matrix = V4L2_H264_VUI_MATRIX_SMPTE170M;
		break;
	case V4L2_COLORSPACE_470_SYSTEM_BG:
		primaries = V4L2_H264_VUI_COLOUR_BT470_SYSTEM_BG;
		transfer = V4L2_H264_VUI_TRANSFER_BT470_SYSTEM_BG;
		matrix = V4L2_H264_VUI_MATRIX_BT470_SYSTEM_BG;
		break;
	case V4L2_COLORSPACE_BT2020:
		primaries = V4L2_H264_VUI_COLOUR_BT2020;
		transfer = V4L2_H264_VUI_TRANSFER_BT709;
		matrix = V4L2_H264_VUI_MATRIX_BT2020;
		break;
	default:
		break;
	}

	if (primaries) {
		video->flags |= V4L2_H264_SPS_VIDEO_FLAG_VUI_VIDEO_SIGNAL_TYPE_PRESENT |
				V4L2_H264_SPS_VIDEO_FLAG_VUI_COLOUR_DESCRIPTION_PRESENT;
		if (fmt->quantization == V4L2_QUANTIZATION_FULL_RANGE)
			video->flags |= V4L2_H264_SPS_VIDEO_FLAG_VUI_VIDEO_FULL_RANGE;
		video->video_format = V4L2_H264_VUI_VIDEO_UNSPECIFIED;
		video->colour_primaries = primaries;
		video->transfer_characteristics = transfer;
		video->matrix_coefficients = matrix;
	}

timing:
	if (cfg->timeperframe.numerator && cfg->timeperframe.denominator) {
		video->flags |= V4L2_H264_SPS_VIDEO_FLAG_VUI_TIMING_INFO_PRESENT |
				V4L2_H264_SPS_VIDEO_FLAG_VUI_FIXED_FRAME_RATE;
		video->num_units_in_tick = cfg->timeperframe.numerator;
		video->time_scale = 2 * cfg->timeperframe.denominator;
	}

	/* No reordering: a decoder may output every frame immediately. */
	video->flags |= V4L2_H264_SPS_VIDEO_FLAG_VUI_BITSTREAM_RESTRICTION |
			V4L2_H264_SPS_VIDEO_FLAG_VUI_MOTION_VECTORS_OVER_PIC_BOUNDARIES;
	video->max_bytes_per_pic_denom = 0;
	video->max_bits_per_mb_denom = 0;
	video->log2_max_mv_length_horizontal = 15;
	video->log2_max_mv_length_vertical = 15;
	video->max_num_reorder_frames = 0;
	video->max_dec_frame_buffering = 1;
}

static void h264e_fill_headers(struct hantro_ctx *ctx)
{
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	struct v4l2_ctrl_h264_sps *sps = &h264->state.sps;
	struct v4l2_ctrl_h264_pps *pps = &h264->state.pps;
	struct v4l2_h264_sps_video *video = &h264->state.sps_video;
	unsigned int mb_w = h264e_mb_width(ctx), mb_h = h264e_mb_height(ctx);
	unsigned int crop_r, crop_b;
	u32 vis_w, vis_h;

	memset(sps, 0, sizeof(*sps));
	memset(pps, 0, sizeof(*pps));
	memset(video, 0, sizeof(*video));

	/* Constrained Baseline: no FMO, ASO or redundant slices are used. */
	sps->profile_idc = 66;
	sps->constraint_set_flags = V4L2_H264_SPS_CONSTRAINT_SET0_FLAG |
				    V4L2_H264_SPS_CONSTRAINT_SET1_FLAG;
	sps->level_idc = h264e_level_idc(ctx);
	sps->seq_parameter_set_id = 0;
	sps->chroma_format_idc = 1;
	sps->log2_max_frame_num_minus4 = H264E_LOG2_MAX_FRAME_NUM_MINUS4;
	sps->pic_order_cnt_type = H264E_POC_TYPE;
	sps->max_num_ref_frames = 1;
	sps->pic_width_in_mbs_minus1 = mb_w - 1;
	sps->pic_height_in_map_units_minus1 = mb_h - 1;
	sps->flags = V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY |
		     V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE;

	hantro_h264_enc_visible(ctx, &vis_w, &vis_h);
	crop_r = mb_w * MB_DIM - vis_w;
	crop_b = mb_h * MB_DIM - vis_h;
	if (crop_r || crop_b) {
		video->flags |= V4L2_H264_SPS_VIDEO_FLAG_FRAME_CROPPING;
		/* 4:2:0 frame coding: crop units are two luma samples. */
		video->frame_crop_right_offset = crop_r / 2;
		video->frame_crop_bottom_offset = crop_b / 2;
	}

	h264e_fill_vui(ctx, video);

	pps->pic_parameter_set_id = 0;
	pps->seq_parameter_set_id = 0;
	pps->num_ref_idx_l0_default_active_minus1 = 0;
	/* The hardware writes slice QP relative to its PPS_INIT_QP of 26. */
	pps->pic_init_qp_minus26 = 0;
	pps->pic_init_qs_minus26 = 0;
	pps->chroma_qp_index_offset = 0;
	/* The hardware slice header carries the deblocking fields. */
	pps->flags = V4L2_H264_PPS_FLAG_DEBLOCKING_FILTER_CONTROL_PRESENT;
}

static int h264e_write_headers(struct hantro_ctx *ctx, u8 *dst, size_t size)
{
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	struct v4l2_h264_enc_rbsp *rbsp = &h264->rbsp;
	int ret;

	v4l2_h264_enc_rbsp_init(rbsp, dst, size);

	ret = v4l2_h264_enc_rbsp_start_code(rbsp);
	if (!ret)
		ret = v4l2_h264_enc_rbsp_sps(rbsp, &h264->state.sps,
					     &h264->state.sps_video);
	if (!ret)
		ret = v4l2_h264_enc_rbsp_start_code(rbsp);
	if (!ret)
		ret = v4l2_h264_enc_rbsp_pps(rbsp, &h264->state.pps);
	if (ret)
		return ret;

	h264->hdr_len = v4l2_h264_enc_rbsp_bytes_count(rbsp);

	return 0;
}

/*
 * Constant bitrate.
 *
 * The driver replaces the CBR estimate of the shared rate control with a
 * frame-level leaky bucket: a virtual buffer of one second of the bitrate
 * that the frames fill and the bitrate drains, steered back to half full
 * over half a second. Each frame type has a complexity, its size scaled to
 * QP 0 assuming size halves every 6 QP; the QP for a frame is the one that
 * brings the larger of the last and the average complexity down to the
 * frame's target. Using the larger one keeps cheap frames (repeated or
 * static pictures) from pulling QP down ahead of a scene change; for the
 * same reason QP falls by at most 2 per frame and stays within 4 of the
 * running average of P-frame QPs. The buffer counts at most half a second
 * of unused bitrate as credit and one second of overshoot as debt, and no
 * frame gets less than a quarter of its share: after a scene change the
 * stream overshoots for a while rather than starving at QP 51.
 */
#define CBR_QP_STEP_NUM		1149	/* 2^(1/6) * 1024 */
#define CBR_QP_STEP_DEN		1024
#define CBR_IP_RATIO_DEFAULT	(6 * 16)

static int h264e_cbr_init(struct v4l2_h264_enc_rc *rc)
{
	struct hantro_ctx *ctx = rc->private_data;
	struct hantro_h264_enc_cbr *cbr = &ctx->h264_enc.cbr;
	u64 size = ctx->h264_enc_cfg.bitrate;

	memset(cbr, 0, sizeof(*cbr));
	cbr->level = size / 2;
	cbr->ip_ratio = CBR_IP_RATIO_DEFAULT;

	return 0;
}

static u64 h264e_cbr_scale(u64 bits, int qp)
{
	for (; qp > 0; qp--)
		bits = div_u64(bits * CBR_QP_STEP_NUM, CBR_QP_STEP_DEN);

	return bits;
}

/* The lowest QP at which a frame of complexity @cplx fits in @target. */
static int h264e_cbr_qp(u64 cplx, u64 target)
{
	int qp = 0;

	target = max(target, 1ULL);
	while (cplx > target && qp < 51) {
		cplx = div_u64(cplx * CBR_QP_STEP_DEN, CBR_QP_STEP_NUM);
		qp++;
	}

	return qp;
}

static int h264e_cbr_estimate(struct v4l2_h264_enc_rc *rc, unsigned int *qp)
{
	struct hantro_ctx *ctx = rc->private_data;
	struct hantro_h264_enc_cbr *cbr = &ctx->h264_enc.cbr;
	struct v4l2_h264_enc_state *state = rc->state;
	struct v4l2_fract *tpf = &state->timeperframe;
	unsigned int type, fps, period;
	u64 rate, size, base, target, cplx;
	s64 correction;
	int new_qp, last_qp;

	if (state->bitrate_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CBR ||
	    !state->frame_rc_enable)
		return -EOPNOTSUPP;

	type = state->encode.slice_type == V4L2_H264_SLICE_TYPE_I ? 0 : 1;
	fps = max(1U, DIV_ROUND_CLOSEST(tpf->denominator, tpf->numerator));
	rate = div_u64((u64)state->bitrate * tpf->numerator, tpf->denominator);
	size = state->bitrate;
	period = cbr->period ? cbr->period : 10 * fps;

	/* Split the frame budget of a period between its I and P frames. */
	base = div_u64(rate * period * 16, (period - 1) * 16 + cbr->ip_ratio);
	if (!type)
		base = div_u64(base * cbr->ip_ratio, 16);

	/* Steer the buffer back to half full over half a second. */
	correction = div_s64((s64)size / 2 - cbr->level, max(fps / 2, 1U));
	target = clamp_t(s64, (s64)base + correction, base / 4, base * 2);

	/*
	 * I frames are predicted from the current P frames, since the
	 * content may have changed since the last I frame.
	 */
	if (cbr->valid[1]) {
		cplx = max(cbr->cplx[1], cbr->cplx_avg[1]);
		last_qp = cbr->qp[1];
		if (!type) {
			cplx = div_u64(cplx * cbr->ip_ratio, 16);
			last_qp = cbr->valid[0] ? cbr->qp[0] : last_qp;
		}
	} else if (cbr->valid[0]) {
		cplx = max(cbr->cplx[0], cbr->cplx_avg[0]);
		if (type)
			cplx = div_u64(cplx * 16, cbr->ip_ratio);
		last_qp = cbr->qp[0];
	} else {
		/* First frame: the shared rate control's initial estimate. */
		return -EOPNOTSUPP;
	}

	new_qp = max(h264e_cbr_qp(cplx, target), last_qp - 2);
	if (cbr->qp_avg)
		new_qp = max(new_qp, (int)(cbr->qp_avg / 16) - 4);
	new_qp = clamp(new_qp, (int)state->qp_min, (int)state->qp_max);

	cbr->type = type;
	cbr->qp_cur = new_qp;
	*qp = new_qp;

	return 0;
}

static int h264e_cbr_complete(struct v4l2_h264_enc_rc *rc,
			      unsigned long bytesused)
{
	struct hantro_ctx *ctx = rc->private_data;
	struct hantro_h264_enc_cbr *cbr = &ctx->h264_enc.cbr;
	struct v4l2_h264_enc_state *state = rc->state;
	struct v4l2_fract *tpf = &state->timeperframe;
	unsigned int type = cbr->type;
	u64 bits = (u64)bytesused * 8;
	u64 rate, size, cplx;

	if (state->bitrate_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CBR ||
	    !state->frame_rc_enable)
		return -EOPNOTSUPP;

	/* The first frame used the shared estimate; take its QP from there. */
	if (!cbr->valid[0] && !cbr->valid[1]) {
		type = state->encode.slice_type == V4L2_H264_SLICE_TYPE_I ? 0 : 1;
		cbr->qp_cur = rc->qp;
	}

	rate = div_u64((u64)state->bitrate * tpf->numerator, tpf->denominator);
	size = state->bitrate;
	cbr->level = clamp_t(s64, cbr->level + (s64)bits - (s64)rate, 0,
			     (s64)size * 3 / 2);

	cplx = h264e_cbr_scale(max(bits, 1ULL), cbr->qp_cur);

	/* I to P size ratio at the same QP. */
	if (!type && cbr->valid[1])
		cbr->ip_ratio = clamp_t(u64, div64_u64(cplx * 16, max(cbr->cplx[1], 1ULL)),
					2 * 16, 20 * 16);

	/* Running average of P-frame QPs, x16, over about half a second. */
	if (type)
		cbr->qp_avg = cbr->qp_avg ?
			      (15 * cbr->qp_avg + 16 * cbr->qp_cur) / 16 :
			      16 * cbr->qp_cur;

	cbr->cplx_avg[type] = cbr->valid[type] ?
			      (3 * cbr->cplx_avg[type] + cplx) / 4 : cplx;
	cbr->cplx[type] = cplx;
	cbr->qp[type] = cbr->qp_cur;
	cbr->valid[type] = true;

	return 0;
}

static const struct v4l2_h264_enc_rc_ops h264e_rc_ops = {
	.init = h264e_cbr_init,
	.estimate = h264e_cbr_estimate,
	.complete = h264e_cbr_complete,
};

int hantro_h264_enc_init(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	size_t size = hantro_h264_enc_rec_luma_size(ctx) * 3 / 2;
	unsigned int i;

	memset(h264, 0, sizeof(*h264));

	for (i = 0; i < HANTRO_H264_ENC_REC_COUNT; i++) {
		struct hantro_aux_buf *rec = &h264->rec[i];

		rec->attrs = DMA_ATTR_NO_KERNEL_MAPPING;
		rec->size = size;
		rec->cpu = dma_alloc_attrs(vpu->dev, size, &rec->dma,
					   GFP_KERNEL, rec->attrs);
		if (!rec->cpu) {
			hantro_h264_enc_exit(ctx);
			return -ENOMEM;
		}
	}

	v4l2_h264_enc_rc_init(&h264->rc);
	h264->rc.ops = &h264e_rc_ops;
	h264->rc.private_data = ctx;

	return 0;
}

void hantro_h264_enc_exit(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	unsigned int i;

	v4l2_h264_enc_rc_exit(&h264->rc);

	for (i = 0; i < HANTRO_H264_ENC_REC_COUNT; i++) {
		struct hantro_aux_buf *rec = &h264->rec[i];

		if (rec->cpu)
			dma_free_attrs(vpu->dev, rec->size, rec->cpu,
				       rec->dma, rec->attrs);
		rec->cpu = NULL;
	}
}

/*
 * Decide the frame type, write SPS and PPS before an IDR frame and pick the
 * QP. Called by the backend's run() with the CPU address of the CAPTURE
 * buffer. On return h264->hdr_len bytes of @dst are used and the backend
 * continues the stream after them.
 */
int hantro_h264_enc_prepare(struct hantro_ctx *ctx, u8 *dst, size_t dst_size)
{
	struct hantro_h264_enc_cfg *cfg = &ctx->h264_enc_cfg;
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	struct v4l2_h264_enc_state *state = &h264->state;
	struct v4l2_ctrl_h264_encode_params *encode = &state->encode;
	u32 period = cfg->i_period ? cfg->i_period : cfg->gop_size;
	bool idr;
	int ret;

	/*
	 * An IDR frame starts the stream, follows a failed frame (its
	 * reconstruction is not a valid reference) and is placed every
	 * GOP; userspace can force one.
	 */
	idr = !h264->headers_sent || h264->frame_pending || cfg->force_idr ||
	      (period && h264->gop_count >= period);
	cfg->force_idr = false;

	state->timeperframe = cfg->timeperframe;
	state->width_mbs = h264e_mb_width(ctx);
	state->height_mbs = h264e_mb_height(ctx);
	/* The rate control picks its mode again when it is switched. */
	if (h264->rc.mode && state->frame_rc_enable != cfg->rc_enable)
		v4l2_h264_enc_rc_mode_update(&h264->rc);
	state->frame_rc_enable = cfg->rc_enable;
	/* The rate control picks its mode again when the mode changes. */
	if (h264->rc.mode && state->bitrate_mode != cfg->bitrate_mode)
		v4l2_h264_enc_rc_mode_update(&h264->rc);
	state->bitrate_mode = cfg->bitrate_mode;
	state->quality = cfg->quality;
	state->quality_min = 1;
	state->quality_max = 100;
	state->bitrate = max(cfg->bitrate, 1U);
	state->qp_min = min(cfg->qp_min, cfg->qp_max);
	state->qp_max = cfg->qp_max;
	state->qp_i = cfg->qp_i;
	state->qp_p = cfg->qp_p;
	state->qp_b = cfg->qp_p;

	memset(encode, 0, sizeof(*encode));
	encode->nal_ref_idc = 1;
	encode->pic_parameter_set_id = 0;

	h264->hdr_len = 0;

	if (idr) {
		h264->frame_num = 0;
		h264->gop_count = 0;
		encode->slice_type = V4L2_H264_SLICE_TYPE_I;
		encode->flags = V4L2_H264_ENCODE_FLAG_IDR_PIC;
		encode->idr_pic_id = h264->idr_pic_id;

		h264e_fill_headers(ctx);
		ret = h264e_write_headers(ctx, dst, dst_size);
		if (ret)
			return ret;
	} else {
		encode->slice_type = V4L2_H264_SLICE_TYPE_P;
	}
	encode->frame_num = h264->frame_num;

	h264->cbr.period = period;
	ret = v4l2_h264_enc_rc_step(&h264->rc, state);
	if (ret)
		return ret;

	h264->frame_idr = idr;
	h264->frame_pending = true;

	return 0;
}

/* Called by the backend's done() with the size of the encoded frame. */
void hantro_h264_enc_complete(struct hantro_ctx *ctx, size_t bytesused)
{
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;

	if (h264->rc.mode)
		v4l2_h264_enc_rc_complete(&h264->rc, bytesused);

	if (h264->frame_idr) {
		h264->headers_sent = true;
		h264->idr_pic_id = (h264->idr_pic_id + 1) & 0xf;
	}

	h264->frame_num = (h264->frame_num + 1) &
			  ((1 << (H264E_LOG2_MAX_FRAME_NUM_MINUS4 + 4)) - 1);
	h264->gop_count++;
	h264->rec_cur ^= 1;
	h264->frame_pending = false;
}

/* Controls */

static int hantro_h264_enc_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct hantro_ctx *ctx = container_of(ctrl->handler, struct hantro_ctx,
					      ctrl_handler);
	struct hantro_h264_enc_cfg *cfg = &ctx->h264_enc_cfg;

	switch (ctrl->id) {
	case V4L2_CID_MPEG_VIDEO_BITRATE:
		cfg->bitrate = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE:
		cfg->rc_enable = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_GOP_SIZE:
		cfg->gop_size = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_PERIOD:
		cfg->i_period = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_HEADER_MODE:
		cfg->header_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME:
		cfg->force_idr = true;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_PROFILE:
		cfg->profile = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_LEVEL:
		cfg->level = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MIN_QP:
		cfg->qp_min = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
		cfg->qp_max = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP:
		cfg->qp_i = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP:
		cfg->qp_p = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_BITRATE_MODE:
		cfg->bitrate_mode = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY:
		cfg->quality = ctrl->val;
		break;
	case V4L2_CID_MPEG_VIDEO_B_FRAMES:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops hantro_h264_enc_ctrl_ops = {
	.s_ctrl = hantro_h264_enc_s_ctrl,
};

int hantro_h264_enc_ctrls_setup(struct hantro_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdl = &ctx->ctrl_handler;
	const struct v4l2_ctrl_ops *ops = &hantro_h264_enc_ctrl_ops;

	ctx->h264_enc_cfg.timeperframe.numerator = 1;
	ctx->h264_enc_cfg.timeperframe.denominator = 30;

	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE,
			  10000, 50000000, 1, 2000000);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_BITRATE_MODE,
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CQ,
			       BIT(V4L2_MPEG_VIDEO_BITRATE_MODE_VBR),
			       V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY,
			  1, 100, 1, 50);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
			  0, 1, 1, 1);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_GOP_SIZE,
			  0, S32_MAX, 1, 30);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD,
			  0, S32_MAX, 1, 0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, 0, 1, 0);
	/* One source frame at a time is enough to keep the encoder busy. */
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 1, 32, 1, 1);
	/* SPS and PPS always come with the IDR frame they apply to. */
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
			       BIT(V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE),
			       V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME,
			  0, 0, 0, 0);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
			       V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
			       0,
			       V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE);
	v4l2_ctrl_new_std_menu(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_LEVEL,
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_2,
			       BIT(V4L2_MPEG_VIDEO_H264_LEVEL_1B),
			       V4L2_MPEG_VIDEO_H264_LEVEL_4_0);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_MIN_QP,
			  0, 51, 1, 10);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
			  0, 51, 1, 51);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP,
			  0, 51, 1, H264E_QP_DEFAULT_I);
	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
			  0, 51, 1, H264E_QP_DEFAULT_P);

	return hdl->error;
}
