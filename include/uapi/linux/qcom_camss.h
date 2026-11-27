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

#define V4L2_CID_QCOM_CAMSS_IFE_CROP \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 1)

#define V4L2_CID_QCOM_CAMSS_BF_ROI \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 2)

#define V4L2_CID_QCOM_CAMSS_LSC \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 3)

#define V4L2_CID_QCOM_CAMSS_DEMUX \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 4)

#define V4L2_CID_QCOM_CAMSS_BPC \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 5)

#define V4L2_CID_QCOM_CAMSS_ABF \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 6)

#define V4L2_CID_QCOM_CAMSS_GTM \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 7)

#define V4L2_QCOM_CAMSS_LSC_H_NUM	17
#define V4L2_QCOM_CAMSS_LSC_V_NUM	13
#define V4L2_QCOM_CAMSS_LSC_POINTS \
	(V4L2_QCOM_CAMSS_LSC_H_NUM * V4L2_QCOM_CAMSS_LSC_V_NUM)

/*
 * Mesh rolloff (LSC 3.4) 17x13 lens shading table for VFE_LINE_PIX.
 * Channel gains use unsigned 13-bit Q10 ([1024..8191], 1024 = 1.0x).
 * Setting enable = 0 disables the hardware LSC block.
 */
struct v4l2_qcom_camss_lsc {
	__u32 enable;
	__u16 r_q10[V4L2_QCOM_CAMSS_LSC_POINTS];
	__u16 gr_q10[V4L2_QCOM_CAMSS_LSC_POINTS];
	__u16 gb_q10[V4L2_QCOM_CAMSS_LSC_POINTS];
	__u16 b_q10[V4L2_QCOM_CAMSS_LSC_POINTS];
};

/*
 * Demux 1.3 per-channel unsigned 15-bit Q10 gains ([0..32767], 1024 = 1.0x)
 * in R, Gr, Gb, B order. Allows balancing Gr/Gb mismatch before WB12.
 */
struct v4l2_qcom_camss_demux {
	__u16 gain_q10[4];
};

#define V4L2_QCOM_CAMSS_BPC_ENABLE_BPC		(1U << 0)
#define V4L2_QCOM_CAMSS_BPC_ENABLE_BCC		(1U << 1)
#define V4L2_QCOM_CAMSS_BPC_ENABLE_CROSSTALK	(1U << 2)

/*
 * Bad pixel and bad cluster correction (BPC/BCC 5.0) parameters for
 * VFE_LINE_PIX. WB cross-channel ratios are derived automatically from the
 * active WB12 gains.
 */
struct v4l2_qcom_camss_bpc {
	__u32 enable;
	__u16 sat_value;
	__u16 fmax;
	__u16 fmin;
	__u16 bcc_offset;
	__u16 offset_hot;
	__u16 offset_cold;
};

#define V4L2_QCOM_CAMSS_ABF_LUT_ENTRIES		65

#define V4L2_QCOM_CAMSS_ABF_ENABLE_FILTER	(1U << 0)
#define V4L2_QCOM_CAMSS_ABF_ENABLE_SINGLE_BPC	(1U << 1)
#define V4L2_QCOM_CAMSS_ABF_ENABLE_CROSS_PLANE	(1U << 2)

/*
 * Adaptive Bayer Filter (ABF 3.4) bilateral denoise configuration for
 * VFE_LINE_PIX. noise_std_lut holds 65 9-bit noise standard deviation
 * thresholds ([1..511]); the driver derives the 64 hardware base/delta DMI
 * entries and frame-center radial distance registers automatically.
 */
struct v4l2_qcom_camss_abf {
	__u32 enable;
	__u8 act_th_r;
	__u8 act_th_g;
	__u8 act_th_b;
	__u8 blk_pix_r;
	__u8 blk_pix_g;
	__u8 blk_pix_b;
	__u8 bpc_max_shft;
	__u8 bpc_min_shft;
	__u16 bpc_offset;
	__u16 reserved;
	__u8 spatial_wt[4];
	__u8 curve_offset[8];
	__u8 edge_softness[4];
	__u16 noise_preserve_lo;
	__u16 noise_preserve_span;
	__u16 noise_preserve_base[2];
	__u16 noise_std_lut[V4L2_QCOM_CAMSS_ABF_LUT_ENTRIES];
	__u16 reserved1;
};

#define V4L2_QCOM_CAMSS_GTM_ENTRIES		65

/*
 * Global Tone Mapping (GTM 1.0) curve for VFE_LINE_PIX. y_ratio_q12 holds 65
 * 18-bit unsigned Q12 tone gains ([0..262143], 4096 = 1.0x) sampled at the
 * 65 hardware luminance knots in [0..16383]; the driver derives the 64
 * 64-bit base/slope DMI entries automatically.
 */
