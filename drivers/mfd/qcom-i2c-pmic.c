// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define QCOM_I2C_PMIC_SUMMARY_BASE	0x0550
#define QCOM_I2C_PMIC_SET_TYPE		0x11
#define QCOM_I2C_PMIC_POL_HIGH		0x12
#define QCOM_I2C_PMIC_POL_LOW		0x13
#define QCOM_I2C_PMIC_LATCHED_CLEAR	0x14
#define QCOM_I2C_PMIC_ENABLE_SET		0x15
#define QCOM_I2C_PMIC_ENABLE_CLEAR	0x16
#define QCOM_I2C_PMIC_LATCHED_STATUS	0x18

enum qcom_i2c_pmic_irq_config {
	QCOM_I2C_PMIC_CFG_TYPE,
	QCOM_I2C_PMIC_CFG_POL_HIGH,
	QCOM_I2C_PMIC_CFG_POL_LOW,
	QCOM_I2C_PMIC_CFG_ENABLE,
	QCOM_I2C_PMIC_CFG_COUNT,
};

struct qcom_i2c_pmic;

struct qcom_i2c_pmic_periph {
	struct qcom_i2c_pmic *pmic;
	/* Serializes the software IRQ configuration and hardware updates. */
	struct mutex lock;
	u16 base;
	u8 desired[QCOM_I2C_PMIC_CFG_COUNT];
	u8 applied[QCOM_I2C_PMIC_CFG_COUNT];
	u8 wake_mask;
};

struct qcom_i2c_pmic {
	struct device *dev;
	struct regmap *regmap;
	struct pinctrl *pinctrl;
	struct irq_domain *domain;
	struct qcom_i2c_pmic_periph *periphs;
	/* Serializes summary handling with the suspend pending handshake. */
	struct mutex irq_lock;
	unsigned int num_periphs;
	int summary_irq;
	bool suspended;
	bool irq_pending;
};

static const unsigned int qcom_i2c_pmic_config_offsets[] = {
	[QCOM_I2C_PMIC_CFG_TYPE] = QCOM_I2C_PMIC_SET_TYPE,
	[QCOM_I2C_PMIC_CFG_POL_HIGH] = QCOM_I2C_PMIC_POL_HIGH,
	[QCOM_I2C_PMIC_CFG_POL_LOW] = QCOM_I2C_PMIC_POL_LOW,
	[QCOM_I2C_PMIC_CFG_ENABLE] = QCOM_I2C_PMIC_ENABLE_SET,
};

static struct qcom_i2c_pmic_periph *
qcom_i2c_pmic_find_periph(struct qcom_i2c_pmic *pmic,
			  irq_hw_number_t hwirq)
{
	u16 base = hwirq & GENMASK(15, 8);
	unsigned int i;

	for (i = 0; i < pmic->num_periphs; i++)
		if (pmic->periphs[i].base == base)
			return &pmic->periphs[i];

	return NULL;
}

static void qcom_i2c_pmic_irq_bus_lock(struct irq_data *data)
{
	struct qcom_i2c_pmic_periph *periph = irq_data_get_irq_chip_data(data);

	mutex_lock(&periph->lock);
}

static void qcom_i2c_pmic_sync_config(struct qcom_i2c_pmic_periph *periph,
				      enum qcom_i2c_pmic_irq_config config)
{
	struct qcom_i2c_pmic *pmic = periph->pmic;
	int ret;

	if (periph->desired[config] == periph->applied[config])
		return;

	ret = regmap_write(pmic->regmap,
			   periph->base | qcom_i2c_pmic_config_offsets[config],
			   periph->desired[config]);
	if (ret) {
		dev_err_ratelimited(pmic->dev,
				    "Failed to configure %#x IRQ register %#x: %d\n",
				    periph->base,
				    qcom_i2c_pmic_config_offsets[config], ret);
		return;
	}

	periph->applied[config] = periph->desired[config];
}

