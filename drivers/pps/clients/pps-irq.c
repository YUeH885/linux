// SPDX-License-Identifier: GPL-2.0-only
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pps_kernel.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "pps-irq-filter.h"

struct pps_irq_data {
	struct pps_device *pps;
	struct pps_irq_filter filter;
};

static irqreturn_t pps_irq_interrupt(int irq, void *data)
{
	struct pps_irq_data *pps = data;
	struct pps_event_time ts;

	pps_get_ts(&ts);
	if (pps_irq_filter_assert(&pps->filter, ktime_get_raw_ns()))
		pps_event(pps->pps, &ts, PPS_CAPTUREASSERT, NULL);
	return IRQ_HANDLED;
}

static void pps_irq_unregister(void *data)
{
	pps_unregister_source(data);
}

static int pps_irq_probe(struct platform_device *pdev)
{
	struct pps_source_info info = {
		.mode = PPS_CAPTUREASSERT | PPS_OFFSETASSERT | PPS_CANWAIT |
			PPS_TSFMT_TSPEC,
		.owner = THIS_MODULE,
		.dev = &pdev->dev,
	};
	struct pps_irq_data *pps;
	unsigned int type;
	int irq, ret;

	pps = devm_kzalloc(&pdev->dev, sizeof(*pps), GFP_KERNEL);
	if (!pps)
		return -ENOMEM;
	ret = device_property_read_u32(&pdev->dev, "assert-period-ns",
				       &pps->filter.period_ns);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "missing assert period\n");
	ret = device_property_read_u32(&pdev->dev, "max-jitter-ns",
				       &pps->filter.max_jitter_ns);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "missing assert jitter limit\n");
	if (!pps->filter.period_ns || !pps->filter.max_jitter_ns ||
	    2ULL * pps->filter.max_jitter_ns >= pps->filter.period_ns)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid assert period or jitter limit\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	type = irq_get_trigger_type(irq);
	if (type != IRQ_TYPE_EDGE_RISING && type != IRQ_TYPE_EDGE_FALLING)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "PPS requires a single edge interrupt\n");

	snprintf(info.name, sizeof(info.name), "%s", dev_name(&pdev->dev));
	pps->pps = pps_register_source(&info, PPS_CAPTUREASSERT | PPS_OFFSETASSERT);
	if (IS_ERR(pps->pps))
		return dev_err_probe(&pdev->dev, PTR_ERR(pps->pps),
				     "failed to register PPS source\n");

	ret = devm_add_action_or_reset(&pdev->dev, pps_irq_unregister, pps->pps);
	if (ret)
		return ret;

	/* Capture before scheduling, including kernels with forced IRQ threads. */
	ret = devm_request_irq(&pdev->dev, irq, pps_irq_interrupt,
			       type | IRQF_NO_THREAD, dev_name(&pdev->dev), pps);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request PPS interrupt\n");

	platform_set_drvdata(pdev, pps);
	dev_info(&pdev->dev, "PPS source %u on IRQ %d, period %u ns, jitter %u ns\n",
		 pps->pps->id, irq, pps->filter.period_ns,
		 pps->filter.max_jitter_ns);
	return 0;
}

static int pps_irq_resume(struct device *dev)
{
	struct pps_irq_data *pps = dev_get_drvdata(dev);

	/* Raw time excludes sleep; reset before interrupt delivery resumes. */
	pps_irq_filter_reset(&pps->filter);
	return 0;
}

static const struct dev_pm_ops pps_irq_pm_ops = {
	NOIRQ_SYSTEM_SLEEP_PM_OPS(NULL, pps_irq_resume)
};

static const struct of_device_id pps_irq_of_match[] = {
	{ .compatible = "pps-irq" },
	{ }
};
MODULE_DEVICE_TABLE(of, pps_irq_of_match);

static struct platform_driver pps_irq_driver = {
	.probe = pps_irq_probe,
	.driver = {
		.name = "pps-irq",
		.of_match_table = pps_irq_of_match,
		.pm = pm_sleep_ptr(&pps_irq_pm_ops),
	},
};
module_platform_driver(pps_irq_driver);

MODULE_DESCRIPTION("PPS client using an interrupt");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pps-irq");
