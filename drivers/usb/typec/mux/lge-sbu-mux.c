// SPDX-License-Identifier: GPL-2.0
/*
 * LGE Alpha USB-C SBU switch
 *
 * The Alpha board routes the SBU pins through GPIO87.  GPIO35 selects the
 * UART/debug path.  GPIO42 enables the DP AUX switch that connects the SBU
 * pins to the DP controller and GPIO148 selects its polarity.  Some board
 * revisions also provide a separate output enable GPIO; it is optional because
 * the GPIO119 assignment is revision-dependent.  The factory/ADC LDO path is
 * intentionally not represented here.
 */

#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>

struct lge_sbu_mux {
	struct gpio_desc *select_gpio;
	struct gpio_desc *uart_select_gpio;
	struct gpio_desc *oe_gpio;
	struct gpio_desc *aux_enable_gpio;
	struct gpio_desc *aux_select_gpio;
	bool reverse;

	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
	struct mutex lock;

	enum typec_orientation orientation;
	unsigned long mode;
	u16 svid;
};

static void lge_sbu_aux_disable(struct lge_sbu_mux *sbu)
{
	if (sbu->aux_enable_gpio)
		gpiod_set_value_cansleep(sbu->aux_enable_gpio, 0);
}

static int lge_sbu_set_safe(struct lge_sbu_mux *sbu)
{
	int ret = 0;

	lge_sbu_aux_disable(sbu);

	if (sbu->oe_gpio)
		gpiod_set_value_cansleep(sbu->oe_gpio, 1);
	else
		ret = gpiod_direction_input(sbu->select_gpio);

	if (sbu->uart_select_gpio)
		gpiod_set_value_cansleep(sbu->uart_select_gpio, 0);

	return ret;
}

static int lge_sbu_apply(struct lge_sbu_mux *sbu)
{
	bool aux = false;
	bool uart = false;
	bool select = false;
	int ret;

	/* SAFE and USB are common states, including when an altmode is active. */
	if (sbu->mode == TYPEC_STATE_SAFE || sbu->mode == TYPEC_STATE_USB) {
		goto safe;
	} else if (sbu->svid == USB_TYPEC_DP_SID) {
		switch (sbu->mode) {
		case TYPEC_DP_STATE_C:
		case TYPEC_DP_STATE_D:
		case TYPEC_DP_STATE_E:
		case TYPEC_DP_STATE_F:
			/* The Alpha SBU path is selected for DP AUX. */
			aux = true;
			break;
		default:
			ret = lge_sbu_set_safe(sbu);
			return ret ? ret : -EOPNOTSUPP;
		}
	} else {
		switch (sbu->mode) {
		case TYPEC_STATE_SAFE:
		case TYPEC_STATE_USB:
		case TYPEC_MODE_USB2:
		case TYPEC_MODE_USB3:
		case TYPEC_MODE_USB4:
			/* Normal USB operation leaves the SBU switch disconnected. */
			goto safe;
		case TYPEC_MODE_DEBUG:
			uart = true;
			break;
		default:
			ret = lge_sbu_set_safe(sbu);
			return ret ? ret : -EOPNOTSUPP;
		}
	}

	if (sbu->orientation == TYPEC_ORIENTATION_NONE)
		goto safe;

	if (sbu->reverse)
		select = !select;

	if (sbu->oe_gpio)
		gpiod_set_value_cansleep(sbu->oe_gpio, 1);
	else {
		ret = gpiod_direction_output(sbu->select_gpio, select);
		if (ret)
			return ret;
	}

	if (sbu->uart_select_gpio)
		gpiod_set_value_cansleep(sbu->uart_select_gpio, uart);
	gpiod_set_value_cansleep(sbu->select_gpio, select);

	if (sbu->oe_gpio)
		gpiod_set_value_cansleep(sbu->oe_gpio, 0);

	/* Route and enable the DP AUX switch only once SBU is connected. */
	if (sbu->aux_select_gpio)
		gpiod_set_value_cansleep(sbu->aux_select_gpio,
					 sbu->orientation == TYPEC_ORIENTATION_REVERSE);
	if (sbu->aux_enable_gpio)
		gpiod_set_value_cansleep(sbu->aux_enable_gpio, aux);

	return 0;

safe:
	return lge_sbu_set_safe(sbu);
}

static int lge_sbu_switch_set(struct typec_switch_dev *sw,
				      enum typec_orientation orientation)
{
	struct lge_sbu_mux *sbu = typec_switch_get_drvdata(sw);
	int ret;

	mutex_lock(&sbu->lock);
	sbu->orientation = orientation;
	ret = lge_sbu_apply(sbu);
	mutex_unlock(&sbu->lock);