static void qcom_i2c_pmic_sync_enable(struct qcom_i2c_pmic_periph *periph)
{
	struct qcom_i2c_pmic *pmic = periph->pmic;
	u8 disable, enable;
	int ret;

	disable = periph->applied[QCOM_I2C_PMIC_CFG_ENABLE] &
		  ~periph->desired[QCOM_I2C_PMIC_CFG_ENABLE];
	enable = ~periph->applied[QCOM_I2C_PMIC_CFG_ENABLE] &
		 periph->desired[QCOM_I2C_PMIC_CFG_ENABLE];

	if (disable) {
		ret = regmap_write(pmic->regmap,
				   periph->base | QCOM_I2C_PMIC_ENABLE_CLEAR,
				   disable);
		if (ret) {
			dev_err_ratelimited(pmic->dev,
					    "Failed to disable %#x IRQs %#x: %d\n",
					    periph->base, disable, ret);
			return;
		}
	}

	if (enable) {
		ret = regmap_write(pmic->regmap,
				   periph->base | QCOM_I2C_PMIC_ENABLE_SET,
				   enable);
		if (ret) {
			dev_err_ratelimited(pmic->dev,
					    "Failed to enable %#x IRQs %#x: %d\n",
					    periph->base, enable, ret);
			return;
		}
	}

	periph->applied[QCOM_I2C_PMIC_CFG_ENABLE] =
		periph->desired[QCOM_I2C_PMIC_CFG_ENABLE];
}

static void qcom_i2c_pmic_irq_bus_sync_unlock(struct irq_data *data)
{
	struct qcom_i2c_pmic_periph *periph = irq_data_get_irq_chip_data(data);

	if (!READ_ONCE(periph->pmic->suspended)) {
		qcom_i2c_pmic_sync_config(periph, QCOM_I2C_PMIC_CFG_TYPE);
		qcom_i2c_pmic_sync_config(periph, QCOM_I2C_PMIC_CFG_POL_HIGH);
		qcom_i2c_pmic_sync_config(periph, QCOM_I2C_PMIC_CFG_POL_LOW);
		qcom_i2c_pmic_sync_enable(periph);
	}
	mutex_unlock(&periph->lock);
}

static void qcom_i2c_pmic_irq_mask(struct irq_data *data)
{
	struct qcom_i2c_pmic_periph *periph = irq_data_get_irq_chip_data(data);

	periph->desired[QCOM_I2C_PMIC_CFG_ENABLE] &= ~(u8)data->hwirq;
}

static void qcom_i2c_pmic_irq_unmask(struct irq_data *data)
{
	struct qcom_i2c_pmic_periph *periph = irq_data_get_irq_chip_data(data);

	periph->desired[QCOM_I2C_PMIC_CFG_ENABLE] |= (u8)data->hwirq;
}

static int qcom_i2c_pmic_irq_set_type(struct irq_data *data,
				      unsigned int type)
{
	struct qcom_i2c_pmic_periph *periph = irq_data_get_irq_chip_data(data);
	u8 mask = data->hwirq;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		periph->desired[QCOM_I2C_PMIC_CFG_TYPE] |= mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_HIGH] |= mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_LOW] &= ~mask;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		periph->desired[QCOM_I2C_PMIC_CFG_TYPE] |= mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_HIGH] &= ~mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_LOW] |= mask;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		periph->desired[QCOM_I2C_PMIC_CFG_TYPE] |= mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_HIGH] |= mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_LOW] |= mask;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		periph->desired[QCOM_I2C_PMIC_CFG_TYPE] &= ~mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_HIGH] |= mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_LOW] &= ~mask;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		periph->desired[QCOM_I2C_PMIC_CFG_TYPE] &= ~mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_HIGH] &= ~mask;
		periph->desired[QCOM_I2C_PMIC_CFG_POL_LOW] |= mask;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int qcom_i2c_pmic_irq_set_wake(struct irq_data *data, unsigned int on)
{
	struct qcom_i2c_pmic_periph *periph = irq_data_get_irq_chip_data(data);
	u8 mask = data->hwirq;

	if (on)
		periph->wake_mask |= mask;
	else
		periph->wake_mask &= ~mask;

	return 0;
}

