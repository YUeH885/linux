// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define SMB1355_MFG_ID_REG		0x01ff
#define SMB1355_I2C_SID_REG		0x0e45
#define SMB1355_BATTERY_STATUS_2_REG	0x100b
#define SMB1355_CHGR_STATUS_REG		0x100c
#define SMB1355_CHGR_ENABLE_CMD_REG	0x1042
#define SMB1355_CHGR_CFG2_REG		0x1051
#define SMB1355_CHGR_CFG_REG		0x1053
#define SMB1355_FCC_REG			0x1061
#define SMB1355_BAT_OV_REG			0x1070
#define SMB1355_PRE_FAST_REG		0x1074
#define SMB1355_HICCUP_REG			0x1272
#define SMB1355_BATID_REG			0x1273
#define SMB1355_DIE_TEMP_REG		0x12c0
#define SMB1355_SPARE1_REG			0x12c2
#define SMB1355_VALLEY_COMP_REG		0x14c1
#define SMB1355_TEMP_STATUS_REG		0x1607
#define SMB1355_BANDGAP_REG			0x1642
#define SMB1355_WDOG_PET_REG		0x1643
#define SMB1355_CLOCK_REQUEST_REG		0x1644
#define SMB1355_WDOG_CFG_REG		0x1651
#define SMB1355_WDOG_TIMEOUT_REG		0x1653
#define SMB1355_RBIAS_REG			0x1655
#define SMB1355_THERMREG_REG		0x1670
#define SMB1355_ILIMIT_CFG_REG		0x16a1

#define SMB1355_CHARGING_ENABLED		BIT(3)
#define SMB1355_FAST_CHARGE		BIT(4)
#define SMB1355_DISABLE_CHARGING		BIT(3)
#define SMB1355_CHG_EN_SOURCE		BIT(7)
#define SMB1355_CHG_EN_POLARITY		BIT(6)
#define SMB1355_TEMP_RST_HOT		BIT(2)
#define SMB1355_TEMP_UB_HOT		BIT(1)
#define SMB1355_TEMP_LB_HOT		BIT(0)
#define SMB1355_WDOG_ENABLE		BIT(0)
#define SMB1355_WDOG_BARK_IRQ_ENABLE	BIT(6)
#define SMB1355_WDOG_BITE_IRQ_ENABLE	BIT(5)
#define SMB1355_WDOG_DISABLE_CHARGING	BIT(7)
#define SMB1355_WDOG_TIMEOUT_DEFAULT	(SMB1355_WDOG_DISABLE_CHARGING | 3)
#define SMB1355_FCC_MAX_UA			6000000
#define SMB1355_FCC_STEP_UA		25000
#define SMB1355_FV_MIN_UV			2450000
#define SMB1355_FV_MAX_UV			5000000
#define SMB1355_FV_STEP_UV			10000
#define SMB1355_DIE_TEMP_BASE_DEGC	34
#define SMB1355_DIE_TEMP_RANGE_DEGC	16
#define SMB1355_DIE_TEMP_MAX_DEGC	97
#define SMB1355_DIE_TEMP_SETTLE_MS	100
#define SMB1355_DIE_TEMP_PERIOD_MS	10000
#define SMB1355_DEFAULT_DIE_TEMP_DEGC	90

struct smb1355 {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	/* Covers complete transitions, secure writes and property access. */
	struct mutex lock;
	int health;
	int status;
	int charge_type;
	int fcc_ua;
	int fv_uv;
	int die_temp_decic;
	unsigned int die_temp_threshold;
	struct delayed_work die_temp_work;
	bool enabled;
	bool suspended;
	bool stopping;
	bool exit_die_temp;
	bool hw_die_temp_mitigation;
};

static int smb1355_update_bits(struct smb1355 *chip, unsigned int reg,
			       unsigned int mask, unsigned int value)
{
	int ret;

	lockdep_assert_held(&chip->lock);
	if (reg == SMB1355_CLOCK_REQUEST_REG || reg == SMB1355_I2C_SID_REG ||
	    (reg & 0xff) >= 0xa0) {
		ret = regmap_write(chip->regmap, (reg & 0xff00) | 0xd0, 0xa5);
		if (ret)
			return ret;
	}
	return regmap_update_bits(chip->regmap, reg, mask, value);
}

static int smb1355_read_health(struct smb1355 *chip, int *health)
{
	unsigned int status;
	int ret;

	ret = regmap_read(chip->regmap, SMB1355_TEMP_STATUS_REG, &status);
	if (ret)
		return ret;
	if (status & SMB1355_TEMP_RST_HOT)
		*health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (status & SMB1355_TEMP_UB_HOT)
		*health = POWER_SUPPLY_HEALTH_HOT;
	else if (status & SMB1355_TEMP_LB_HOT)
		*health = POWER_SUPPLY_HEALTH_WARM;
	else
		*health = POWER_SUPPLY_HEALTH_COOL;
	return 0;
}

