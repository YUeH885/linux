// SPDX-License-Identifier: GPL-2.0

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "camss.h"
#include "camss-vfe.h"
#include "camss-vfe-17x-stats.h"

#define VFE_MODULE_STATS_CGC_OVERRIDE		(0x030)
#define VFE_MODULE_STATS_EN			(0x044)
#define		STATS_AWB_BG_EN			BIT(3)
#define		STATS_BHIST_EN			BIT(4)
#define		STATS_BF_EN			BIT(2)

#define VFE_STATS_AWB_BG_RGN_OFFSET_CFG		(0x0bc8)
#define VFE_STATS_AWB_BG_RGN_NUM_CFG		(0x0bcc)
#define VFE_STATS_AWB_BG_RGN_SIZE_CFG		(0x0bd0)
#define VFE_STATS_AWB_BG_HI_THRESHOLD_0_CFG	(0x0bd4)
#define VFE_STATS_AWB_BG_HI_THRESHOLD_1_CFG	(0x0bd8)
#define VFE_STATS_AWB_BG_LO_THRESHOLD_0_CFG	(0x0bdc)
#define VFE_STATS_AWB_BG_LO_THRESHOLD_1_CFG	(0x0be0)
#define VFE_STATS_AWB_BG_CH_Y_CFG		(0x0c14)
#define VFE_STATS_AWB_BG_CFG			(0x0cb0)
#define VFE_STATS_BHIST_RGN_OFFSET_CFG		(0x0be4)
#define VFE_STATS_BHIST_RGN_NUM_CFG		(0x0be8)
#define VFE_STATS_BF_CFG			(0x0ae4)
#define VFE_STATS_BF_ACTIVE_WINDOW_CFG		(0x0ca4)
#define VFE_STATS_BF_SHIFT_BITS_CFG		(0x0b4c)
#define		STATS_BHIST_DMI_BANK		(0x3a)
#define		STATS_BF_DMI_BANK		(0x40)
#define VFE_STATS_AWB_BG_WM			(15)
#define VFE_STATS_BHIST_WM			(16)
#define VFE_STATS_BF_WM				(14)

#define VFE_DMI_CFG				(0x0c24)
#define VFE_DMI_ADDR				(0x0c28)
#define VFE_DMI_DATA_HI			(0x0c2c)
#define VFE_DMI_DATA_LO			(0x0c30)
#define		DMI_CFG_AUTO_INCREMENT		(0x100)

#define VFE_BUS_WM_CFG(n)			(0x2208 + (n) * 0x100)
#define VFE_BUS_WM_IMAGE_ADDR(n)		(0x2214 + (n) * 0x100)
#define VFE_BUS_WM_BUFFER_WIDTH_CFG(n)		(0x221c + (n) * 0x100)
#define VFE_BUS_WM_BUFFER_HEIGHT_CFG(n)		(0x2220 + (n) * 0x100)
#define VFE_BUS_WM_PACKER_CFG(n)		(0x2224 + (n) * 0x100)
#define VFE_BUS_WM_STRIDE(n)			(0x2228 + (n) * 0x100)
#define VFE_BUS_WM_FRAME_INC(n)			(0x2258 + (n) * 0x100)
#define VFE_BUS_WM_BURST_LIMIT(n)		(0x225c + (n) * 0x100)

enum vfe_stats_type {
	VFE_STATS_AWB,
	VFE_STATS_HIST,
	VFE_STATS_FOCUS,
	VFE_STATS_COUNT,
};

#define VFE_STATS_AWB_SIZE \
	(V4L2_QCOM_CAMSS_AWB_BG_REGIONS * V4L2_QCOM_CAMSS_AWB_BG_REGION_SIZE)
#define VFE_STATS_HIST_SIZE \
	(V4L2_QCOM_CAMSS_BHIST_BINS * V4L2_QCOM_CAMSS_BHIST_CHANNELS * sizeof(u32))
#define VFE_STATS_BF_OFFSET (VFE_STATS_AWB_SIZE + VFE_STATS_HIST_SIZE)
#define VFE_STATS_SIZE (VFE_STATS_BF_OFFSET + V4L2_QCOM_CAMSS_BF_SIZE)
#define VFE_STATS_DMA_SIZE ALIGN(VFE_STATS_SIZE, 64)
#define VFE_STATS_ALL_DONE GENMASK(VFE_STATS_COUNT - 1, 0)

