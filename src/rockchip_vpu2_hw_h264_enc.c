// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Mariotaku <git@mariotaku.me>
/*
 * Hantro VPU codec driver: H.264 encoding on Rockchip VEPU2 (RK3399).
 *
 * Register programming ported from Bootlin's rk3399_vpu_hw_h264_enc.c
 * (Paul Kocialkowski, github.com/bootlin/linux branch
 * hantro/h264-encoding-v5.11), reworked for a stateful encoder: the frame
 * parameters, QP and reference come from hantro_h264_enc.c instead of
 * stateless controls, and the stream continues bit-exactly after the SPS
 * and PPS the driver wrote into the CAPTURE buffer.
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2021 Bootlin
 * Copyright (C) 2026 rk3399-vpu-dkms contributors
 */

#include <linux/unaligned.h>
#include <media/v4l2-mem2mem.h>

#include "hantro.h"
#include "hantro_hw.h"
#include "hantro_v4l2.h"
#include "rockchip_vpu2_regs.h"

/* Motion estimation and mode decision tables, indexed by QP (Bootlin). */
static const u32 h264_pred_mode_favor[52] = {
	7, 7, 8, 8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 16, 17, 18,
	19, 20, 21, 22, 24, 25, 27, 29, 30, 32, 34, 36, 38, 41, 43, 46,
	49, 51, 55, 58, 61, 65, 69, 73, 78, 82, 87, 93, 98, 104, 110,
	117, 124, 132, 140
};

/* sqrt(2^((qp-12)/3))*8 */
static const u32 h264_diff_mv_penalty[52] = {
	2, 2, 3, 3, 3, 4, 4, 4, 5, 6,
	6, 7, 8, 9, 10, 11, 13, 14, 16, 18,
	20, 23, 26, 29, 32, 36, 40, 45, 51, 57,
	64, 72, 81, 91, 102, 114, 128, 144, 161, 181,
	203, 228, 256, 287, 323, 362, 406, 456, 512, 575,
	645, 724
};

/* 31*sqrt(2^((qp-12)/3))/4 */
static const u32 h264_diff_mv_penalty4p[52] = {
	2, 2, 2, 3, 3, 3, 4, 4, 5, 5,
	6, 7, 8, 9, 10, 11, 12, 14, 16, 17,
	20, 22, 25, 28, 31, 35, 39, 44, 49, 55,
	62, 70, 78, 88, 98, 110, 124, 139, 156, 175,
	197, 221, 248, 278, 312, 351, 394, 442, 496, 557,
	625, 701
};

static const u32 h264_intra16_favor[52] = {
	24, 24, 24, 26, 27, 30, 32, 35, 39, 43, 48, 53, 58, 64, 71, 78,
	85, 93, 102, 111, 121, 131, 142, 154, 167, 180, 195, 211, 229,
	248, 271, 296, 326, 361, 404, 457, 523, 607, 714, 852, 1034,
	1272, 1588, 2008, 2568, 3318, 4323, 5672, 7486, 9928, 13216,
	17648
};

static const u32 h264_inter_favor[52] = {
	40, 40, 41, 42, 43, 44, 45, 48, 51, 53, 55, 60, 62, 67, 69, 72,
	78, 84, 90, 96, 110, 120, 135, 152, 170, 189, 210, 235, 265,
	297, 335, 376, 420, 470, 522, 572, 620, 670, 724, 770, 820,
	867, 915, 970, 1020, 1076, 1132, 1180, 1230, 1275, 1320, 1370
};

static const u32 h264_skip_sad_penalty[52] = {
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 233, 205, 182, 163,
	146, 132, 120, 109, 100,  92,  84,  78,  71,  66,  61,  56,  52,  48,
	44,  41,  38,  35,  32,  30,  27,  25,  23,  21,  19,  17,  15,  14,
	12,  11,   9,   8,   7,   5,   4,   3,   2,   1
};

