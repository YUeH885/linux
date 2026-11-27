// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm SM8150 Titan 175 Hardware Face Detection (FD501) driver
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interconnect.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/uaccess.h>
#include <uapi/linux/qcom_fd.h>

/* FD501 Core registers (0x0ac5a000) */
#define FD_CORE_CONTROL			0x000
#define FD_CORE_RESULT_CNT		0x004
#define FD_CORE_COND			0x008
#define FD_CORE_START_X			0x00c
#define FD_CORE_START_Y			0x010
#define FD_CORE_IMG_WIDTH		0x014
#define FD_CORE_IMG_HEIGHT		0x018
#define FD_CORE_THRESHOLD		0x01c
#define FD_CORE_RESULT_ADDR		0x020
#define FD_CORE_IMAGE_ADDR		0x024
#define FD_CORE_WORK_ADDR		0x028
#define FD_CORE_IMAGE_SIZE_MODE		0x02c
#define FD_CORE_IMG_STRIDE		0x030
#define FD_CORE_RO_MODE			0x034
#define FD_CORE_VERSION			0x038
#define FD_CORE_RESULTS_BASE		0x400

/* FD Wrapper 2.0.0 registers (0x0ac5b000) */
#define FD_WRAPPER_VERSION		0x000
#define FD_WRAPPER_CGC_DISABLE		0x004
#define FD_WRAPPER_HW_STOP		0x008
#define FD_WRAPPER_SW_RESET		0x010
#define FD_WRAPPER_VBIF_REQ_PRIORITY	0x020
#define FD_WRAPPER_VBIF_PRIORITY_LEVEL	0x024
#define FD_WRAPPER_IRQ_MASK		0x050
#define FD_WRAPPER_IRQ_STATUS		0x054
#define FD_WRAPPER_IRQ_CLEAR		0x058

#define FD_IRQ_FRAME_DONE		BIT(0)
#define FD_IRQ_HALT_DONE		BIT(1)
#define FD_IRQ_RESET_DONE		BIT(2)

#define FD_QOS_PRIORITY			4
#define FD_QOS_PRIORITY_LEVEL		4

#define FD_MAX_WIDTH			854
#define FD_MAX_HEIGHT			480
#define FD_MAX_STRIDE			ALIGN(FD_MAX_WIDTH, 16)
#define FD_IMAGE_BUF_SIZE		ALIGN(FD_MAX_STRIDE * FD_MAX_HEIGHT, PAGE_SIZE)
#define FD_WORK_BUF_SIZE		(4 * 1024 * 1024)
#define FD_RESULT_BUF_SIZE		PAGE_SIZE

#define FD_HW_TIMEOUT_MS		500
#define FD_AUTOSUSPEND_MS		500

struct qcom_fd_clk_cfg {
	const char *name;
	unsigned long rate;
};

static const struct qcom_fd_clk_cfg qcom_fd_clk_cfgs[] = {
	{ "cam_ahb", 0 },
	{ "cam_hf_axi", 0 },
	{ "cam_sf_axi", 0 },
	{ "slow_ahb_src", 80000000 },
	{ "cpas_ahb", 80000000 },
	{ "core_ahb", 0 },
	/* 150 MHz is the LOW_SVS ceiling for cam_cc_camnoc_axi_clk_src. */
	{ "camnoc_axi", 150000000 },
	{ "fd_core", 400000000 },
	{ "fd_core_uar", 0 },
};

struct qcom_fd {
	struct device *dev;
	struct miscdevice miscdev;
	void __iomem *core_base;
	void __iomem *wrapper_base;
	int irq;

	struct clk_bulk_data clks[ARRAY_SIZE(qcom_fd_clk_cfgs)];
	struct icc_path *icc_ahb;
	struct icc_path *icc_sf_mem;
	struct icc_path *icc_sf;

	struct mutex lock;
	struct completion frame_done;
	struct completion halt_done;
	struct completion reset_done;