static const struct {
	u8 wm;
	u32 offset;
	u32 size;
} vfe_stats_output[] = {
	{ VFE_STATS_AWB_BG_WM, 0, VFE_STATS_AWB_SIZE },
	{ VFE_STATS_BHIST_WM, VFE_STATS_AWB_SIZE, VFE_STATS_HIST_SIZE },
	{ VFE_STATS_BF_WM, VFE_STATS_BF_OFFSET, V4L2_QCOM_CAMSS_BF_SIZE },
};

struct vfe_17x_stats {
	void *cpu;
	dma_addr_t dma;
	void *pending;
	void *snapshot;
	void *reader;
	struct mutex read_lock;
	spinlock_t lock;
	u8 slot[VFE_STATS_COUNT];
	u8 done;
	u32 pending_sequence;
	u32 sequence;
	u64 timestamp;
	bool ready;
};

static void vfe_stats_wm_queue(struct vfe_device *vfe, u8 wm, dma_addr_t addr)
{
	writel_relaxed(addr, vfe->base + VFE_BUS_WM_IMAGE_ADDR(wm));
	writel_relaxed(0, vfe->base + VFE_BUS_WM_FRAME_INC(wm));
	writel(0x3, vfe->base + VFE_BUS_WM_CFG(wm));
}

static void vfe_stats_wm_start(struct vfe_device *vfe, u8 wm, dma_addr_t addr)
{
	writel_relaxed(0, vfe->base + VFE_BUS_WM_CFG(wm));
	writel_relaxed(0, vfe->base + VFE_BUS_WM_BUFFER_WIDTH_CFG(wm));
	writel_relaxed(0, vfe->base + VFE_BUS_WM_BUFFER_HEIGHT_CFG(wm));
	writel_relaxed(0xa, vfe->base + VFE_BUS_WM_PACKER_CFG(wm));
	writel_relaxed(1, vfe->base + VFE_BUS_WM_STRIDE(wm));
	writel_relaxed(0xf, vfe->base + VFE_BUS_WM_BURST_LIMIT(wm));
	vfe_stats_wm_queue(vfe, wm, addr);
	vfe_stats_wm_queue(vfe, wm, addr + VFE_STATS_DMA_SIZE);
}

static u32 vfe_bf_fir_word(const s8 *a)
{
	return (a[0] & 0x3f) | ((a[1] & 0x3f) << 6) |
	       ((a[2] & 0x3f) << 12) | ((a[3] & 0x3f) << 18);
}

static u32 vfe_bf_iir_word(s16 a, s16 b)
{
	return (u16)a | ((u32)(u16)b << 16);
}

static void vfe_stats_bf_filters(struct vfe_device *vfe)
{
	static const s8 h_fir[16] = {
		-1, -2, -1, 1, 5, 8, 10, 8, 5, 1, -1, -2, -1,
	};
	/* Horizontal IIR: B10/B11, B12/B22, A11/A12, B20/B21, A21/A22. */
	static const s16 h_iir[10] = {
		1513, 0, -1513, -1851, 28052, -13357, 1851, 0, 30632, -14725,
	};
	static const s16 v_iir[10] = {
		14662, -29324, 14662, -1851, 29141, -13120, 1851, 0, 30632, -14725,
	};
	unsigned int i, j;

	for (i = 0; i < 4; i++)
		writel_relaxed(vfe_bf_fir_word(&h_fir[i * 4]),
			       vfe->base + 0x0af0 + 4 * i);
	for (i = 0; i < 5; i++)
		writel_relaxed(vfe_bf_iir_word(h_iir[i * 2], h_iir[i * 2 + 1]),
			       vfe->base + 0x0af0 + 4 * (4 + i));

	for (i = 0; i < 3; i++)
		writel_relaxed(vfe_bf_iir_word(v_iir[i * 2], v_iir[i * 2 + 1]),
			       vfe->base + 0x0b40 + 4 * i);
	for (i = 0; i < 2; i++)
		writel_relaxed(vfe_bf_iir_word(v_iir[6 + i * 2],
					       v_iir[7 + i * 2]),
			       vfe->base + 0x0c9c + 4 * i);

	/* Each coring table has 17 five-bit values, spaced six bits apart. */
	writel_relaxed(0x10000, vfe->base + 0x0b50);
	writel_relaxed(0x10000, vfe->base + 0x0b78);
	for (i = 0; i < 4; i++) {
		u32 word = 0;

		for (j = 0; j < 5 && i * 5 + j < 17; j++)
			if (i * 5 + j)
				word |= 16 << (j * 6);
		writel_relaxed(word, vfe->base + 0x0b50 + 4 * (i + 1));
		writel_relaxed(word, vfe->base + 0x0b78 + 4 * (i + 1));
	}

	writel_relaxed(16, vfe->base + 0x0b8c);
	writel_relaxed(16, vfe->base + 0x0b90);
	writel_relaxed(0x0d, vfe->base + 0x0b4c);
}

