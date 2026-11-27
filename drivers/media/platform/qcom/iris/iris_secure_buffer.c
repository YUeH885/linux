// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dma-mapping.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/genalloc.h>
#include <linux/iommu.h>
#include <linux/of_platform.h>
#include <dt-bindings/firmware/qcom,scm.h>

#include "iris_core.h"
#include "iris_secure_buffer.h"

struct iris_cp_context {
	struct iommu_domain *domain;
	struct gen_pool *pool;
	struct mutex lock; /* Protects the allocation list across sessions. */
	struct list_head buffers;
};

struct iris_secure_buffer {
	struct list_head list;
	void *pages;
	size_t size;
	u64 owner;
	unsigned long iova;
};

static int iris_secure_buffer_reclaim(struct iris_core *core,
				      struct iris_secure_buffer *secure)
{
	struct qcom_scm_vmperm perm = {
		QCOM_SCM_VMID_HLOS, QCOM_SCM_PERM_RWX,
	};
	int ret;

	ret = qcom_scm_assign_mem(virt_to_phys(secure->pages), secure->size,
				  &secure->owner, &perm, 1);
	if (ret)
		dev_err(core->dev, "failed to reclaim protected ARP buffer: %d\n", ret);

	return ret;
}

int iris_secure_buffer_alloc(struct iris_core *core, struct iris_buffer *buffer)
{
	struct qcom_scm_vmperm perm = {
		QCOM_SCM_VMID_CP_NON_PIXEL, QCOM_SCM_PERM_RW,
	};
	struct iris_cp_context *context = core->cp_context;
	struct iris_secure_buffer *secure;
	dma_addr_t dma;
	unsigned long iova;
	int ret;

	if (!context) {
		dev_err(core->dev, "encoding requires a protected non-pixel context\n");
		return -EOPNOTSUPP;
	}

	secure = kzalloc_obj(*secure);
	if (!secure)
		return -ENOMEM;

	secure->size = PAGE_ALIGN(buffer->buffer_size);
	secure->pages = alloc_pages_exact(secure->size, GFP_KERNEL | __GFP_ZERO);
	if (!secure->pages) {
		dev_err(core->dev, "failed to allocate %zu-byte protected ARP buffer\n",
			secure->size);
		ret = -ENOMEM;
		goto free_secure;
	}

	iova = gen_pool_alloc(context->pool, secure->size);
	if (!iova) {
		dev_err(core->dev, "protected ARP IOVA allocation failed\n");
		ret = -ENOMEM;
		goto free_pages;
	}
	secure->iova = iova;

	/* Clean CPU cache lines before handing exclusive ownership to CP. */
	dma = dma_map_single(core->dev, secure->pages, secure->size, DMA_TO_DEVICE);
	if (dma_mapping_error(core->dev, dma)) {
		ret = -EIO;
		goto free_iova;
	}
	dma_unmap_single(core->dev, dma, secure->size, DMA_TO_DEVICE);

	secure->owner = BIT_ULL(QCOM_SCM_VMID_HLOS);
	ret = qcom_scm_assign_mem(virt_to_phys(secure->pages), secure->size,
				  &secure->owner, &perm, 1);
	if (ret) {
		dev_err(core->dev, "failed to protect ARP buffer: %d\n", ret);
		goto free_iova;
	}
	mutex_lock(&context->lock);
	list_add_tail(&secure->list, &context->buffers);
	mutex_unlock(&context->lock);

	ret = iommu_map(context->domain, iova, virt_to_phys(secure->pages),
			secure->size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		dev_err(core->dev, "failed to map protected ARP buffer: %d\n", ret);
		if (iris_secure_buffer_reclaim(core, secure))
			return ret;
		mutex_lock(&context->lock);
		list_del(&secure->list);
		mutex_unlock(&context->lock);
		goto free_iova;
	}

	buffer->secure = secure;
	buffer->device_addr = iova;
	return 0;

free_iova:
	gen_pool_free(context->pool, iova, secure->size);
free_pages:
	free_pages_exact(secure->pages, secure->size);
free_secure:
	kfree(secure);
	return ret;
}