static s32 exp_golomb_signed(s32 value)
{
	s32 tmp = 0;

	if (value > 0)
		value = 2 * value;
	else
		value = -2 * value + 1;

	while (value >> ++tmp)
		;

	return tmp * 2 - 1;
}

/*
 * 32-bit RGB input. With only the 32-bit input swap, the hardware sees each
 * pixel as the little-endian 32-bit word of its four bytes; the mask fields
 * give the bit position of the most significant bit of each component in
 * that word.
 */
static const struct {
	u32 fourcc;
	u8 r_msb, g_msb, b_msb;
} vepu2_rgb_fmts[] = {
	/* bytes B, G, R, X */
	{ V4L2_PIX_FMT_XBGR32, 23, 15, 7 },
	{ V4L2_PIX_FMT_ABGR32, 23, 15, 7 },
	/* bytes R, G, B, X */
	{ V4L2_PIX_FMT_RGBX32, 7, 15, 23 },
	{ V4L2_PIX_FMT_RGBA32, 7, 15, 23 },
	/* bytes X, R, G, B */
	{ V4L2_PIX_FMT_XRGB32, 15, 23, 31 },
	{ V4L2_PIX_FMT_ARGB32, 15, 23, 31 },
	/* bytes X, B, G, R */
	{ V4L2_PIX_FMT_BGRX32, 31, 23, 15 },
	{ V4L2_PIX_FMT_BGRA32, 31, 23, 15 },
};

/*
 * RGB to YCbCr, BT.709, full range (the conversion has no offset term):
 * Y = a R + b G + c B, Cb = e (B - Y), Cr = f (R - Y), in 1/65536 units,
 * with a = Kr, b = 1 - Kr - Kb, c = Kb, e = 0.5 / (1 - Kb),
 * f = 0.5 / (1 - Kr), Kr = 0.2126, Kb = 0.0722. The SPS VUI says the same.
 */
#define VEPU2_CSC_A	13933
#define VEPU2_CSC_B	46871
#define VEPU2_CSC_C	4732
#define VEPU2_CSC_E	35317
#define VEPU2_CSC_F	41615

static int vepu2_rgb_index(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vepu2_rgb_fmts); i++)
		if (vepu2_rgb_fmts[i].fourcc == fourcc)
			return i;
	return -1;
}

static void vepu2_h264_set_input(struct hantro_ctx *ctx,
				 struct vb2_buffer *src_buf)
{
	struct hantro_dev *vpu = ctx->dev;
	struct v4l2_pix_format_mplane *fmt = &ctx->src_fmt;
	const struct v4l2_format_info *info;
	dma_addr_t addr[3] = { 0 };
	unsigned int i, row_len;
	int rgb = vepu2_rgb_index(fmt->pixelformat);
	u32 reg;

	info = v4l2_format_info(fmt->pixelformat);

	for (i = 0; i < fmt->num_planes; i++)
		addr[i] = vb2_dma_contig_plane_dma_addr(src_buf, i) +
			  src_buf->planes[i].data_offset;

	/* Single-buffer formats: the planes follow each other. */
	if (fmt->num_planes == 1 && info && info->comp_planes > 1) {
		u32 luma = fmt->plane_fmt[0].bytesperline * fmt->height;

		addr[1] = addr[0] + luma;
		if (info->comp_planes > 2)
			addr[2] = addr[1] + luma / 4;
	}

	vepu_write_relaxed(vpu, addr[0], VEPU_REG_ADDR_IN_PLANE_0);
	vepu_write_relaxed(vpu, addr[1], VEPU_REG_ADDR_IN_PLANE_1);
	vepu_write_relaxed(vpu, addr[2], VEPU_REG_ADDR_IN_PLANE_2);

	/* Row length is in pixels. */
	row_len = fmt->plane_fmt[0].bytesperline;
	if (info && info->bpp[0])
		row_len /= info->bpp[0];
	reg = VEPU_REG_IN_IMG_CHROMA_OFFSET(0) |
	      VEPU_REG_IN_IMG_LUMA_OFFSET(0) |
	      VEPU_REG_IN_IMG_CTRL_ROW_LEN(row_len);
	vepu_write_relaxed(vpu, reg, VEPU_REG_INPUT_LUMA_INFO);