	void *image_cpu;
	dma_addr_t image_dma;
	void *work_cpu;
	dma_addr_t work_dma;
	void *result_cpu;
	dma_addr_t result_dma;

	u32 core_version;
	u32 wrapper_version;
};

static irqreturn_t qcom_fd_irq(int irq, void *data)
{
	struct qcom_fd *fd = data;
	u32 status;

	status = readl(fd->wrapper_base + FD_WRAPPER_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	writel(status, fd->wrapper_base + FD_WRAPPER_IRQ_CLEAR);

	if (status & FD_IRQ_HALT_DONE)
		complete(&fd->halt_done);
	if (status & FD_IRQ_RESET_DONE)
		complete(&fd->reset_done);
	if (status & FD_IRQ_FRAME_DONE)
		complete(&fd->frame_done);

	return IRQ_HANDLED;
}

static int qcom_fd_hw_reset(struct qcom_fd *fd)
{
	unsigned long timeout = msecs_to_jiffies(FD_HW_TIMEOUT_MS);

	/*
	 * FD501 errata requires masking only one IRQ bit at a time during
	 * halt, reset, and frame processing.
	 */
	writel(1, fd->wrapper_base + FD_WRAPPER_CGC_DISABLE);

	reinit_completion(&fd->halt_done);
	writel(FD_IRQ_HALT_DONE, fd->wrapper_base + FD_WRAPPER_IRQ_MASK);
	writel(1, fd->wrapper_base + FD_WRAPPER_HW_STOP);
	if (!wait_for_completion_timeout(&fd->halt_done, timeout))
		dev_warn(fd->dev, "FD halt timeout\n");

	reinit_completion(&fd->reset_done);
	writel(1, fd->core_base + FD_CORE_CONTROL);
	writel(FD_IRQ_RESET_DONE, fd->wrapper_base + FD_WRAPPER_IRQ_MASK);
	writel(1, fd->wrapper_base + FD_WRAPPER_SW_RESET);
	if (!wait_for_completion_timeout(&fd->reset_done, timeout)) {
		dev_err(fd->dev, "FD reset timeout\n");
		writel(0, fd->wrapper_base + FD_WRAPPER_CGC_DISABLE);
		return -ETIMEDOUT;
	}

	writel(0, fd->wrapper_base + FD_WRAPPER_CGC_DISABLE);
	writel(FD_QOS_PRIORITY, fd->wrapper_base + FD_WRAPPER_VBIF_REQ_PRIORITY);
	writel(FD_QOS_PRIORITY_LEVEL, fd->wrapper_base + FD_WRAPPER_VBIF_PRIORITY_LEVEL);

	return 0;
}

static int qcom_fd_size_mode(u32 width, u32 height)
{
	if (width == 320 && height == 240)
		return 0;
	if (width == 640 && height == 480)
		return 1;
	if (width == 427 && height == 240)
		return 2;
	if (width == 854 && height == 480)
		return 3;
	return -EINVAL;
}

static u32 qcom_fd_cond_value(u32 min_face_size, u32 flags)
{
	u32 cond;

	if (min_face_size <= 20)
		cond = 0x20;
	else if (min_face_size <= 25)
		cond = 0x21;
	else if (min_face_size <= 32)
		cond = 0x22;
	else
		cond = 0x23;

	if (flags & QCOM_FD_FLAG_ALL_ANGLES)
		cond |= 0x100;

	return cond;
}

static s16 qcom_fd_decode_pose(u32 pose_idx)
{
	static const s16 pose_lut[] = { 0, 0, 45, 90, -45, -90 };

	if (pose_idx < ARRAY_SIZE(pose_lut))
		return pose_lut[pose_idx];
	return 0;
}

static int qcom_fd_run_detect(struct qcom_fd *fd, struct qcom_fd_detect *req)
{
	int size_mode;
	u32 hw_stride, threshold, cond, face_cnt, i;
	const u8 __user *src;
	u8 *dst;
	int ret;

	size_mode = qcom_fd_size_mode(req->width, req->height);
	if (size_mode < 0)
		return -EINVAL;

	if (!req->image_ptr || req->stride < req->width || req->stride > 4096)
		return -EINVAL;

	hw_stride = ALIGN(req->width, 16);
	src = u64_to_user_ptr(req->image_ptr);
	dst = fd->image_cpu;

	for (i = 0; i < req->height; i++) {
		if (copy_from_user(dst + i * hw_stride, src + i * req->stride,
				   req->width))
			return -EFAULT;
		if (hw_stride > req->width)
			memset(dst + i * hw_stride + req->width, 0,
			       hw_stride - req->width);
	}

	/* Ensure coherent writes hit RAM before DMA fetches the Y plane. */
	dma_wmb();

	ret = pm_runtime_resume_and_get(fd->dev);
	if (ret < 0)
		return ret;

	threshold = req->threshold ? min_t(u32, req->threshold, 9) : 5;
	cond = qcom_fd_cond_value(req->min_face_size ? req->min_face_size : 20,
				  req->flags);

	writel(1, fd->core_base + FD_CORE_CONTROL);
	writel(0, fd->core_base + FD_CORE_CONTROL);

	writel(lower_32_bits(fd->image_dma), fd->core_base + FD_CORE_IMAGE_ADDR);
	/* FD501 errata requires RO mode and a valid result DMA address. */
	writel(lower_32_bits(fd->result_dma), fd->core_base + FD_CORE_RESULT_ADDR);
	writel(1, fd->core_base + FD_CORE_RO_MODE);
	writel(lower_32_bits(fd->work_dma), fd->core_base + FD_CORE_WORK_ADDR);

	writel(cond, fd->core_base + FD_CORE_COND);
	writel(0, fd->core_base + FD_CORE_START_X);
	writel(0, fd->core_base + FD_CORE_START_Y);
	writel(threshold, fd->core_base + FD_CORE_THRESHOLD);
	writel(size_mode, fd->core_base + FD_CORE_IMAGE_SIZE_MODE);
	writel(hw_stride, fd->core_base + FD_CORE_IMG_STRIDE);
	writel(req->width, fd->core_base + FD_CORE_IMG_WIDTH);
	writel(req->height, fd->core_base + FD_CORE_IMG_HEIGHT);

	reinit_completion(&fd->frame_done);
	writel(FD_IRQ_FRAME_DONE, fd->wrapper_base + FD_WRAPPER_IRQ_MASK);
	writel(2, fd->core_base + FD_CORE_CONTROL);

	if (!wait_for_completion_timeout(&fd->frame_done,
					 msecs_to_jiffies(FD_HW_TIMEOUT_MS))) {
		dev_err(fd->dev, "FD frame processing timeout\n");
		qcom_fd_hw_reset(fd);
		ret = -ETIMEDOUT;
		goto out_put;
	}

	/*
	 * FD501 errata leaves DDR RO-mode results unreliable; always read the
	 * face count and 4-word descriptors directly from core MMIO.
	 */
	face_cnt = readl(fd->core_base + FD_CORE_RESULT_CNT) & 0x3f;
	if (face_cnt > QCOM_FD_MAX_FACES)
		face_cnt = QCOM_FD_MAX_FACES;

	req->face_count = face_cnt;
	for (i = 0; i < face_cnt; i++) {
		u32 base = FD_CORE_RESULTS_BASE + i * 16;
		u32 w0 = readl(fd->core_base + base + 0);
		u32 w1 = readl(fd->core_base + base + 4);
		u32 w2 = readl(fd->core_base + base + 8);
		u32 w3 = readl(fd->core_base + base + 12);

		req->faces[i].center_x = w0 & 0x3ff;
		req->faces[i].center_y = w1 & 0x1ff;
		req->faces[i].size = w2 & 0x1ff;
		req->faces[i].confidence = ((w2 >> 9) & 0xf) * 100;
		req->faces[i].roll = w3 & 0x1ff;
		req->faces[i].pose = qcom_fd_decode_pose((w3 >> 9) & 0x7);
	}

	ret = 0;

out_put:
	pm_runtime_mark_last_busy(fd->dev);
	pm_runtime_put_autosuspend(fd->dev);
	return ret;
}

static long qcom_fd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct qcom_fd *fd = container_of(file->private_data,
					  struct qcom_fd, miscdev);
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case QCOM_FD_IOC_GET_CAPS: {
		struct qcom_fd_caps caps = {
			.core_version = fd->core_version,
			.wrapper_version = fd->wrapper_version,
			.max_faces = QCOM_FD_MAX_FACES,
		};

		if (copy_to_user(uarg, &caps, sizeof(caps)))
			return -EFAULT;
		return 0;
	}
	case QCOM_FD_IOC_DETECT: {
		struct qcom_fd_detect *req;
		int ret;

		req = kzalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		if (copy_from_user(req, uarg, sizeof(*req))) {
			kfree(req);
			return -EFAULT;
		}

		mutex_lock(&fd->lock);
		ret = qcom_fd_run_detect(fd, req);
		mutex_unlock(&fd->lock);

		if (!ret && copy_to_user(uarg, req, sizeof(*req)))
			ret = -EFAULT;

		kfree(req);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations qcom_fd_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = qcom_fd_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int qcom_fd_runtime_resume(struct device *dev)
{
	struct qcom_fd *fd = dev_get_drvdata(dev);
	int i, ret;

	ret = icc_set_bw(fd->icc_ahb, 38400, 76800);
	if (ret)
		return ret;

	ret = icc_set_bw(fd->icc_sf_mem, 524288, 1048576);
	if (ret)
		goto err_icc_ahb;

	ret = icc_set_bw(fd->icc_sf, 524288, 1048576);
	if (ret)
		goto err_icc_sf_mem;

	for (i = 0; i < ARRAY_SIZE(qcom_fd_clk_cfgs); i++) {
		if (qcom_fd_clk_cfgs[i].rate) {
			ret = clk_set_rate(fd->clks[i].clk, qcom_fd_clk_cfgs[i].rate);
			if (ret)
				goto err_icc_sf;
		}
	}

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(fd->clks), fd->clks);
	if (ret)
		goto err_icc_sf;

	ret = qcom_fd_hw_reset(fd);
	if (ret)
		goto err_clks;

	return 0;

err_clks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(fd->clks), fd->clks);
err_icc_sf:
	icc_set_bw(fd->icc_sf, 0, 0);
err_icc_sf_mem:
	icc_set_bw(fd->icc_sf_mem, 0, 0);
err_icc_ahb:
	icc_set_bw(fd->icc_ahb, 0, 0);
	return ret;
}

