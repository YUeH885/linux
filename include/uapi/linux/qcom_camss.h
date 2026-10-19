/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_QCOM_CAMSS_H
#define _UAPI_LINUX_QCOM_CAMSS_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>

#define V4L2_CID_USER_QCOM_CAMSS_BASE \
	(V4L2_CID_USER_BASE + 0x1250)
#define V4L2_CID_QCOM_CAMSS_IFE_CONFIG \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 0)

#define V4L2_QCOM_CAMSS_IFE_CONFIG_VERSION	2

#define V4L2_QCOM_CAMSS_IFE_CONFIG_BLACK_LEVEL	(1U << 0)
#define V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE	(1U << 1)
#define V4L2_QCOM_CAMSS_IFE_CONFIG_CCM		(1U << 2)
#define V4L2_QCOM_CAMSS_IFE_CONFIG_GAMMA		(1U << 3)
#define V4L2_QCOM_CAMSS_IFE_CONFIG_ALL \
	(V4L2_QCOM_CAMSS_IFE_CONFIG_BLACK_LEVEL | \
	 V4L2_QCOM_CAMSS_IFE_CONFIG_WHITE_BALANCE | \
	 V4L2_QCOM_CAMSS_IFE_CONFIG_CCM | \
	 V4L2_QCOM_CAMSS_IFE_CONFIG_GAMMA)

#define V4L2_QCOM_CAMSS_IFE_GAMMA_CHANNELS	3
#define V4L2_QCOM_CAMSS_IFE_GAMMA_ENTRIES	65

/*
 * IFE tuning payload for VFE_LINE_PIX. frame_sequence is the sequence number
 * of the next frame to receive the update; use zero while the line is stopped.
 * black_level and black_level_comp use the IFE 14-bit code domain; the
 * hardware encodes the compensation thresholds at 8-bit precision. White
 * balance gains use unsigned Q7, white balance offsets use signed 15-bit
 * values. White balance arrays use R, Gr, Gb, B order; Gr and Gb must match
 * because WB12 has a single green channel. CCM coefficients use signed Q7
 * in RGB row-major order and CCM offsets use signed 11-bit RGB values.
 * Gamma is supplied as 65 10-bit samples per channel in R, G, B order;
 * the driver derives the 64 hardware
 * base/delta entries from consecutive samples.
 */
struct v4l2_qcom_camss_ife_config {
	__u32 version;
	__u32 frame_sequence;
	__u32 flags;
	__u32 reserved;
	__u16 black_level;
	__u16 black_level_comp[4];
	__u16 reserved1;
	__u16 wb_gain_q7[4];
	__s16 wb_offset[4];
	__s16 ccm_q7[9];
	__s16 ccm_offset[3];
	__u16 gamma[V4L2_QCOM_CAMSS_IFE_GAMMA_CHANNELS]
		[V4L2_QCOM_CAMSS_IFE_GAMMA_ENTRIES];
};

#define V4L2_QCOM_CAMSS_STATS_VERSION	1

#define V4L2_QCOM_CAMSS_AWB_BG_H_NUM	32
#define V4L2_QCOM_CAMSS_AWB_BG_V_NUM	24
#define V4L2_QCOM_CAMSS_AWB_BG_REGIONS	\
	(V4L2_QCOM_CAMSS_AWB_BG_H_NUM * V4L2_QCOM_CAMSS_AWB_BG_V_NUM)
#define V4L2_QCOM_CAMSS_AWB_BG_REGION_SIZE	48
#define V4L2_QCOM_CAMSS_BHIST_BINS	1024
#define V4L2_QCOM_CAMSS_BHIST_CHANNELS	1
#define V4L2_QCOM_CAMSS_BF_ROIS		25
#define V4L2_QCOM_CAMSS_BF_REGION_SIZE	32
#define V4L2_QCOM_CAMSS_BF_SIZE \
	(8 + V4L2_QCOM_CAMSS_BF_ROIS * V4L2_QCOM_CAMSS_BF_REGION_SIZE)

/*
 * Raw 3A statistics for the most recent IFE frame. The caller supplies user
 * buffers through awb_bg, bhist and bf; the driver copies the hardware DMA
 * output. AWB BG uses V4L2_QCOM_CAMSS_AWB_BG_REGIONS entries of 48 bytes,
 * BHIST uses 1024 32-bit bins for the selected channel and BF uses 25 32-byte
 * regions following an eight-byte frame tag. All DMA fields are little-endian.
 *
 * Each AWB BG region contains four 30-bit sums in R, B, Gr, Gb order, followed
 * by two words containing 16-bit R/B and Gr/Gb counts. Six more words hold the
 * same fields for saturated pixels. BHIST bins contain 25-bit counts.
 * Each BF region contains four 64-bit words: H sum[36:0]/count[61:39],
 * H sharpness[39:0]/region ID[61:54], V sum[36:0]/count[61:39],
 * and V sharpness[39:0]. sequence uses the PIX SOF frame number;
 * timestamp is the CLOCK_MONOTONIC time of snapshot completion in nanoseconds.
 * The last complete snapshot remains available after stream-off.
 */
struct v4l2_qcom_camss_stats {
	__u32 version;
	__u32 sequence;
	__aligned_u64 timestamp;
	__u32 awb_bg_regions;
	__u32 bhist_bins;
	__u32 bf_regions;
	__u32 reserved;
	__aligned_u64 awb_bg;
	__aligned_u64 bhist;
	__aligned_u64 bf;
};

#define VIDIOC_QCOM_CAMSS_GET_STATS \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 0, struct v4l2_qcom_camss_stats)

#endif /* _UAPI_LINUX_QCOM_CAMSS_H */