void iris_secure_buffer_free(struct iris_core *core, struct iris_buffer *buffer)
{
	struct iris_secure_buffer *secure = buffer->secure;
	struct iris_cp_context *context = core->cp_context;
	int ret;

	if (iommu_unmap(context->domain, buffer->device_addr, secure->size) != secure->size) {
		dev_err(core->dev, "failed to unmap protected ARP buffer\n");
		return;
	}

	ret = iris_secure_buffer_reclaim(core, secure);
	if (ret)
		return;
	mutex_lock(&context->lock);
	list_del(&secure->list);
	mutex_unlock(&context->lock);

	gen_pool_free(context->pool, buffer->device_addr, secure->size);
	memzero_explicit(secure->pages, secure->size);
	free_pages_exact(secure->pages, secure->size);
	kfree(secure);
}

static int iris_cp_probe(struct platform_device *pdev)
{
	const struct tz_cp_config *config = sm8150_data.tz_cp_config_data;
	struct iris_cp_context *context;
	int ret;

	context = devm_kzalloc(&pdev->dev, sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	mutex_init(&context->lock);
	INIT_LIST_HEAD(&context->buffers);

	context->pool = gen_pool_create(PAGE_SHIFT, -1);
	if (!context->pool)
		return -ENOMEM;

	ret = gen_pool_add(context->pool, config->cp_nonpixel_start,
			   config->cp_nonpixel_size, -1);
	if (ret)
		goto destroy_pool;

	context->domain = iommu_paging_domain_alloc(&pdev->dev);
	if (IS_ERR(context->domain)) {
		ret = PTR_ERR(context->domain);
		goto destroy_pool;
	}

	ret = iommu_attach_device(context->domain, &pdev->dev);
	if (ret)
		goto free_domain;

	platform_set_drvdata(pdev, context);
	return 0;

free_domain:
	iommu_domain_free(context->domain);
destroy_pool:
	gen_pool_destroy(context->pool);
	return ret;
}

static void iris_cp_remove(struct platform_device *pdev)
{
	struct iris_cp_context *context = platform_get_drvdata(pdev);
	struct iris_secure_buffer *secure, *next;

	iommu_detach_device(context->domain, &pdev->dev);
	iommu_domain_free(context->domain);
	/* Quarantine pages whose CP ownership could not be reclaimed. */
	list_for_each_entry_safe(secure, next, &context->buffers, list) {
		gen_pool_free(context->pool, secure->iova, secure->size);
		list_del(&secure->list);
		kfree(secure);
	}
	gen_pool_destroy(context->pool);
	mutex_destroy(&context->lock);
}

static const struct of_device_id iris_cp_match[] = {
	{ .compatible = "qcom,sm8150-iris-cp-nonpixel" },
	{ }
};

struct platform_driver iris_cp_driver = {
	.probe = iris_cp_probe,
	.remove = iris_cp_remove,
	.driver = {
		.name = "iris-cp-nonpixel",
		.of_match_table = iris_cp_match,
		.suppress_bind_attrs = true,
	},
};

static void iris_cp_unregister(void *data)
{
	struct platform_device *pdev = data;

	of_platform_device_destroy(&pdev->dev, NULL);
}

int iris_secure_buffer_init(struct iris_core *core)
{
	struct platform_device *pdev;
	int ret;

	if (!core->iris_platform_data->secure_arp)
		return 0;

	struct device_node *node __free(device_node) =
		of_get_child_by_name(core->dev->of_node, "cp-nonpixel");
	if (!node)
		return -ENODEV;

	pdev = of_platform_device_create(node, NULL, core->dev);
	if (!pdev)
		return -ENOMEM;

	ret = devm_add_action_or_reset(core->dev, iris_cp_unregister, pdev);
	if (ret)
		return ret;

	core->cp_context = platform_get_drvdata(pdev);
	return core->cp_context ? 0 : -EPROBE_DEFER;
}