static int qcom_fd_runtime_suspend(struct device *dev)
{
	struct qcom_fd *fd = dev_get_drvdata(dev);

	writel(0, fd->wrapper_base + FD_WRAPPER_IRQ_MASK);
	clk_bulk_disable_unprepare(ARRAY_SIZE(fd->clks), fd->clks);
	icc_set_bw(fd->icc_sf, 0, 0);
	icc_set_bw(fd->icc_sf_mem, 0, 0);
	icc_set_bw(fd->icc_ahb, 0, 0);

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(qcom_fd_pm_ops, qcom_fd_runtime_suspend,
				 qcom_fd_runtime_resume, NULL);

static int qcom_fd_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_fd *fd;
	int i, ret;

	fd = devm_kzalloc(dev, sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return -ENOMEM;

	fd->dev = dev;
	mutex_init(&fd->lock);
	init_completion(&fd->frame_done);
	init_completion(&fd->halt_done);
	init_completion(&fd->reset_done);
	platform_set_drvdata(pdev, fd);

	fd->core_base = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(fd->core_base))
		return PTR_ERR(fd->core_base);

	fd->wrapper_base = devm_platform_ioremap_resource_byname(pdev, "wrapper");
	if (IS_ERR(fd->wrapper_base))
		return PTR_ERR(fd->wrapper_base);

	fd->irq = platform_get_irq(pdev, 0);
	if (fd->irq < 0)
		return fd->irq;

	for (i = 0; i < ARRAY_SIZE(qcom_fd_clk_cfgs); i++)
		fd->clks[i].id = qcom_fd_clk_cfgs[i].name;

	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(fd->clks), fd->clks);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get clocks\n");

	fd->icc_ahb = devm_of_icc_get(dev, "cam_ahb");
	if (IS_ERR(fd->icc_ahb))
		return dev_err_probe(dev, PTR_ERR(fd->icc_ahb),
				     "Failed to get cam_ahb interconnect\n");

	fd->icc_sf_mem = devm_of_icc_get(dev, "cam_sf_mem");
	if (IS_ERR(fd->icc_sf_mem))
		return dev_err_probe(dev, PTR_ERR(fd->icc_sf_mem),
				     "Failed to get cam_sf_mem interconnect\n");

	fd->icc_sf = devm_of_icc_get(dev, "cam_sf_0_mnoc");
	if (IS_ERR(fd->icc_sf))
		return dev_err_probe(dev, PTR_ERR(fd->icc_sf),
				     "Failed to get cam_sf_0_mnoc interconnect\n");

	/* FD501 DMA address registers are 32-bit. */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to set 32-bit DMA mask\n");

	fd->image_cpu = dmam_alloc_coherent(dev, FD_IMAGE_BUF_SIZE,
					    &fd->image_dma, GFP_KERNEL);
	if (!fd->image_cpu)
		return -ENOMEM;

	fd->work_cpu = dmam_alloc_coherent(dev, FD_WORK_BUF_SIZE,
					   &fd->work_dma, GFP_KERNEL);
	if (!fd->work_cpu)
		return -ENOMEM;

	fd->result_cpu = dmam_alloc_coherent(dev, FD_RESULT_BUF_SIZE,
					     &fd->result_dma, GFP_KERNEL);
	if (!fd->result_cpu)
		return -ENOMEM;

	ret = devm_request_irq(dev, fd->irq, qcom_fd_irq, 0,
			       dev_name(dev), fd);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ\n");

	pm_runtime_set_autosuspend_delay(dev, FD_AUTOSUSPEND_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto err_pm;

	fd->core_version = readl(fd->core_base + FD_CORE_VERSION);
	fd->wrapper_version = readl(fd->wrapper_base + FD_WRAPPER_VERSION);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	dev_info(dev, "Qualcomm FD hardware core %#x wrapper %#x (dma img=%pad work=%pad res=%pad)\n",
		 fd->core_version, fd->wrapper_version,
		 &fd->image_dma, &fd->work_dma, &fd->result_dma);

	fd->miscdev.minor = MISC_DYNAMIC_MINOR;
	fd->miscdev.name = "qcom-fd";
	fd->miscdev.fops = &qcom_fd_fops;
	fd->miscdev.parent = dev;

	ret = misc_register(&fd->miscdev);
	if (ret)
		goto err_pm;

	return 0;

err_pm:
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	return ret;
}

static void qcom_fd_remove(struct platform_device *pdev)
{
	struct qcom_fd *fd = platform_get_drvdata(pdev);

	misc_deregister(&fd->miscdev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	if (!pm_runtime_status_suspended(&pdev->dev))
		qcom_fd_runtime_suspend(&pdev->dev);
	pm_runtime_set_suspended(&pdev->dev);
}

static const struct of_device_id qcom_fd_of_match[] = {
	{ .compatible = "qcom,sm8150-fd" },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_fd_of_match);

static struct platform_driver qcom_fd_driver = {
	.probe = qcom_fd_probe,
	.remove = qcom_fd_remove,
	.driver = {
		.name = "qcom-fd",
		.of_match_table = qcom_fd_of_match,
		.pm = pm_ptr(&qcom_fd_pm_ops),
	},
};
module_platform_driver(qcom_fd_driver);

MODULE_DESCRIPTION("Qualcomm SM8150 Titan 175 Hardware Face Detection driver");
MODULE_LICENSE("GPL");