static void smb1355_die_temp_work(struct work_struct *work)
{
	struct smb1355 *chip = container_of(to_delayed_work(work),
						struct smb1355, die_temp_work);
	unsigned int status;
	int i, ret;

	for (i = 0; i <= SMB1355_DIE_TEMP_MAX_DEGC -
				 SMB1355_DIE_TEMP_BASE_DEGC; i++) {
		mutex_lock(&chip->lock);
		if (chip->exit_die_temp || !chip->enabled || chip->suspended) {
			mutex_unlock(&chip->lock);
			return;
		}
		ret = smb1355_update_bits(chip, SMB1355_DIE_TEMP_REG,
					  GENMASK(5, 0), i);
		mutex_unlock(&chip->lock);
		if (ret)
			continue;

		msleep(SMB1355_DIE_TEMP_SETTLE_MS);

		mutex_lock(&chip->lock);
		if (chip->exit_die_temp || !chip->enabled || chip->suspended) {
			mutex_unlock(&chip->lock);
			return;
		}
		ret = regmap_read(chip->regmap, SMB1355_TEMP_STATUS_REG, &status);
		mutex_unlock(&chip->lock);
		if (ret)
			continue;
		if (!(status & SMB1355_TEMP_UB_HOT))
			break;
	}

	mutex_lock(&chip->lock);
	chip->die_temp_decic = min(SMB1355_DIE_TEMP_BASE_DEGC + i,
				   SMB1355_DIE_TEMP_MAX_DEGC) * 10;
	if (!chip->exit_die_temp && chip->enabled && !chip->suspended)
		schedule_delayed_work(&chip->die_temp_work,
				      msecs_to_jiffies(SMB1355_DIE_TEMP_PERIOD_MS));
	mutex_unlock(&chip->lock);
}

static int smb1355_disable(struct smb1355 *chip)
{
	unsigned int cfg, cmd;
	int ret;

	chip->exit_die_temp = true;
	chip->die_temp_decic = -EINVAL;
	cancel_delayed_work(&chip->die_temp_work);

	/* Keep the watchdog armed until both software charge gates are closed. */
	ret = smb1355_update_bits(chip, SMB1355_CHGR_ENABLE_CMD_REG, BIT(0), 0);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_CFG2_REG,
				  SMB1355_CHG_EN_SOURCE | SMB1355_CHG_EN_POLARITY, 0);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, SMB1355_CHGR_CFG2_REG, &cfg);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, SMB1355_CHGR_ENABLE_CMD_REG, &cmd);
	if (ret)
		return ret;
	if ((cfg & SMB1355_CHG_EN_SOURCE) || (cmd & BIT(0)))
		return -EIO;

	chip->enabled = false;
	ret = smb1355_update_bits(chip, SMB1355_BANDGAP_REG, BIT(0), 0);
	if (ret)
		return ret;
	return smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				   SMB1355_WDOG_ENABLE, 0);
}

static int smb1355_enable(struct smb1355 *chip)
{
	int health, ret, cleanup_ret;

	if (chip->enabled) {
		/* Policy renewals must keep feeding the armed watchdog. */
		ret = regmap_write(chip->regmap, SMB1355_WDOG_PET_REG, BIT(0));
		if (ret)
			return ret;
		return 0;
	}
	ret = smb1355_read_health(chip, &health);
	if (ret)
		return ret;
	chip->health = health;
	if (health == POWER_SUPPLY_HEALTH_OVERHEAT)
		return -EAGAIN;
	ret = regmap_write(chip->regmap, SMB1355_WDOG_PET_REG, BIT(0));
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				  SMB1355_WDOG_ENABLE, SMB1355_WDOG_ENABLE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_CFG2_REG,
				  SMB1355_CHG_EN_SOURCE | SMB1355_CHG_EN_POLARITY,
				  SMB1355_CHG_EN_SOURCE);
	if (ret)
		goto disable;
	ret = smb1355_update_bits(chip, SMB1355_BANDGAP_REG, BIT(0), BIT(0));
	if (ret)
		goto disable;
	chip->enabled = true;
	chip->exit_die_temp = false;
	if (!chip->hw_die_temp_mitigation)
		schedule_delayed_work(&chip->die_temp_work, 0);
	return 0;

