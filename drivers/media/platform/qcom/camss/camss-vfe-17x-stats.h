/* SPDX-License-Identifier: GPL-2.0 */
#ifndef QC_MSM_CAMSS_VFE_17X_STATS_H
#define QC_MSM_CAMSS_VFE_17X_STATS_H

#include <linux/bits.h>
#include <linux/types.h>

struct camss_video;
struct v4l2_qcom_camss_stats;
struct vfe_device;
struct vfe_line;

#define VFE_17X_STATS_IRQ_MASK GENMASK(16, 14)

int vfe_17x_stats_start(struct vfe_line *line);
void vfe_17x_stats_isr(struct vfe_device *vfe, u32 status);
long vfe_17x_get_stats(struct camss_video *vid,
		       struct v4l2_qcom_camss_stats *stats);

#endif