	return ret;
}

static int lge_sbu_mux_set(struct typec_mux_dev *mux,
				   struct typec_mux_state *state)
{
	struct lge_sbu_mux *sbu = typec_mux_get_drvdata(mux);
	int ret;

	mutex_lock(&sbu->lock);
	sbu->mode = state->mode;
	sbu->svid = state->alt ? state->alt->svid : 0;
	ret = lge_sbu_apply(sbu);
	mutex_unlock(&sbu->lock);

	return ret;
}

static int lge_sbu_mux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_mux_desc mux_desc = { };
	struct lge_sbu_mux *sbu;
	int ret;

	sbu = devm_kzalloc(dev, sizeof(*sbu), GFP_KERNEL);
	if (!sbu)
		return -ENOMEM;

	mutex_init(&sbu->lock);
	sbu->select_gpio = devm_gpiod_get(dev, "select", GPIOD_OUT_LOW);
	if (IS_ERR(sbu->select_gpio))
		return dev_err_probe(dev, PTR_ERR(sbu->select_gpio),
				     "failed to get select GPIO\n");

	sbu->uart_select_gpio = devm_gpiod_get_optional(dev, "uart-sel",
							GPIOD_OUT_LOW);
	if (IS_ERR(sbu->uart_select_gpio))
		return dev_err_probe(dev, PTR_ERR(sbu->uart_select_gpio),
				     "failed to get UART select GPIO\n");

	sbu->oe_gpio = devm_gpiod_get_optional(dev, "oe", GPIOD_OUT_HIGH);
	if (IS_ERR(sbu->oe_gpio))
		return dev_err_probe(dev, PTR_ERR(sbu->oe_gpio),
				     "failed to get output-enable GPIO\n");

	sbu->aux_enable_gpio = devm_gpiod_get_optional(dev, "aux-enable",
						       GPIOD_OUT_LOW);
	if (IS_ERR(sbu->aux_enable_gpio))
		return dev_err_probe(dev, PTR_ERR(sbu->aux_enable_gpio),
				     "failed to get DP AUX enable GPIO\n");

	sbu->aux_select_gpio = devm_gpiod_get_optional(dev, "aux-select",
						       GPIOD_OUT_LOW);
	if (IS_ERR(sbu->aux_select_gpio))
		return dev_err_probe(dev, PTR_ERR(sbu->aux_select_gpio),
				     "failed to get DP AUX select GPIO\n");

	sbu->reverse = device_property_read_bool(dev, "lg,reverse-sbu-switch");
	sbu->mode = TYPEC_STATE_SAFE;
	sbu->orientation = TYPEC_ORIENTATION_NONE;
	ret = lge_sbu_set_safe(sbu);
	if (ret)
		return ret;

	sw_desc.fwnode = dev_fwnode(dev);
	sw_desc.drvdata = sbu;
	sw_desc.set = lge_sbu_switch_set;
	sbu->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(sbu->sw))
		return dev_err_probe(dev, PTR_ERR(sbu->sw),
				     "failed to register Type-C switch\n");

	mux_desc.fwnode = dev_fwnode(dev);
	mux_desc.drvdata = sbu;
	mux_desc.set = lge_sbu_mux_set;
	sbu->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(sbu->mux)) {
		typec_switch_unregister(sbu->sw);
		return dev_err_probe(dev, PTR_ERR(sbu->mux),
				     "failed to register Type-C mux\n");
	}

	platform_set_drvdata(pdev, sbu);
	return 0;
}

static void lge_sbu_mux_remove(struct platform_device *pdev)
{
	struct lge_sbu_mux *sbu = platform_get_drvdata(pdev);

	mutex_lock(&sbu->lock);
	lge_sbu_set_safe(sbu);
	mutex_unlock(&sbu->lock);
	typec_mux_unregister(sbu->mux);
	typec_switch_unregister(sbu->sw);
}

static const struct of_device_id lge_sbu_mux_of_match[] = {
	{ .compatible = "lg,lge-sbu-mux" },
	{ }
};
MODULE_DEVICE_TABLE(of, lge_sbu_mux_of_match);

static struct platform_driver lge_sbu_mux_driver = {
	.probe = lge_sbu_mux_probe,
	.remove = lge_sbu_mux_remove,
	.driver = {
		.name = "lge-sbu-mux",
		.of_match_table = lge_sbu_mux_of_match,
	},
};
module_platform_driver(lge_sbu_mux_driver);

MODULE_DESCRIPTION("LGE Alpha USB-C SBU mux driver");
MODULE_LICENSE("GPL");
