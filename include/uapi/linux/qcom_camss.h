/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_QCOM_CAMSS_H
#define _UAPI_LINUX_QCOM_CAMSS_H

#include <linux/types.h>
#include <linux/v4l2-controls.h>

#define V4L2_CID_USER_QCOM_CAMSS_BASE \
	(V4L2_CID_USER_BASE + 0x1250)
#define V4L2_CID_QCOM_CAMSS_IFE_CONFIG \
	(V4L2_CID_USER_QCOM_CAMSS_BASE + 0)

#define V4L2_QCOM_CAMSS_IFE_CONFIG_VERSION	1

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
#define V4L2_QCOM_CAMSS_IFE_GAMMA_ENTRIES	64

/*
 * IFE tuning payload for VFE_LINE_PIX. frame_sequence is the sequence number
 * of the next frame to receive the update; use zero while the line is stopped.
 * black_level and black_level_comp use the IFE 14-bit code domain; the
 * hardware encodes the compensation thresholds at 8-bit precision. White
 * balance gains use unsigned Q7, white balance offsets use signed 12-bit
 * values, CCM coefficients use signed Q7, CCM offsets use signed 12-bit
 * values, and gamma entries use 10-bit values.
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

#endif /* _UAPI_LINUX_QCOM_CAMSS_H */
