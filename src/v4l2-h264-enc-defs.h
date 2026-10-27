/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Definitions of the V4L2 H.264 stateless encode series that are not in
 * Linux 6.12, kept internal to this module.
 *
 * Taken from "[PATCH 02/14] media: uapi: Add H.264 stateless encode support"
 * and "[PATCH 03/14] media: h264: Add SPS video definitions" by Paul
 * Kocialkowski (message id 20260522101653.2565125-1-paulk@sys-base.io).
 * Here they are plain kernel-internal types: nothing in this module exposes
 * them to userspace, and no new control type is registered.
 *
 * Copyright (C) 2025-2026 Paul Kocialkowski <paulk@sys-base.io>
 */

#ifndef _HANTRO_V4L2_H264_ENC_DEFS_H
#define _HANTRO_V4L2_H264_ENC_DEFS_H

#include <linux/bits.h>
#include <linux/types.h>
#include <linux/v4l2-controls.h>

/* From PATCH 02/14 (include/uapi/linux/v4l2-controls.h). */

#define V4L2_H264_ENCODE_FLAG_IDR_PIC				0x01
#define V4L2_H264_ENCODE_FLAG_FIELD_PIC				0x02
#define V4L2_H264_ENCODE_FLAG_BOTTOM_FIELD			0x04
#define V4L2_H264_ENCODE_FLAG_DIRECT_SPATIAL_MV_PRED		0x08
#define V4L2_H264_ENCODE_FLAG_NUM_REF_IDX_ACTIVE_OVERRIDE	0x10
#define V4L2_H264_ENCODE_FLAG_NO_OUTPUT_OF_PRIOR_PICS		0x20
#define V4L2_H264_ENCODE_FLAG_LONG_TERM_REFERENCE		0x40

struct v4l2_ctrl_h264_encode_params {
	__u8 nal_ref_idc;
	__u8 slice_type;
	__u8 pic_parameter_set_id;
	__u8 colour_plane_id;
	__u16 frame_num;
	__u16 idr_pic_id;
	__u16 pic_order_cnt_lsb;
	__u8 reserved0[2];
	__s32 delta_pic_order_cnt_bottom;
	__s32 delta_pic_order_cnt0;
	__s32 delta_pic_order_cnt1;
	__u8 num_ref_idx_l0_active_minus1;
	__u8 num_ref_idx_l1_active_minus1;
	__u8 cabac_init_idc;
	__s8 slice_qp_delta;
	__u8 disable_deblocking_filter_idc;
	__s8 slice_alpha_c0_offset_div2;
	__s8 slice_beta_offset_div2;
	__u8 reserved1[6];
	__u32 flags; /* V4L2_H264_ENCODE_FLAG_ */
};

/* From PATCH 03/14 (include/media/v4l2-h264.h). */

#define V4L2_H264_START_CODE_ANNEX_B		0x00000001

#define V4L2_H264_NALU_TYPE_SLICE_NON_IDR	1
#define V4L2_H264_NALU_TYPE_SLICE_IDR		5
#define V4L2_H264_NALU_TYPE_SPS			7
#define V4L2_H264_NALU_TYPE_PPS			8
#define V4L2_H264_NALU_TYPE_AUD			9

#define V4L2_H264_PRIMARY_PIC_TYPE_I		0
#define V4L2_H264_PRIMARY_PIC_TYPE_IP		1
#define V4L2_H264_PRIMARY_PIC_TYPE_IPB		2

#define V4L2_H264_VUI_ASPECT_RATIO_IDC_EXTENDED	255

#define V4L2_H264_VUI_VIDEO_COMPONENT		0
#define V4L2_H264_VUI_VIDEO_PAL			1
#define V4L2_H264_VUI_VIDEO_NTSC		2
#define V4L2_H264_VUI_VIDEO_SECAM		3
#define V4L2_H264_VUI_VIDEO_MAC			4
#define V4L2_H264_VUI_VIDEO_UNSPECIFIED		5

#define V4L2_H264_VUI_COLOUR_BT709		1
#define V4L2_H264_VUI_COLOUR_UNSPECIFIED	2
#define V4L2_H264_VUI_COLOUR_BT470_SYSTEM_M	4
#define V4L2_H264_VUI_COLOUR_BT470_SYSTEM_BG	5
#define V4L2_H264_VUI_COLOUR_SMPTE170M		6
#define V4L2_H264_VUI_COLOUR_SMPTE240M		7
#define V4L2_H264_VUI_COLOUR_BT2020		9