	if (rgb >= 0) {
		reg = VEPU_REG_RGB2YUV_CONVERSION_COEFB(VEPU2_CSC_B) |
		      VEPU_REG_RGB2YUV_CONVERSION_COEFA(VEPU2_CSC_A);
		vepu_write_relaxed(vpu, reg, VEPU_REG_RGB2YUV_CONVERSION_COEF1);
		reg = VEPU_REG_RGB2YUV_CONVERSION_COEFE(VEPU2_CSC_E) |
		      VEPU_REG_RGB2YUV_CONVERSION_COEFC(VEPU2_CSC_C);
		vepu_write_relaxed(vpu, reg, VEPU_REG_RGB2YUV_CONVERSION_COEF2);
		reg = VEPU_REG_RGB2YUV_CONVERSION_COEFF(VEPU2_CSC_F);
		vepu_write_relaxed(vpu, reg, VEPU_REG_RGB2YUV_CONVERSION_COEF3);
		reg = VEPU_REG_RGB_MASK_B_MSB(vepu2_rgb_fmts[rgb].b_msb) |
		      VEPU_REG_RGB_MASK_G_MSB(vepu2_rgb_fmts[rgb].g_msb) |
		      VEPU_REG_RGB_MASK_R_MSB(vepu2_rgb_fmts[rgb].r_msb);
		vepu_write_relaxed(vpu, reg, VEPU_REG_RGB_MASK_MSB);
	} else {
		vepu_write_relaxed(vpu, 0, VEPU_REG_RGB2YUV_CONVERSION_COEF1);
		vepu_write_relaxed(vpu, 0, VEPU_REG_RGB2YUV_CONVERSION_COEF2);
		vepu_write_relaxed(vpu, 0, VEPU_REG_RGB2YUV_CONVERSION_COEF3);
		vepu_write_relaxed(vpu, 0, VEPU_REG_RGB_MASK_MSB);
	}
}

/*
 * The hardware continues the stream at an 8-byte aligned address, after
 * a bit offset, and gets the 8 bytes already written at that address to
 * merge its first bits with (as Rockchip's MPP does it).
 */
static void vepu2_h264_set_output(struct hantro_ctx *ctx,
				  struct vb2_buffer *dst_buf, u8 *dst)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	size_t size = vb2_plane_size(dst_buf, 0);
	u32 hdr = h264->hdr_len;
	u32 base = hdr & ~7U;
	u32 rem_msb = 0, rem_lsb = 0;

	if (hdr & 7) {
		memset(dst + hdr, 0, 8 - (hdr & 7));
		rem_msb = get_unaligned_be32(dst + base);
		rem_lsb = get_unaligned_be32(dst + base + 4);
	}
	h264->strm_base = base;

	vepu_write_relaxed(vpu, vb2_dma_contig_plane_dma_addr(dst_buf, 0) + base,
			   VEPU_REG_ADDR_OUTPUT_STREAM);
	vepu_write_relaxed(vpu, rem_msb, VEPU_REG_STR_HDR_REM_MSB);
	vepu_write_relaxed(vpu, rem_lsb, VEPU_REG_STR_HDR_REM_LSB);
	/* The limit is written in 8-byte units and read back in bits. */
	vepu_write_relaxed(vpu, ((size - base) >> 3) & ~7U,
			   VEPU_REG_STR_BUF_LIMIT);
	vepu_write_relaxed(vpu, 0, VEPU_REG_ADDR_OUTPUT_CTRL);
}