static struct irq_chip qcom_i2c_pmic_irq_chip = {
	.name = "qcom-i2c-pmic",
	.irq_bus_lock = qcom_i2c_pmic_irq_bus_lock,
	.irq_bus_sync_unlock = qcom_i2c_pmic_irq_bus_sync_unlock,
	.irq_mask = qcom_i2c_pmic_irq_mask,
	.irq_unmask = qcom_i2c_pmic_irq_unmask,
	.irq_disable = qcom_i2c_pmic_irq_mask,
	.irq_enable = qcom_i2c_pmic_irq_unmask,
	.irq_set_type = qcom_i2c_pmic_irq_set_type,
	.irq_set_wake = qcom_i2c_pmic_irq_set_wake,
};

static int qcom_i2c_pmic_domain_map(struct irq_domain *domain,
				    unsigned int virq,
				    irq_hw_number_t hwirq)
{
	struct qcom_i2c_pmic *pmic = domain->host_data;
	struct qcom_i2c_pmic_periph *periph;

	periph = qcom_i2c_pmic_find_periph(pmic, hwirq);
	if (!periph || !is_power_of_2(hwirq & 0xff))
		return -EINVAL;

	irq_set_chip_data(virq, periph);
	irq_set_chip_and_handler(virq, &qcom_i2c_pmic_irq_chip,
				 handle_level_irq);
	irq_set_nested_thread(virq, true);
	irq_set_parent(virq, pmic->summary_irq);
	irq_set_noprobe(virq);

	return 0;
}

static void qcom_i2c_pmic_domain_unmap(struct irq_domain *domain,
				       unsigned int virq)
{
	irq_set_chip_and_handler(virq, NULL, NULL);
	irq_set_chip_data(virq, NULL);
}