#define V4L2_H264_VUI_TRANSFER_BT709		1
#define V4L2_H264_VUI_TRANSFER_UNSPECIFIED	2
#define V4L2_H264_VUI_TRANSFER_BT470_SYSTEM_M	4
#define V4L2_H264_VUI_TRANSFER_BT470_SYSTEM_BG	5
#define V4L2_H264_VUI_TRANSFER_SMPTE170M	6
#define V4L2_H264_VUI_TRANSFER_SMPTE240M	7
#define V4L2_H264_VUI_TRANSFER_LINEAR		8
#define V4L2_H264_VUI_TRANSFER_SRGB		13

#define V4L2_H264_VUI_MATRIX_IDENTITY		0
#define V4L2_H264_VUI_MATRIX_BT709		1
#define V4L2_H264_VUI_MATRIX_UNSPECIFIED	2
#define V4L2_H264_VUI_MATRIX_BT470_SYSTEM_M	4
#define V4L2_H264_VUI_MATRIX_BT470_SYSTEM_BG	5
#define V4L2_H264_VUI_MATRIX_SMPTE170M		6
#define V4L2_H264_VUI_MATRIX_SMPTE240M		7
#define V4L2_H264_VUI_MATRIX_BT2020		9
#define V4L2_H264_VUI_MATRIX_BT2020_CONST_LUM	10

#define V4L2_H264_SPS_VIDEO_FLAG_FRAME_CROPPING				BIT(0)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_PARAMETERS_PRESENT			BIT(1)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_ASPECT_RATIO_INFO_PRESENT		BIT(2)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_OVERSCAN_INFO_PRESENT		BIT(3)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_OVERSCAN_APPROPRIATE		BIT(4)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_VIDEO_SIGNAL_TYPE_PRESENT		BIT(5)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_VIDEO_FULL_RANGE			BIT(6)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_COLOUR_DESCRIPTION_PRESENT		BIT(7)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_CHROMA_LOC_INFO_PRESENT		BIT(8)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_TIMING_INFO_PRESENT		BIT(9)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_FIXED_FRAME_RATE			BIT(10)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_NAL_HRD_PARAMETERS_PRESENT		BIT(11)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_VCL_HRD_PARAMETERS_PRESENT		BIT(12)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_LOW_DELAY_HRD			BIT(13)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_PIC_STRUCT_PRESENT			BIT(14)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_BITSTREAM_RESTRICTION		BIT(15)
#define V4L2_H264_SPS_VIDEO_FLAG_VUI_MOTION_VECTORS_OVER_PIC_BOUNDARIES	BIT(16)

struct v4l2_h264_sps_video_hrd {
	u8 cpb_cnt_minus1;
	u8 bit_rate_scale;
	u8 cpb_size_scale;

	u32 bit_rate_value_minus1[32];
	u32 cpb_size_value_minus1[32];
	u8 cbr_flag[32];

	u8 initial_cpb_removal_delay_length_minus1;
	u8 cpb_removal_delay_length_minus1;
	u8 dpb_output_delay_length_minus1;
	u8 time_offset_length;
};

struct v4l2_h264_sps_video {
	u32 frame_crop_left_offset;
	u32 frame_crop_right_offset;
	u32 frame_crop_top_offset;
	u32 frame_crop_bottom_offset;

	u8 aspect_ratio_idc;
	u16 sar_width;
	u16 sar_height;

	u8 video_format;
	u8 colour_primaries;
	u8 transfer_characteristics;
	u8 matrix_coefficients;

	u8 chroma_sample_loc_type_top_field;
	u8 chroma_sample_loc_type_bottom_field;

	u32 num_units_in_tick;
	u32 time_scale;

	struct v4l2_h264_sps_video_hrd nal_hrd;
	struct v4l2_h264_sps_video_hrd vcl_hrd;

	u32 max_bytes_per_pic_denom;
	u32 max_bits_per_mb_denom;
	u32 log2_max_mv_length_horizontal;
	u32 log2_max_mv_length_vertical;
	u32 max_num_reorder_frames;
	u32 max_dec_frame_buffering;

	u32 flags;
};


#endif