int rockchip_vpu2_h264_enc_run(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	struct v4l2_ctrl_h264_encode_params *encode = &h264->state.encode;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	unsigned int mb_w = MB_WIDTH(ctx->src_fmt.width);
	unsigned int mb_h = MB_HEIGHT(ctx->src_fmt.height);
	unsigned int overfill_r, overfill_b, qp, i;
	u32 vis_w, vis_h;
	dma_addr_t rec, ref;
	u32 scaler, skip_penalty, reg;
	u8 dmv_penalty[128], dmv_qpel_penalty[128];
	bool intra;
	u8 *dst;
	int ret;

	src_buf = hantro_get_src_buf(ctx);
	dst_buf = hantro_get_dst_buf(ctx);

	hantro_start_prepare_run(ctx);

	dst = vb2_plane_vaddr(&dst_buf->vb2_buf, 0);
	if (!dst)
		return -ENOMEM;

	ret = hantro_h264_enc_prepare(ctx, dst, vb2_plane_size(&dst_buf->vb2_buf, 0));
	if (ret)
		return ret;

	intra = encode->slice_type == V4L2_H264_SLICE_TYPE_I;
	qp = min(h264->rc.qp, 51U);

	/* Select the H.264 encoder before writing the other registers. */
	vepu_write_relaxed(vpu, VEPU_REG_ENCODE_FORMAT_H264, VEPU_REG_ENCODE_START);

	reg = VEPU_REG_AXI_CTRL_READ_ID(0) |
	      VEPU_REG_AXI_CTRL_WRITE_ID(0) |
	      VEPU_REG_AXI_CTRL_BURST_LEN(16) |
	      VEPU_REG_AXI_CTRL_INCREMENT_MODE(0) |
	      VEPU_REG_AXI_CTRL_BIRST_DISCARD(0);
	vepu_write_relaxed(vpu, reg, VEPU_REG_AXI_CTRL);

	if (vepu2_rgb_index(ctx->src_fmt.pixelformat) >= 0)
		reg = VEPU_REG_INPUT_SWAP32;
	else
		reg = VEPU_REG_INPUT_SWAP8 | VEPU_REG_INPUT_SWAP16 |
		      VEPU_REG_INPUT_SWAP32;
	reg |= VEPU_REG_OUTPUT_SWAP32 | VEPU_REG_OUTPUT_SWAP16 |
	       VEPU_REG_OUTPUT_SWAP8;
	vepu_write_relaxed(vpu, reg, VEPU_REG_DATA_ENDIAN);

	vepu2_h264_set_input(ctx, &src_buf->vb2_buf);
	vepu2_h264_set_output(ctx, &dst_buf->vb2_buf, dst);

	/* Reconstruction of this frame, and the previous one as reference. */
	rec = h264->rec[h264->rec_cur].dma;
	ref = h264->rec[h264->rec_cur ^ 1].dma;
	vepu_write_relaxed(vpu, rec, VEPU_REG_ADDR_REC_LUMA);
	vepu_write_relaxed(vpu, rec + hantro_h264_enc_rec_luma_size(ctx),
			   VEPU_REG_ADDR_REC_CHROMA);
	vepu_write_relaxed(vpu, ref, VEPU_REG_ADDR_REF_LUMA);
	vepu_write_relaxed(vpu, ref + hantro_h264_enc_rec_luma_size(ctx),
			   VEPU_REG_ADDR_REF_CHROMA);

	/* No forced intra area, no cyclic intra refresh, no ROI. */
	reg = VEPU_REG_INTRA_AREA_TOP(mb_h) | VEPU_REG_INTRA_AREA_BOTTOM(mb_h) |
	      VEPU_REG_INTRA_AREA_LEFT(mb_w) | VEPU_REG_INTRA_AREA_RIGHT(mb_w);
	vepu_write_relaxed(vpu, reg, VEPU_REG_INTRA_AREA_CTRL);
	vepu_write_relaxed(vpu, 0, VEPU_REG_CIR_INTRA_CTRL);
	vepu_write_relaxed(vpu, 0, VEPU_REG_INTRA_SLICE_BITMAP(0));
	vepu_write_relaxed(vpu, 0, VEPU_REG_INTRA_SLICE_BITMAP(1));
	reg = VEPU_REG_ROI1_TOP_MB(mb_h) | VEPU_REG_ROI1_BOTTOM_MB(mb_h) |
	      VEPU_REG_ROI1_LEFT_MB(mb_w) | VEPU_REG_ROI1_RIGHT_MB(mb_w);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ROI1);
	reg = VEPU_REG_ROI2_TOP_MB(mb_h) | VEPU_REG_ROI2_BOTTOM_MB(mb_h) |
	      VEPU_REG_ROI2_LEFT_MB(mb_w) | VEPU_REG_ROI2_RIGHT_MB(mb_w);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ROI2);

	/*
	 * One slice per frame, CAVLC, no 8x8 transform; stream mode stays off
	 * (the hardware writes start code, NAL header and slice header).
	 */
	reg = VEPU_REG_H264_INTER4X4_MODE | VEPU_REG_H264_SLICE_SIZE(0) |
	      VEPU_REG_DEBLOCKING_FILTER_MODE(0);
	if (mb_w * mb_h > 3600)
		reg |= VEPU_REG_DISABLE_QUARTER_PIXEL_MV;
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_CTRL0);

	reg = VEPU_REG_MAD_THRESHOLD(0) |
	      VEPU_REG_IN_IMG_CTRL_FMT(ctx->vpu_src_fmt->enc_fmt) |
	      VEPU_REG_IN_IMG_ROTATE_MODE(0);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_CTRL1);

	reg = VEPU_REG_PPS_INIT_QP(h264->state.pps.pic_init_qp_minus26 + 26) |
	      VEPU_REG_SLICE_FILTER_ALPHA(0) |
	      VEPU_REG_SLICE_FILTER_BETA(0) |
	      VEPU_REG_CHROMA_QP_OFFSET(h264->state.pps.chroma_qp_index_offset) |
	      VEPU_REG_IDR_PIC_ID(encode->idr_pic_id);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_CTRL2);

	reg = VEPU_REG_PPS_ID(encode->pic_parameter_set_id) |
	      VEPU_REG_INTRA_PRED_MODE(h264_pred_mode_favor[qp]) |
	      VEPU_REG_FRAME_NUM(encode->frame_num);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_CTRL3);

	/* Crop inside the last macroblock column and row. */
	hantro_h264_enc_visible(ctx, &vis_w, &vis_h);
	overfill_r = (mb_w * MB_DIM - vis_w) / 4;
	overfill_b = mb_h * MB_DIM - vis_h;
	scaler = max(1U, 200 / (mb_w + mb_h));
	skip_penalty = min(255U, h264_skip_sad_penalty[qp] * scaler);
	reg = VEPU_REG_STREAM_START_OFFSET((h264->hdr_len & 7) * 8) |
	      VEPU_REG_SKIP_MACROBLOCK_PENALTY(skip_penalty) |
	      VEPU_REG_IN_IMG_CTRL_OVRFLR_D4(overfill_r) |
	      VEPU_REG_IN_IMG_CTRL_OVRFLB(overfill_b);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_OVER_FILL_STRM_OFFSET);

	vepu_write_relaxed(vpu, VEPU_REG_ZERO_MV_FAVOR_D2(10), VEPU_REG_MVC_RELATE);

	reg = VEPU_REG_INTRA16X16_MODE(h264_intra16_favor[qp]) |
	      VEPU_REG_INTER_MODE(h264_inter_favor[qp]);
	vepu_write_relaxed(vpu, reg, VEPU_REG_INTRA_INTER_MODE);

	/*
	 * Frame-level rate control: one QP for the whole frame, no MAD or
	 * checkpoint adjustment inside it.
	 */
	vepu_write_relaxed(vpu, 0, VEPU_QP_ADJUST_MAD_DELTA_ROI);
	reg = VEPU_REG_H264_LUMA_INIT_QP(qp) |
	      VEPU_REG_H264_QP_MAX(qp) |
	      VEPU_REG_H264_QP_MIN(qp) |
	      VEPU_REG_H264_CHKPT_DISTANCE(0);
	vepu_write_relaxed(vpu, reg, VEPU_REG_QP_VAL);
	for (i = 0; i < 5; i++)
		vepu_write_relaxed(vpu, 0, VEPU_REG_CHECKPOINT(i));
	for (i = 0; i < 3; i++)
		vepu_write_relaxed(vpu, 0, VEPU_REG_CHKPT_WORD_ERR(i));
	vepu_write_relaxed(vpu, 0, VEPU_REG_CHKPT_DELTA_QP);

	reg = VEPU_REG_4MV_PENALTY(h264_diff_mv_penalty4p[qp]) |
	      VEPU_REG_1MV_PENALTY(h264_diff_mv_penalty[qp]) |
	      VEPU_REG_QMV_PENALTY(h264_diff_mv_penalty[qp]) |
	      VEPU_REG_SPLIT_MV_MODE_EN;
	vepu_write_relaxed(vpu, reg, VEPU_REG_MV_PENALTY);

	for (i = 0; i < ARRAY_SIZE(dmv_penalty); i++) {
		dmv_penalty[i] = i;
		dmv_qpel_penalty[i] = min(255, exp_golomb_signed(i));
	}

	for (i = 0; i < ARRAY_SIZE(dmv_penalty); i += 4) {
		reg = VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i], 3) |
		      VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i + 1], 2) |
		      VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i + 2], 1) |
		      VEPU_REG_DMV_PENALTY_TABLE_BIT(dmv_penalty[i + 3], 0);
		vepu_write_relaxed(vpu, reg, VEPU_REG_DMV_PENALTY_TBL(i / 4));

		reg = VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i], 3) |
		      VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i + 1], 2) |
		      VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i + 2], 1) |
		      VEPU_REG_DMV_Q_PIXEL_PENALTY_TABLE_BIT(dmv_qpel_penalty[i + 3], 0);
		vepu_write_relaxed(vpu, reg, VEPU_REG_DMV_Q_PIXEL_PENALTY_TBL(i / 4));
	}

	/* Unused features */
	vepu_write_relaxed(vpu, 0, VEPU_REG_ADDR_NEXT_PIC);
	vepu_write_relaxed(vpu, 0, VEPU_REG_ADDR_MV_OUT);
	vepu_write_relaxed(vpu, 0, VEPU_REG_ADDR_CABAC_TBL);
	vepu_write_relaxed(vpu, 0, VEPU_REG_STABILIZATION_OUTPUT);

	vepu_write(vpu, VEPU_REG_INTERRUPT_TIMEOUT_EN, VEPU_REG_INTERRUPT);

	if (intra)
		dst_buf->flags |= V4L2_BUF_FLAG_KEYFRAME;
	else
		dst_buf->flags |= V4L2_BUF_FLAG_PFRAME;

	reg = VEPU_REG_MB_HEIGHT(mb_h) |
	      VEPU_REG_MB_WIDTH(mb_w) |
	      (intra ? VEPU_REG_FRAME_TYPE_INTRA : VEPU_REG_FRAME_TYPE_INTER) |
	      VEPU_REG_ENCODE_FORMAT_H264 |
	      VEPU_REG_ENCODE_ENABLE;

	/* Kick the watchdog and start encoding. */
	hantro_end_prepare_run(ctx);
	vepu_write(vpu, reg, VEPU_REG_ENCODE_START);

	return 0;
}

void rockchip_vpu2_h264_enc_done(struct hantro_ctx *ctx)
{
	struct hantro_dev *vpu = ctx->dev;
	struct hantro_h264_enc_hw_ctx *h264 = &ctx->h264_enc;
	struct vb2_v4l2_buffer *dst_buf = hantro_get_dst_buf(ctx);
	/* Bits written, counted from the aligned stream start. */
	u32 bits = vepu_read(vpu, VEPU_REG_STR_BUF_LIMIT);
	size_t bytesused = h264->strm_base + DIV_ROUND_UP(bits, 8);

	bytesused = min(bytesused, vb2_plane_size(&dst_buf->vb2_buf, 0));
	vb2_set_plane_payload(&dst_buf->vb2_buf, 0, bytesused);

	hantro_h264_enc_complete(ctx, bytesused);
}
