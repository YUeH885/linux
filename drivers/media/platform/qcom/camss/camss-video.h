/* SPDX-License-Identifier: GPL-2.0 */
/*
 * camss-video.h
 *
 * Qualcomm MSM Camera Subsystem - V4L2 device node
 *
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 * Copyright (C) 2015-2018 Linaro Ltd.
 */
#ifndef QC_MSM_CAMSS_VIDEO_H
#define QC_MSM_CAMSS_VIDEO_H

#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <media/media-request.h>
#include <media/v4l2-ctrls.h>
#include <linux/videodev2.h>
#include <media/media-entity.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mediabus.h>
#include <media/videobuf2-v4l2.h>
#include <uapi/linux/qcom_camss.h>

struct camss_buffer {
	struct vb2_v4l2_buffer vb;
	dma_addr_t addr[3];
	struct list_head queue;
	struct v4l2_qcom_camss_ife_config ife;
	struct work_struct complete_work;
	enum vb2_buffer_state completion_state;
	u32 input_sequence;
	bool ife_valid;
	bool ife_applied;
	bool request_completed;
	bool dummy;
};

struct camss_video;

struct camss_video_ops {
	int (*queue_buffer)(struct camss_video *vid, struct camss_buffer *buf);
	int (*flush_buffers)(struct camss_video *vid,
			     enum vb2_buffer_state state);
	long (*get_stats)(struct camss_video *vid,
			  struct v4l2_qcom_camss_stats *stats);
	long (*get_frame_sequence)(struct camss_video *vid, u32 *sequence);
	int (*prepare_request)(struct camss_video *vid, struct camss_buffer *buf);
	int (*start_requests)(struct camss_video *vid);
	void (*stop_requests)(struct camss_video *vid);
};

struct camss_video {
	struct camss *camss;
	struct vb2_queue vb2_q;
	struct video_device vdev;
	struct media_pad pad;
	struct v4l2_format active_fmt;
	enum v4l2_buf_type type;
	struct media_pipeline pipe;
	const struct camss_video_ops *ops;
	struct v4l2_ctrl_handler *request_controls;
	struct workqueue_struct *request_wq;
	struct mutex lock;
	struct mutex q_lock;
	unsigned int bpl_alignment;
	unsigned int line_based;
	const struct camss_format_info *formats;
	unsigned int nformats;
};

int msm_video_register(struct camss_video *video, struct v4l2_device *v4l2_dev,
		       const char *name);

void msm_video_unregister(struct camss_video *video);
int camss_video_request_validate(struct media_request *request);
void camss_buffer_done(struct camss_buffer *buffer, enum vb2_buffer_state state);

#endif /* QC_MSM_CAMSS_VIDEO_H */