disable:
	cleanup_ret = smb1355_disable(chip);
	if (cleanup_ret)
		dev_err(chip->dev, "Failed to close parallel charge gate: %d\n", cleanup_ret);
	return ret;
}

static void smb1355_parse_dt(struct smb1355 *chip)
{
	unsigned int value;

	chip->hw_die_temp_mitigation =
		device_property_read_bool(chip->dev, "qcom,hw-die-temp-mitigation");
	chip->die_temp_threshold = SMB1355_DEFAULT_DIE_TEMP_DEGC;
	if (!device_property_read_u32(chip->dev, "qcom,die-temp-threshold-degc",
				      &value))
		chip->die_temp_threshold = clamp_t(unsigned int, value,
					   SMB1355_DIE_TEMP_BASE_DEGC,
					   SMB1355_DIE_TEMP_MAX_DEGC);
}

static int smb1355_hw_init(struct smb1355 *chip)
{
	static const struct reg_sequence settings[] = {
		{ SMB1355_FCC_REG, 0 },
		/* Bark after 16 s, bite 8 s later; bite disables charging. */
		{ SMB1355_WDOG_TIMEOUT_REG, SMB1355_WDOG_TIMEOUT_DEFAULT },
	};
	static const struct { unsigned int reg, mask, value; } config[] = {
		{ SMB1355_I2C_SID_REG, GENMASK(3, 0), 1 },
		{ SMB1355_HICCUP_REG, GENMASK(5, 0), 0 },
		{ SMB1355_CHGR_CFG_REG, BIT(0), BIT(0) },
		{ SMB1355_PRE_FAST_REG, GENMASK(2, 0), 0 },
		{ SMB1355_VALLEY_COMP_REG, BIT(0), BIT(0) },
		{ SMB1355_ILIMIT_CFG_REG, BIT(3), BIT(3) },
		{ SMB1355_SPARE1_REG, BIT(2), BIT(2) },
		{ SMB1355_BATID_REG, BIT(2), BIT(2) },
		{ SMB1355_RBIAS_REG, BIT(2), BIT(2) },
		{ SMB1355_THERMREG_REG, BIT(1), BIT(1) },
	};
	unsigned int range, value;
	int i, ret;

	ret = smb1355_update_bits(chip, SMB1355_CLOCK_REQUEST_REG, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_ENABLE_CMD_REG, BIT(0), 0);
	if (ret)
		return ret;
	ret = smb1355_disable(chip);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(chip->regmap, settings, ARRAY_SIZE(settings));
	if (ret)
		return ret;
	/* Keep bark and bite reporting armed while charging enables the timer. */
	ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				  BIT(6) | BIT(5) | BIT(1) | BIT(0),
				  SMB1355_WDOG_BARK_IRQ_ENABLE |
				  SMB1355_WDOG_BITE_IRQ_ENABLE);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = smb1355_update_bits(chip, config[i].reg, config[i].mask,
					  config[i].value);
		if (ret)
			return ret;
	}
	if (chip->hw_die_temp_mitigation) {
		range = (chip->die_temp_threshold - SMB1355_DIE_TEMP_BASE_DEGC) /
			 SMB1355_DIE_TEMP_RANGE_DEGC;
		value = (range << 4) |
			(chip->die_temp_threshold -
			 ((range * SMB1355_DIE_TEMP_RANGE_DEGC) +
			  SMB1355_DIE_TEMP_BASE_DEGC)) % SMB1355_DIE_TEMP_RANGE_DEGC;
		ret = smb1355_update_bits(chip, SMB1355_DIE_TEMP_REG,
					  GENMASK(5, 0), value);
		if (ret)
			return ret;
	}
	ret = smb1355_update_bits(chip, SMB1355_THERMREG_REG,
				  BIT(2) | BIT(0),
				  BIT(0) | (chip->hw_die_temp_mitigation ? 0 : BIT(2)));
	if (ret)
		return ret;
	return smb1355_update_bits(chip, SMB1355_SPARE1_REG, BIT(1),
				   chip->hw_die_temp_mitigation ? BIT(1) : 0);
}

static enum power_supply_property smb1355_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_TEMP,
};

static int smb1355_get_property(struct power_supply *psy,
				enum power_supply_property prop,
				union power_supply_propval *val)
{
	struct smb1355 *chip = power_supply_get_drvdata(psy);
	unsigned int status, pin, raw;
	int ret = 0;