static void vfe_stats_bf_dmi(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	const unsigned int h_num = 5;
	const unsigned int v_num = 5;
	const unsigned int roi_width = ((sink->width - 128) / h_num) & ~1U;
	const unsigned int roi_height = ((sink->height - 64) / v_num) & ~1U;
	const unsigned int left = 64;
	const unsigned int top = 32;
	unsigned int i;

	writel_relaxed(DMI_CFG_AUTO_INCREMENT | STATS_BF_DMI_BANK,
		       vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);

	/* DATA_LO commits a 64-bit entry and advances DMI_ADDR. */
	writel(0, vfe->base + VFE_DMI_DATA_HI);
	writel(0, vfe->base + VFE_DMI_DATA_LO);
	for (i = 0; i < h_num * v_num; i++) {
		unsigned int x = i % h_num;
		unsigned int y = i / h_num;
		u64 entry = (u64)(i & 0x1ff) << 54;

		entry |= (u64)((left + x * roi_width) & 0x1fff) << 41;
		entry |= (u64)((top + y * roi_height) & 0x3fff) << 27;
		entry |= (u64)((roi_width - 1) & 0xfff) << 14;
		entry |= (roi_height - 1) & 0x1fff;
		writel(upper_32_bits(entry), vfe->base + VFE_DMI_DATA_HI);
		writel(lower_32_bits(entry), vfe->base + VFE_DMI_DATA_LO);
	}
	writel(0, vfe->base + VFE_DMI_DATA_HI);
	writel(0, vfe->base + VFE_DMI_DATA_LO);

	writel_relaxed(0, vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
}

int vfe_17x_stats_start(struct vfe_line *line)
{
	struct vfe_device *vfe = to_vfe(line);
	struct v4l2_mbus_framefmt *sink = &line->fmt[MSM_VFE_PAD_SINK];
	unsigned int region_width =
		(sink->width / V4L2_QCOM_CAMSS_AWB_BG_H_NUM) & ~1U;
	unsigned int region_height =
		(sink->height / V4L2_QCOM_CAMSS_AWB_BG_V_NUM) & ~1U;
	struct vfe_17x_stats *stats = line->stats;
	unsigned long flags;
	unsigned int i;

	if (sink->width < 256 || sink->height < 128)
		return -EINVAL;

	if (!stats) {
		stats = devm_kzalloc(vfe->camss->dev, sizeof(*stats), GFP_KERNEL);
		if (!stats)
			return -ENOMEM;
		stats->cpu = dmam_alloc_coherent(vfe->camss->dev,
						2 * VFE_STATS_DMA_SIZE,
						&stats->dma, GFP_KERNEL);
		stats->pending = devm_kzalloc(vfe->camss->dev, VFE_STATS_SIZE,
					     GFP_KERNEL);
		stats->snapshot = devm_kzalloc(vfe->camss->dev, VFE_STATS_SIZE,
					      GFP_KERNEL);
		stats->reader = devm_kzalloc(vfe->camss->dev, VFE_STATS_SIZE,
					    GFP_KERNEL);
		if (!stats->cpu || !stats->pending || !stats->snapshot ||
		    !stats->reader) {
			devm_kfree(vfe->camss->dev, stats->reader);
			devm_kfree(vfe->camss->dev, stats->snapshot);
			devm_kfree(vfe->camss->dev, stats->pending);
			if (stats->cpu)
				dmam_free_coherent(vfe->camss->dev,
						   2 * VFE_STATS_DMA_SIZE,
						   stats->cpu, stats->dma);
			devm_kfree(vfe->camss->dev, stats);
			return -ENOMEM;
		}
		spin_lock_init(&stats->lock);
		mutex_init(&stats->read_lock);
		/* Publish the initialized buffers and lock to concurrent readers. */
		smp_store_release(&line->stats, stats);
	}
	spin_lock_irqsave(&stats->lock, flags);
	stats->ready = false;
	stats->done = 0;
	memset(stats->slot, 0, sizeof(stats->slot));
	spin_unlock_irqrestore(&stats->lock, flags);
	memset(stats->cpu, 0, 2 * VFE_STATS_DMA_SIZE);

	/* Stats clock gating is controlled explicitly while streaming. */
	writel_relaxed(~0U, vfe->base + VFE_MODULE_STATS_CGC_OVERRIDE);

	writel_relaxed(0, vfe->base + VFE_STATS_AWB_BG_RGN_OFFSET_CFG);
	writel_relaxed(((V4L2_QCOM_CAMSS_AWB_BG_V_NUM - 1) << 16) |
		       (V4L2_QCOM_CAMSS_AWB_BG_H_NUM - 1),
		       vfe->base + VFE_STATS_AWB_BG_RGN_NUM_CFG);
	writel_relaxed(((region_height - 1) << 16) | (region_width - 1),
		       vfe->base + VFE_STATS_AWB_BG_RGN_SIZE_CFG);
	writel_relaxed(0x3fff3fff,
		       vfe->base + VFE_STATS_AWB_BG_HI_THRESHOLD_0_CFG);
	writel_relaxed(0x3fff3fff,
		       vfe->base + VFE_STATS_AWB_BG_HI_THRESHOLD_1_CFG);
	writel_relaxed(0, vfe->base + VFE_STATS_AWB_BG_LO_THRESHOLD_0_CFG);
	writel_relaxed(0, vfe->base + VFE_STATS_AWB_BG_LO_THRESHOLD_1_CFG);
	writel_relaxed(0, vfe->base + VFE_STATS_AWB_BG_CH_Y_CFG);
	/* Saturation output plus all sixteen region sampling positions. */
	writel_relaxed(0x00ffff01, vfe->base + VFE_STATS_AWB_BG_CFG);

	writel_relaxed(0, vfe->base + VFE_STATS_BHIST_RGN_OFFSET_CFG);
	writel_relaxed((((sink->height / 2) - 1) << 16) |
		       ((sink->width / 2) - 1),
		       vfe->base + VFE_STATS_BHIST_RGN_NUM_CFG);

	/* The BHIST LUT is unused by the linear binning mode. */
	writel_relaxed(DMI_CFG_AUTO_INCREMENT | STATS_BHIST_DMI_BANK,
		       vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);
	for (i = 0; i < V4L2_QCOM_CAMSS_BHIST_BINS; i++)
		writel_relaxed(0, vfe->base + VFE_DMI_DATA_LO);
	writel_relaxed(0, vfe->base + VFE_DMI_CFG);
	writel_relaxed(0, vfe->base + VFE_DMI_ADDR);

	writel_relaxed(BIT(8) | BIT(9) | BIT(13),
		       vfe->base + VFE_STATS_BF_CFG);
	writel_relaxed(0, vfe->base + VFE_STATS_BF_CFG + 4);
	writel_relaxed(0, vfe->base + VFE_STATS_BF_CFG + 8);
	writel_relaxed(32 << 16 | 64,
		       vfe->base + VFE_STATS_BF_ACTIVE_WINDOW_CFG);
	writel_relaxed((sink->height - 32) << 16 | (sink->width - 64),
		       vfe->base + VFE_STATS_BF_ACTIVE_WINDOW_CFG + 4);
	writel_relaxed(0, vfe->base + VFE_STATS_BF_SHIFT_BITS_CFG);
	vfe_stats_bf_filters(vfe);
	vfe_stats_bf_dmi(line);

	writel_relaxed(STATS_AWB_BG_EN | STATS_BHIST_EN | STATS_BF_EN,
		       vfe->base + VFE_MODULE_STATS_EN);

	for (i = 0; i < VFE_STATS_COUNT; i++)
		vfe_stats_wm_start(vfe, vfe_stats_output[i].wm,
				   stats->dma + vfe_stats_output[i].offset);

	return 0;
}

void vfe_17x_stats_isr(struct vfe_device *vfe, u32 status)
{
	struct vfe_line *line = &vfe->line[VFE_LINE_PIX];
	/* Pair with the buffer publication in vfe_17x_stats_start(). */
	struct vfe_17x_stats *stats = smp_load_acquire(&line->stats);
	u32 sequence = line->output.sof_sequence - 1;
	unsigned long flags;
	unsigned int i;

	if (!stats || line->output.state != VFE_OUTPUT_ON)
		return;

	if (stats->pending_sequence != sequence) {
		stats->pending_sequence = sequence;
		stats->done = 0;
	}
	for (i = 0; i < VFE_STATS_COUNT; i++) {
		u32 offset = vfe_stats_output[i].offset;
		u32 slot_offset = stats->slot[i] * VFE_STATS_DMA_SIZE;

		if (!(status & BIT(vfe_stats_output[i].wm)))
			continue;

		dma_rmb();
		memcpy(stats->pending + offset, stats->cpu + slot_offset + offset,
		       vfe_stats_output[i].size);
		/* Two queued buffers keep DMA away from the completed buffer. */
		vfe_stats_wm_queue(vfe, vfe_stats_output[i].wm,
				   stats->dma + slot_offset + offset);
		stats->slot[i] ^= 1;
		stats->done |= BIT(i);
	}
	if (stats->done != VFE_STATS_ALL_DONE)
		return;

	spin_lock_irqsave(&stats->lock, flags);
	memcpy(stats->snapshot, stats->pending, VFE_STATS_SIZE);
	stats->sequence = sequence;
	stats->timestamp = ktime_get_ns();
	stats->ready = true;
	spin_unlock_irqrestore(&stats->lock, flags);
	stats->done = 0;
}

long vfe_17x_get_stats(struct camss_video *vid,
		      struct v4l2_qcom_camss_stats *request)
{
	struct vfe_line *line = container_of(vid, struct vfe_line, video_out);
	/* Pair with the buffer publication in vfe_17x_stats_start(). */
	struct vfe_17x_stats *stats = smp_load_acquire(&line->stats);
	unsigned long flags;
	long ret = 0;

	if (line->id != VFE_LINE_PIX || !stats)
		return -ENODATA;
	if (request->version != V4L2_QCOM_CAMSS_STATS_VERSION || request->reserved)
		return -EINVAL;
	if (!request->awb_bg || !request->bhist || !request->bf)
		return -EINVAL;

	if (mutex_lock_interruptible(&stats->read_lock))
		return -ERESTARTSYS;

	spin_lock_irqsave(&stats->lock, flags);
	if (!stats->ready) {
		ret = -EAGAIN;
	} else {
		memcpy(stats->reader, stats->snapshot, VFE_STATS_SIZE);
		request->sequence = stats->sequence;
		request->timestamp = stats->timestamp;
	}
	spin_unlock_irqrestore(&stats->lock, flags);
	if (ret)
		goto out;

	request->awb_bg_regions = V4L2_QCOM_CAMSS_AWB_BG_REGIONS;
	request->bhist_bins = V4L2_QCOM_CAMSS_BHIST_BINS;
	request->bf_regions = V4L2_QCOM_CAMSS_BF_ROIS;
	if (copy_to_user(u64_to_user_ptr(request->awb_bg), stats->reader,
			 VFE_STATS_AWB_SIZE) ||
	    copy_to_user(u64_to_user_ptr(request->bhist),
			 stats->reader + VFE_STATS_AWB_SIZE, VFE_STATS_HIST_SIZE) ||
	    copy_to_user(u64_to_user_ptr(request->bf),
			 stats->reader + VFE_STATS_BF_OFFSET, V4L2_QCOM_CAMSS_BF_SIZE))
		ret = -EFAULT;
out:
	mutex_unlock(&stats->read_lock);
	return ret;
}