struct v4l2_qcom_camss_gtm {
	__u32 enable;
	__u32 y_ratio_q12[V4L2_QCOM_CAMSS_GTM_ENTRIES];
};

/*
 * Active Bayer Focus statistics grid window for VFE_LINE_PIX in sink pad
 * sensor pixels. The 5x5 hardware BF grid divides [left, left + width) x
 * [top, top + height) evenly within the hardware filter margins
 * ([64, sink_width - 64) x [32, sink_height - 32)). All fields must be even;
 * all zeros reset to the default full-frame grid.
 */
struct v4l2_qcom_camss_bf_roi {
	__u16 left;
	__u16 top;
	__u16 width;
	__u16 height;
};

/*
 * Per-request geometry for the NV12 PIX stream. Scale the sensor frame
 * to compose_width x compose_height before cropping at left, top.
 * Offsets are in scaled luma pixels. All four fields must be even,
 * compose_width x compose_height must lie within [negotiated compose, sink],
 * and the negotiated output size is fixed.
 * If omitted, that request uses the negotiated compose and crop.
 */
struct v4l2_qcom_camss_ife_crop {
	__u16 compose_width;
	__u16 compose_height;
	__u16 left;
	__u16 top;
};

#define V4L2_QCOM_CAMSS_IFE_CONFIG_VERSION	6

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
 * black_level and black_level_comp use the IFE 14-bit code domain ([0..16383]).
 * White balance gains use unsigned Q7, white balance offsets use signed 15-bit
 * values. White balance arrays use R, Gr, Gb, B order; Gr and Gb must match
 * because WB12 has a single green channel. CCM coefficients use signed Q7
 * in RGB row-major order and CCM offsets use signed 11-bit RGB values.
 * Request controls require frame_sequence zero, both WHITE_BALANCE and CCM
 * flags, and may optionally include BLACK_LEVEL and GAMMA. sensor_delay must
 * be two for requests and zero for global tuning. A
 * request contains one NV12 PIX buffer, this control and the sensor
 * exposure, analogue gain and vertical blanking control handler. The
 * completed request returns the applied input frame in frame_sequence and its
 * CAMIF SOF time in sensor_timestamp (CLOCK_MONOTONIC nanoseconds). Both
 * sensor_timestamp and frame_duration_us must be zero on submission.
 * frame_duration_us is derived from two successive CAMIF SOF timestamps.
 * Gamma is supplied as 65 10-bit samples per channel in R, G, B order;
 * the driver derives the 64 hardware
 * base/delta entries from consecutive samples.
 */
struct v4l2_qcom_camss_ife_config {
	__u32 version;
	__u32 frame_sequence;
	__u32 flags;
	__u32 sensor_delay;
	__u16 black_level;
	__u16 black_level_comp[4];
	__u16 reserved1;
	__u16 wb_gain_q7[4];
	__s16 wb_offset[4];
	__s16 ccm_q7[9];
	__s16 ccm_offset[3];
	__u16 gamma[V4L2_QCOM_CAMSS_IFE_GAMMA_CHANNELS]
		[V4L2_QCOM_CAMSS_IFE_GAMMA_ENTRIES];
	__u32 frame_duration_us;
	__aligned_u64 sensor_timestamp;
};

#define V4L2_QCOM_CAMSS_STATS_VERSION	3
#define V4L2_QCOM_CAMSS_STATS_SENSOR_CONTROLS	(1U << 0)

#define V4L2_QCOM_CAMSS_AWB_BG_H_NUM	64
#define V4L2_QCOM_CAMSS_AWB_BG_V_NUM	48
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
	__u32 flags;
	__u32 exposure_lines;
	__u32 analogue_gain;
	__u32 reserved1;
};

#define VIDIOC_QCOM_CAMSS_GET_STATS \
	_IOWR('V', BASE_VIDIOC_PRIVATE + 0, struct v4l2_qcom_camss_stats)

/* Next PIX input SOF sequence, or zero while the output is stopped. */
#define VIDIOC_QCOM_CAMSS_GET_FRAME_SEQUENCE \
	_IOR('V', BASE_VIDIOC_PRIVATE + 1, __u32)

/* Earliest input target for the next queued request; zero while stopped. */
#define VIDIOC_QCOM_CAMSS_GET_REQUEST_SEQUENCE \
	_IOR('V', BASE_VIDIOC_PRIVATE + 2, __u32)

#endif /* _UAPI_LINUX_QCOM_CAMSS_H */