static int qcom_i2c_pmic_domain_xlate(struct irq_domain *domain,
				      struct device_node *controller,
				      const u32 *intspec,
				      unsigned int intsize,
				      unsigned long *out_hwirq,
				      unsigned int *out_type)
{
	if (intsize != 3 || intspec[0] > 0xff || intspec[1] > 7 ||
	    intspec[2] > IRQ_TYPE_SENSE_MASK)
		return -EINVAL;

	*out_hwirq = intspec[0] << 8 | BIT(intspec[1]);
	*out_type = intspec[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static const struct irq_domain_ops qcom_i2c_pmic_domain_ops = {
	.map = qcom_i2c_pmic_domain_map,
	.unmap = qcom_i2c_pmic_domain_unmap,
	.xlate = qcom_i2c_pmic_domain_xlate,
};

static void qcom_i2c_pmic_remove_domain(void *data)
{
	irq_domain_remove(data);
}

static void qcom_i2c_pmic_disable_summary_wake(void *data)
{
	disable_irq_wake((unsigned long)data);
}

static int qcom_i2c_pmic_set_masks(struct qcom_i2c_pmic *pmic,
				   bool wake_only)
{
	unsigned int i;
	int ret;

	for (i = 0; i < pmic->num_periphs; i++) {
		struct qcom_i2c_pmic_periph *periph = &pmic->periphs[i];
		u8 mask;

		mutex_lock(&periph->lock);
		mask = wake_only ? periph->wake_mask :
			periph->desired[QCOM_I2C_PMIC_CFG_ENABLE];
		ret = regmap_write(pmic->regmap,
				   periph->base | QCOM_I2C_PMIC_ENABLE_CLEAR,
				   U8_MAX);
		if (ret) {
			mutex_unlock(&periph->lock);
			return ret;
		}
		if (!wake_only)
			periph->applied[QCOM_I2C_PMIC_CFG_ENABLE] = 0;
		if (!mask) {
			mutex_unlock(&periph->lock);
			continue;
		}
		ret = regmap_write(pmic->regmap,
				   periph->base | QCOM_I2C_PMIC_ENABLE_SET,
				   mask);
		if (ret) {
			mutex_unlock(&periph->lock);
			return ret;
		}
		if (!wake_only)
			periph->applied[QCOM_I2C_PMIC_CFG_ENABLE] = mask;
		mutex_unlock(&periph->lock);
	}

	return 0;
}

static void qcom_i2c_pmic_irq_disable_now(struct qcom_i2c_pmic *pmic,
						irq_hw_number_t hwirq)
{
	struct qcom_i2c_pmic_periph *periph;
	u8 mask = hwirq;
	int ret;

	periph = qcom_i2c_pmic_find_periph(pmic, hwirq);
	if (!periph)
		return;

	mutex_lock(&periph->lock);
	periph->desired[QCOM_I2C_PMIC_CFG_ENABLE] &= ~mask;
	ret = regmap_write(pmic->regmap,
			   periph->base | QCOM_I2C_PMIC_ENABLE_CLEAR, mask);
	if (ret)
		dev_err_ratelimited(pmic->dev,
				    "Failed to disable unmapped IRQ %#llx: %d\n",
				    (unsigned long long)hwirq, ret);
	else
		periph->applied[QCOM_I2C_PMIC_CFG_ENABLE] =
			periph->desired[QCOM_I2C_PMIC_CFG_ENABLE];
	mutex_unlock(&periph->lock);
}

static void qcom_i2c_pmic_handle_periph(struct qcom_i2c_pmic *pmic,
						struct qcom_i2c_pmic_periph *periph)
{
	unsigned int status;
	int ret;

	ret = regmap_read(pmic->regmap,
			  periph->base | QCOM_I2C_PMIC_LATCHED_STATUS,
			  &status);
	if (ret) {
		dev_err_ratelimited(pmic->dev,
				    "Failed to read %#x IRQ status: %d\n",
				    periph->base, ret);
		return;
	}

	while (status) {
		unsigned int bit = __ffs(status);
		irq_hw_number_t hwirq = periph->base | BIT(bit);
		unsigned int virq = irq_find_mapping(pmic->domain, hwirq);

		status &= ~BIT(bit);

		if (!virq) {
			dev_err_ratelimited(pmic->dev,
					    "No mapping for IRQ %#llx; disabling it\n",
					    (unsigned long long)hwirq);
			qcom_i2c_pmic_irq_disable_now(pmic, hwirq);
		} else {
			handle_nested_irq(virq);
		}

		ret = regmap_write(pmic->regmap,
				   periph->base | QCOM_I2C_PMIC_LATCHED_CLEAR,
				   BIT(bit));
		if (ret)
			dev_err_ratelimited(pmic->dev,
					    "Failed to acknowledge IRQ %#llx: %d\n",
					    (unsigned long long)hwirq, ret);
	}
}

static irqreturn_t qcom_i2c_pmic_irq_thread(int irq, void *data)
{
	struct qcom_i2c_pmic *pmic = data;
	unsigned int i, summary;
	int ret;

	mutex_lock(&pmic->irq_lock);
	if (pmic->suspended) {
		pmic->irq_pending = true;
		disable_irq_nosync(irq);
		mutex_unlock(&pmic->irq_lock);
		return IRQ_HANDLED;
	}

	for (i = 0; i < pmic->num_periphs; i++) {
		if (!(i % 8)) {
			ret = regmap_read(pmic->regmap,
					  QCOM_I2C_PMIC_SUMMARY_BASE + i / 8,
					  &summary);
			if (ret) {
				dev_err_ratelimited(pmic->dev,
						    "Failed to read IRQ summary: %d\n",
						    ret);
				break;
			}
		}
		if (!(summary & BIT(i % 8)))
			continue;
		qcom_i2c_pmic_handle_periph(pmic, &pmic->periphs[i]);
	}
	mutex_unlock(&pmic->irq_lock);

	return IRQ_HANDLED;
}

static int qcom_i2c_pmic_parse_periphs(struct qcom_i2c_pmic *pmic)
{
	struct device_node *node = pmic->dev->of_node;
	int count, i, j, ret;
	u32 address;

	count = of_property_count_u32_elems(node, "qcom,periph-map");
	if (count <= 0)
		return count ?: -EINVAL;
	if (count > 32)
		return -E2BIG;

	pmic->periphs = devm_kcalloc(pmic->dev, count,
				     sizeof(*pmic->periphs), GFP_KERNEL);
	if (!pmic->periphs)
		return -ENOMEM;
	pmic->num_periphs = count;

	for (i = 0; i < count; i++) {
		ret = of_property_read_u32_index(node, "qcom,periph-map", i,
						 &address);
		if (ret)
			return ret;
		if (address > 0xff)
			return -ERANGE;
		for (j = 0; j < i; j++)
			if (pmic->periphs[j].base == address << 8)
				return -EINVAL;

		pmic->periphs[i].pmic = pmic;
		pmic->periphs[i].base = address << 8;
		mutex_init(&pmic->periphs[i].lock);
	}

	return 0;
}

static int qcom_i2c_pmic_init_irqs(struct qcom_i2c_pmic *pmic)
{
	unsigned int i, config, value;
	int ret;

	for (i = 0; i < pmic->num_periphs; i++) {
		struct qcom_i2c_pmic_periph *periph = &pmic->periphs[i];

		for (config = 0; config < QCOM_I2C_PMIC_CFG_COUNT; config++) {
			ret = regmap_read(pmic->regmap,
					  periph->base |
					  qcom_i2c_pmic_config_offsets[config],
					  &value);
			if (ret)
				return ret;
			periph->desired[config] = value;
			periph->applied[config] = value;
		}
	}

	return 0;
}

static int qcom_i2c_pmic_suspend(struct device *dev)
{
	struct qcom_i2c_pmic *pmic = dev_get_drvdata(dev);
	bool pending;
	int ret, restore_ret;

	mutex_lock(&pmic->irq_lock);
	pmic->suspended = true;
	mutex_unlock(&pmic->irq_lock);

	ret = qcom_i2c_pmic_set_masks(pmic, true);
	if (ret) {
		restore_ret = qcom_i2c_pmic_set_masks(pmic, false);
		if (restore_ret)
			dev_err(pmic->dev,
				"Failed to restore IRQ masks after suspend error: %d\n",
				restore_ret);
		mutex_lock(&pmic->irq_lock);
		pmic->suspended = false;
		pending = pmic->irq_pending;
		pmic->irq_pending = false;
		mutex_unlock(&pmic->irq_lock);
		if (pending) {
			qcom_i2c_pmic_irq_thread(pmic->summary_irq, pmic);
			enable_irq(pmic->summary_irq);
		}
	}

	return ret;
}

static int qcom_i2c_pmic_suspend_noirq(struct device *dev)
{
	struct qcom_i2c_pmic *pmic = dev_get_drvdata(dev);

	return READ_ONCE(pmic->irq_pending) ? -EBUSY : 0;
}

static int qcom_i2c_pmic_resume(struct device *dev)
{
	struct qcom_i2c_pmic *pmic = dev_get_drvdata(dev);
	bool pending;
	int ret;

	ret = qcom_i2c_pmic_set_masks(pmic, false);

	mutex_lock(&pmic->irq_lock);
	pmic->suspended = false;
	pending = pmic->irq_pending;
	pmic->irq_pending = false;
	mutex_unlock(&pmic->irq_lock);

	if (pending) {
		qcom_i2c_pmic_irq_thread(pmic->summary_irq, pmic);
		enable_irq(pmic->summary_irq);
	}

	return ret;
}

static const struct dev_pm_ops qcom_i2c_pmic_pm_ops = {
	.suspend = qcom_i2c_pmic_suspend,
	.suspend_noirq = qcom_i2c_pmic_suspend_noirq,
	.resume = qcom_i2c_pmic_resume,
};

static const struct regmap_config qcom_i2c_pmic_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

static int qcom_i2c_pmic_probe(struct i2c_client *client)
{
	struct qcom_i2c_pmic *pmic;
	int ret;

	if (client->irq <= 0)
		return dev_err_probe(&client->dev, -EINVAL,
				     "Summary IRQ is required\n");

	pmic = devm_kzalloc(&client->dev, sizeof(*pmic), GFP_KERNEL);
	if (!pmic)
		return -ENOMEM;

	pmic->dev = &client->dev;
	pmic->summary_irq = client->irq;
	mutex_init(&pmic->irq_lock);
	i2c_set_clientdata(client, pmic);

	pmic->regmap = devm_regmap_init_i2c(client,
					    &qcom_i2c_pmic_regmap_config);
	if (IS_ERR(pmic->regmap))
		return dev_err_probe(pmic->dev, PTR_ERR(pmic->regmap),
				     "Failed to initialize regmap\n");

	ret = qcom_i2c_pmic_parse_periphs(pmic);
	if (ret)
		return dev_err_probe(pmic->dev, ret,
				     "Invalid peripheral map\n");
	ret = qcom_i2c_pmic_init_irqs(pmic);
	if (ret)
		return dev_err_probe(pmic->dev, ret,
				     "Failed to initialize interrupts\n");
	if (device_property_present(pmic->dev, "pinctrl-names")) {
		pmic->pinctrl = devm_pinctrl_get_select_default(pmic->dev);
		if (IS_ERR(pmic->pinctrl))
			return dev_err_probe(pmic->dev, PTR_ERR(pmic->pinctrl),
					     "Failed to select default pinctrl\n");
	}

	pmic->domain = irq_domain_create_tree(dev_fwnode(pmic->dev),
					      &qcom_i2c_pmic_domain_ops, pmic);
	if (!pmic->domain)
		return -ENOMEM;
	ret = devm_add_action_or_reset(pmic->dev,
				       qcom_i2c_pmic_remove_domain, pmic->domain);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(pmic->dev, pmic->summary_irq, NULL,
					qcom_i2c_pmic_irq_thread,
					IRQF_ONESHOT | IRQF_SHARED,
					dev_name(pmic->dev), pmic);
	if (ret)
		return dev_err_probe(pmic->dev, ret,
				     "Failed to request summary IRQ\n");

	ret = devm_device_init_wakeup(pmic->dev);
	if (ret)
		return ret;
	ret = enable_irq_wake(pmic->summary_irq);
	if (ret)
		return dev_err_probe(pmic->dev, ret,
				     "Failed to enable summary IRQ wake\n");
	ret = devm_add_action_or_reset(pmic->dev,
				       qcom_i2c_pmic_disable_summary_wake,
				       (void *)(unsigned long)pmic->summary_irq);
	if (ret)
		return ret;

	ret = devm_of_platform_populate(pmic->dev);
	if (ret)
		return dev_err_probe(pmic->dev, ret,
				     "Failed to populate PMIC children\n");

	return 0;
}

static const struct of_device_id qcom_i2c_pmic_of_match[] = {
	{ .compatible = "qcom,i2c-pmic" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_i2c_pmic_of_match);

static struct i2c_driver qcom_i2c_pmic_driver = {
	.probe = qcom_i2c_pmic_probe,
	.driver = {
		.name = "qcom-i2c-pmic",
		.of_match_table = qcom_i2c_pmic_of_match,
		.pm = pm_sleep_ptr(&qcom_i2c_pmic_pm_ops),
	},
};
module_i2c_driver(qcom_i2c_pmic_driver);

MODULE_DESCRIPTION("Qualcomm I2C PMIC core and interrupt controller");
MODULE_LICENSE("GPL");
