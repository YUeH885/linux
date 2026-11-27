/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_QCOM_FD_H
#define _UAPI_LINUX_QCOM_FD_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define QCOM_FD_MAX_FACES		35

/* Enable extended multi-angle/profile search in FD501 COND register. */
#define QCOM_FD_FLAG_ALL_ANGLES		(1U << 0)

struct qcom_fd_caps {
	__u32 core_version;
	__u32 wrapper_version;
	__u32 max_faces;
	__u32 reserved;
};

struct qcom_fd_face {
	__u16 center_x;
	__u16 center_y;
	__u16 size;
	__u16 confidence;
	__u16 roll;
	__s16 pose;
};

struct qcom_fd_detect {
	__u64 image_ptr;
	__u32 width;
	__u32 height;
	__u32 stride;
	__u32 min_face_size;
	__u32 threshold;
	__u32 flags;
	__u32 face_count;
	__u32 reserved;
	struct qcom_fd_face faces[QCOM_FD_MAX_FACES];
};

#define QCOM_FD_IOC_GET_CAPS	_IOR('F', 0x00, struct qcom_fd_caps)
#define QCOM_FD_IOC_DETECT	_IOWR('F', 0x01, struct qcom_fd_detect)

#endif /* _UAPI_LINUX_QCOM_FD_H */
