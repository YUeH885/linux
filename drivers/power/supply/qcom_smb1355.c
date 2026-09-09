// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
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
#define SMB1355_DISABLE_CHARGING		BIT(3)
#define SMB1355_CHG_EN_SOURCE		BIT(7)
#define SMB1355_CHG_EN_POLARITY		BIT(6)
#define SMB1355_TEMP_RST_HOT		BIT(2)
#define SMB1355_TEMP_UB_HOT		BIT(1)
#define SMB1355_TEMP_LB_HOT		BIT(0)
#define SMB1355_WDOG_ENABLE		BIT(0)
#define SMB1355_WDOG_DISABLE_CHARGING	BIT(7)
#define SMB1355_FCC_MAX_UA			6000000
#define SMB1355_FCC_STEP_UA		25000
#define SMB1355_FV_MIN_UV			2450000
#define SMB1355_FV_MAX_UV			5000000
#define SMB1355_FV_STEP_UV			10000
#define SMB1355_WDOG_PET_INTERVAL_MS	5000
#define SMB1355_POLICY_TIMEOUT_MS		20000

struct smb1355 {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct delayed_work watchdog_work;
	/* Covers complete transitions, secure writes and property access. */
	struct mutex lock;
	bool enabled;
	bool stopping;
	unsigned long policy_deadline;
};

static const struct regmap_config smb1355_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
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
		*health = POWER_SUPPLY_HEALTH_GOOD;
	return 0;
}

static int smb1355_disable(struct smb1355 *chip)
{
	unsigned int cfg, cmd;
	int ret;

	/* Never stop the watchdog until the charge gate is confirmed closed. */
	chip->enabled = false;
	cancel_delayed_work(&chip->watchdog_work);
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
		chip->policy_deadline = jiffies + msecs_to_jiffies(SMB1355_POLICY_TIMEOUT_MS);
		return 0;
	}
	ret = smb1355_read_health(chip, &health);
	if (ret)
		return ret;
	if (health == POWER_SUPPLY_HEALTH_OVERHEAT)
		return -EAGAIN;
	ret = regmap_write(chip->regmap, SMB1355_WDOG_PET_REG, BIT(0));
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				  SMB1355_WDOG_ENABLE, SMB1355_WDOG_ENABLE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_BANDGAP_REG, BIT(0), BIT(0));
	if (ret)
		goto disable;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_CFG2_REG,
				  SMB1355_CHG_EN_SOURCE | SMB1355_CHG_EN_POLARITY,
				  SMB1355_CHG_EN_SOURCE);
	if (ret)
		goto disable;
	chip->enabled = true;
	chip->policy_deadline = jiffies + msecs_to_jiffies(SMB1355_POLICY_TIMEOUT_MS);
	mod_delayed_work(system_dfl_wq, &chip->watchdog_work,
			 msecs_to_jiffies(SMB1355_WDOG_PET_INTERVAL_MS));
	return 0;

disable:
	cleanup_ret = smb1355_disable(chip);
	if (cleanup_ret)
		dev_err(chip->dev, "Failed to close parallel charge gate: %d\n", cleanup_ret);
	return ret;
}

static void smb1355_watchdog_work(struct work_struct *work)
{
	struct smb1355 *chip = container_of(to_delayed_work(work),
					    struct smb1355, watchdog_work);
	int health, ret, disable_ret;

	mutex_lock(&chip->lock);
	if (!chip->enabled || chip->stopping)
		goto out;
	/* A live secondary worker must not mask a stalled main charging policy. */
	ret = time_after_eq(jiffies, chip->policy_deadline) ? -ETIMEDOUT :
		smb1355_read_health(chip, &health);
	if (!ret && health == POWER_SUPPLY_HEALTH_OVERHEAT)
		ret = -EOVERFLOW;
	if (!ret)
		ret = regmap_write(chip->regmap, SMB1355_WDOG_PET_REG, BIT(0));
	if (ret) {
		/* Only the main policy may enable the secondary path again. */
		disable_ret = smb1355_disable(chip);
		dev_err_ratelimited(chip->dev, "Parallel monitor failed: %d, disable: %d\n",
				    ret, disable_ret);
	} else {
		mod_delayed_work(system_dfl_wq, &chip->watchdog_work,
				 msecs_to_jiffies(SMB1355_WDOG_PET_INTERVAL_MS));
	}
out:
	mutex_unlock(&chip->lock);
	power_supply_changed(chip->psy);
}