	mutex_lock(&chip->lock);
	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		if (chip->suspended) {
			val->intval = chip->status;
			break;
		}
		ret = regmap_read(chip->regmap, SMB1355_CHGR_STATUS_REG, &status);
		if (ret)
			break;
		ret = regmap_read(chip->regmap, SMB1355_BATTERY_STATUS_2_REG, &pin);
		if (!ret) {
			val->intval = (status & SMB1355_CHARGING_ENABLED) &&
				!(pin & SMB1355_DISABLE_CHARGING) ?
				POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_NOT_CHARGING;
			chip->status = val->intval;
		}
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (chip->suspended) {
			val->intval = chip->charge_type;
			break;
		}
		ret = regmap_read(chip->regmap, SMB1355_CHGR_STATUS_REG, &status);
		if (!ret) {
			if (!(status & SMB1355_CHARGING_ENABLED))
				val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
			else if (status & SMB1355_FAST_CHARGE)
				val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
			else
				val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
			chip->charge_type = val->intval;
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		if (chip->suspended) {
			val->intval = chip->enabled;
			break;
		}
		/* The software gate remains online during transient charge pauses. */
		ret = regmap_read(chip->regmap, SMB1355_CHGR_CFG2_REG, &raw);
		if (!ret)
			val->intval = chip->enabled && (raw & SMB1355_CHG_EN_SOURCE);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (chip->suspended) {
			val->intval = chip->health;
			break;
		}
		ret = smb1355_read_health(chip, &val->intval);
		if (!ret)
			chip->health = val->intval;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (chip->suspended) {
			val->intval = chip->fcc_ua;
			break;
		}
		ret = regmap_read(chip->regmap, SMB1355_FCC_REG, &raw);
		if (!ret) {
			val->intval = raw * SMB1355_FCC_STEP_UA;
			chip->fcc_ua = val->intval;
		}
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (chip->suspended) {
			val->intval = chip->fv_uv;
			break;
		}
		ret = regmap_read(chip->regmap, SMB1355_BAT_OV_REG, &raw);
		if (!ret) {
			val->intval = SMB1355_FV_MIN_UV + raw * SMB1355_FV_STEP_UV;
			chip->fv_uv = val->intval;
		}
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = chip->die_temp_decic;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&chip->lock);
	return ret;
}

static int smb1355_set_property(struct power_supply *psy,
				enum power_supply_property prop,
				const union power_supply_propval *val)
{
	struct smb1355 *chip = power_supply_get_drvdata(psy);
	int ret;

	mutex_lock(&chip->lock);
	if (chip->stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (chip->suspended) {
		ret = -EBUSY;
		goto out;
	}
	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = val->intval ? smb1355_enable(chip) : smb1355_disable(chip);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		if (val->intval < 0 || val->intval > SMB1355_FCC_MAX_UA) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_write(chip->regmap, SMB1355_FCC_REG,
				   val->intval / SMB1355_FCC_STEP_UA);
		if (!ret)
			chip->fcc_ua = val->intval;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		if (val->intval < SMB1355_FV_MIN_UV || val->intval > SMB1355_FV_MAX_UV) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_write(chip->regmap, SMB1355_BAT_OV_REG,
				   (val->intval - SMB1355_FV_MIN_UV) / SMB1355_FV_STEP_UV);
		if (!ret)
			chip->fv_uv = val->intval;
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&chip->lock);
	if (!ret)
		power_supply_changed(psy);
	return ret;
}

static int smb1355_property_is_writeable(struct power_supply *psy,
					 enum power_supply_property prop)
{
	return prop == POWER_SUPPLY_PROP_ONLINE ||
	       prop == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX ||
	       prop == POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX ||
	       prop == POWER_SUPPLY_PROP_VOLTAGE_MAX;
}

static const struct power_supply_desc smb1355_desc = {
	.name = "parallel",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = smb1355_properties,
	.num_properties = ARRAY_SIZE(smb1355_properties),
	.get_property = smb1355_get_property,
	.set_property = smb1355_set_property,
	.property_is_writeable = smb1355_property_is_writeable,
};

static irqreturn_t smb1355_changed_irq(int irq, void *data)
{
	struct smb1355 *chip = data;

	power_supply_changed(chip->psy);

	return IRQ_HANDLED;
}

static irqreturn_t smb1355_wdog_bark_irq(int irq, void *data)
{
	struct smb1355 *chip = data;
	int ret;

	mutex_lock(&chip->lock);
	ret = regmap_write(chip->regmap, SMB1355_WDOG_PET_REG, BIT(0));
	mutex_unlock(&chip->lock);
	if (ret)
		dev_err_ratelimited(chip->dev,
				    "Failed to pet parallel watchdog: %d\n", ret);

	return IRQ_HANDLED;
}

static void smb1355_disable_irq_wake(void *data)
{
	disable_irq_wake((unsigned long)data);
}

