// SPDX-License-Identifier: GPL-2.0-only
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pps_kernel.h>

static irqreturn_t pps_irq_interrupt(int irq, void *data)
{
	struct pps_event_time ts;

	pps_get_ts(&ts);
	pps_event(data, &ts, PPS_CAPTUREASSERT, NULL);
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
	struct pps_device *pps;
	unsigned int type;
	int irq, ret;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	type = irq_get_trigger_type(irq);
	if (type != IRQ_TYPE_EDGE_RISING && type != IRQ_TYPE_EDGE_FALLING)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "PPS requires a single edge interrupt\n");

	snprintf(info.name, sizeof(info.name), "%s", dev_name(&pdev->dev));
	pps = pps_register_source(&info, PPS_CAPTUREASSERT | PPS_OFFSETASSERT);
	if (IS_ERR(pps))
		return dev_err_probe(&pdev->dev, PTR_ERR(pps),
				     "failed to register PPS source\n");

	ret = devm_add_action_or_reset(&pdev->dev, pps_irq_unregister, pps);
	if (ret)
		return ret;

	/* Capture before scheduling, including kernels with forced IRQ threads. */
	ret = devm_request_irq(&pdev->dev, irq, pps_irq_interrupt,
			       type | IRQF_NO_THREAD, dev_name(&pdev->dev), pps);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to request PPS interrupt\n");

	dev_info(&pdev->dev, "PPS source %u on IRQ %d\n", pps->id, irq);
	return 0;
}

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
	},
};
module_platform_driver(pps_irq_driver);

MODULE_DESCRIPTION("PPS client using an interrupt");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:pps-irq");