static int smb1355_hw_init(struct smb1355 *chip)
{
	static const struct reg_sequence settings[] = {
		{ SMB1355_FCC_REG, 0 },
		/* Bark after 16 s, bite 8 s later; bite disables charging. */
		{ SMB1355_WDOG_TIMEOUT_REG, SMB1355_WDOG_DISABLE_CHARGING | 3 },
	};
	static const struct { unsigned int reg, mask, value; } config[] = {
		{ SMB1355_I2C_SID_REG, GENMASK(3, 0), 1 },
		{ SMB1355_HICCUP_REG, GENMASK(5, 0), 0 },
		{ SMB1355_CHGR_CFG_REG, BIT(0), BIT(0) },
		{ SMB1355_PRE_FAST_REG, GENMASK(2, 0), 0 },
		/* Fixed 90 C lower comparator threshold, rather than scanning it. */
		{ SMB1355_DIE_TEMP_REG, GENMASK(5, 0), 90 - 34 },
		/* Enable hardware die mitigation; clear software bypass. */
		{ SMB1355_THERMREG_REG, BIT(2) | BIT(0), BIT(0) },
		{ SMB1355_SPARE1_REG, BIT(1), BIT(1) },
		{ SMB1355_VALLEY_COMP_REG, BIT(0), BIT(0) },
		{ SMB1355_ILIMIT_CFG_REG, BIT(3), BIT(3) },
		{ SMB1355_SPARE1_REG, BIT(2), BIT(2) },
		{ SMB1355_BATID_REG, BIT(2), BIT(2) },
		{ SMB1355_RBIAS_REG, BIT(2), BIT(2) },
		{ SMB1355_THERMREG_REG, BIT(1), BIT(1) },
	};
	int i, ret;

	ret = smb1355_update_bits(chip, SMB1355_CLOCK_REQUEST_REG, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = smb1355_disable(chip);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(chip->regmap, settings, ARRAY_SIZE(settings));
	if (ret)
		return ret;
	/* No IRQ controller is available; poll and let hardware bite on starvation. */
	ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				  BIT(7) | BIT(6) | BIT(5) | BIT(1) | BIT(0), 0);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(config); i++) {
		ret = smb1355_update_bits(chip, config[i].reg, config[i].mask,
					  config[i].value);
		if (ret)
			return ret;
	}
	return 0;
}

static enum power_supply_property smb1355_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
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
		ret = regmap_read(chip->regmap, SMB1355_CHGR_STATUS_REG, &status);
		if (ret)
			break;
		ret = regmap_read(chip->regmap, SMB1355_BATTERY_STATUS_2_REG, &pin);
		if (!ret)
			val->intval = (status & SMB1355_CHARGING_ENABLED) &&
				!(pin & SMB1355_DISABLE_CHARGING) ?
				POWER_SUPPLY_STATUS_CHARGING : POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		/* Gate state is distinct from transient ESR pause or taper. */
		ret = regmap_read(chip->regmap, SMB1355_CHGR_CFG2_REG, &raw);
		if (!ret)
			val->intval = chip->enabled && (raw & SMB1355_CHG_EN_SOURCE);
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		ret = smb1355_read_health(chip, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = regmap_read(chip->regmap, SMB1355_FCC_REG, &raw);
		if (!ret)
			val->intval = raw * SMB1355_FCC_STEP_UA;
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = regmap_read(chip->regmap, SMB1355_BAT_OV_REG, &raw);
		if (!ret)
			val->intval = SMB1355_FV_MIN_UV + raw * SMB1355_FV_STEP_UV;
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
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		if (val->intval < SMB1355_FV_MIN_UV || val->intval > SMB1355_FV_MAX_UV) {
			ret = -EINVAL;
			break;
		}
		ret = regmap_write(chip->regmap, SMB1355_BAT_OV_REG,
				   (val->intval - SMB1355_FV_MIN_UV) / SMB1355_FV_STEP_UV);
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

static const struct power_supply_desc smb1355_desc = {
	.name = "smb1355-parallel",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = smb1355_properties,
	.num_properties = ARRAY_SIZE(smb1355_properties),
	.get_property = smb1355_get_property,
	.set_property = smb1355_set_property,
};

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
	cancel_delayed_work_sync(&chip->watchdog_work);
	if (ret)
		dev_err(chip->dev, "Failed to stop parallel charger: %d\n", ret);
}

static int smb1355_probe(struct i2c_client *client)
{
	struct power_supply_config config = {};
	struct smb1355 *chip;
	unsigned int id;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &client->dev;
	mutex_init(&chip->lock);
	INIT_DELAYED_WORK(&chip->watchdog_work, smb1355_watchdog_work);
	i2c_set_clientdata(client, chip);
	chip->regmap = devm_regmap_init_i2c(client, &smb1355_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(chip->dev, PTR_ERR(chip->regmap), "No regmap\n");
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
	return devm_add_action_or_reset(chip->dev, smb1355_stop, chip);
}

static void smb1355_shutdown(struct i2c_client *client)
{
	smb1355_stop(i2c_get_clientdata(client));
}

static const struct of_device_id smb1355_of_match[] = {
	{ .compatible = "qcom,smb1355" },
	{}
};
MODULE_DEVICE_TABLE(of, smb1355_of_match);

static struct i2c_driver smb1355_driver = {
	.driver = {
		.name = "qcom-smb1355",
		.of_match_table = smb1355_of_match,
	},
	.probe = smb1355_probe,
	.shutdown = smb1355_shutdown,
};
module_i2c_driver(smb1355_driver);

MODULE_DESCRIPTION("Qualcomm SMB1355 parallel charger driver");
MODULE_LICENSE("GPL");