static int smb1355_request_irq(struct platform_device *pdev, const char *name,
			       irq_handler_t handler, bool wake)
{
	int irq, ret;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0)
		return irq;
	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL, handler,
					IRQF_ONESHOT, name,
					platform_get_drvdata(pdev));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to request %s IRQ\n", name);
	if (!wake)
		return 0;

	ret = enable_irq_wake(irq);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to enable %s IRQ wake\n", name);

	return devm_add_action_or_reset(&pdev->dev,
					smb1355_disable_irq_wake,
					(void *)(unsigned long)irq);
}

static void smb1355_stop(void *data)
{
	struct smb1355 *chip = data;
	int ret;

	mutex_lock(&chip->lock);
	chip->stopping = true;
	ret = smb1355_disable(chip);
	if (!ret)
		ret = smb1355_update_bits(chip, SMB1355_CLOCK_REQUEST_REG, BIT(0), 0);
	mutex_unlock(&chip->lock);
	cancel_delayed_work_sync(&chip->die_temp_work);
	if (ret)
		dev_err(chip->dev, "Failed to stop parallel charger: %d\n", ret);
}

static int smb1355_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct smb1355 *chip;
	unsigned int id;
	int ret;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &pdev->dev;
	mutex_init(&chip->lock);
	INIT_DELAYED_WORK(&chip->die_temp_work, smb1355_die_temp_work);
	chip->die_temp_decic = -EINVAL;
	platform_set_drvdata(pdev, chip);
	smb1355_parse_dt(chip);
	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV, "No parent regmap\n");
	ret = regmap_read(chip->regmap, SMB1355_MFG_ID_REG, &id);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to read chip ID\n");
	if (id != 0xff)
		return dev_err_probe(chip->dev, -ENODEV, "Unexpected chip ID: %#x\n", id);
	mutex_lock(&chip->lock);
	ret = smb1355_hw_init(chip);
	mutex_unlock(&chip->lock);
	if (ret) {
		smb1355_stop(chip);
		return dev_err_probe(chip->dev, ret, "Failed to initialize charger\n");
	}

	config.drv_data = chip;
	config.fwnode = dev_fwnode(chip->dev);
	chip->psy = devm_power_supply_register(chip->dev, &smb1355_desc, &config);
	if (IS_ERR(chip->psy)) {
		smb1355_stop(chip);
		return dev_err_probe(chip->dev, PTR_ERR(chip->psy), "No power supply\n");
	}
	ret = devm_add_action_or_reset(chip->dev, smb1355_stop, chip);
	if (ret)
		return ret;

	ret = devm_device_init_wakeup(chip->dev);
	if (ret)
		return ret;
	ret = smb1355_request_irq(pdev, "wdog-bark",
				  smb1355_wdog_bark_irq, true);
	if (ret)
		return ret;
	ret = smb1355_request_irq(pdev, "chg-state-change",
				  smb1355_changed_irq, true);
	if (ret)
		return ret;

	return smb1355_request_irq(pdev, "temperature-change",
				   smb1355_changed_irq, false);
}

static void smb1355_shutdown(struct platform_device *pdev)
{
	smb1355_stop(platform_get_drvdata(pdev));
}

static int smb1355_suspend(struct device *dev)
{
	struct smb1355 *chip = dev_get_drvdata(dev);

	mutex_lock(&chip->lock);
	chip->suspended = true;
	chip->exit_die_temp = true;
	mutex_unlock(&chip->lock);
	cancel_delayed_work_sync(&chip->die_temp_work);

	return 0;
}

static int smb1355_resume(struct device *dev)
{
	struct smb1355 *chip = dev_get_drvdata(dev);

	mutex_lock(&chip->lock);
	chip->suspended = false;
	if (chip->enabled && !chip->hw_die_temp_mitigation) {
		chip->exit_die_temp = false;
		schedule_delayed_work(&chip->die_temp_work, 0);
	}
	mutex_unlock(&chip->lock);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(smb1355_pm_ops,
				smb1355_suspend, smb1355_resume);

static const struct of_device_id smb1355_of_match[] = {
	{ .compatible = "qcom,smb1355" },
	{}
};
MODULE_DEVICE_TABLE(of, smb1355_of_match);

static struct platform_driver smb1355_driver = {
	.driver = {
		.name = "qcom-smb1355",
		.of_match_table = smb1355_of_match,
		.pm = pm_sleep_ptr(&smb1355_pm_ops),
	},
	.probe = smb1355_probe,
	.shutdown = smb1355_shutdown,
};
module_platform_driver(smb1355_driver);

MODULE_DESCRIPTION("Qualcomm SMB1355 parallel charger driver");
MODULE_LICENSE("GPL");
