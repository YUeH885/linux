// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define SMB1355_MFG_ID_REG			0x01ff
#define SMB1355_MFG_ID				0xff
#define SMB1355_I2C_SID_REG			0x0e45
#define SMB1355_BATTERY_STATUS_2_REG		0x100b
#define SMB1355_CHGR_STATUS_REG			0x100c
#define SMB1355_CHGR_ENABLE_CMD_REG		0x1042
#define SMB1355_CHGR_CFG2_REG			0x1051
#define SMB1355_CHGR_CFG_REG			0x1053
#define SMB1355_FCC_REG				0x1061
#define SMB1355_BAT_OV_REG			0x1070
#define SMB1355_PRE_FAST_REG			0x1074
#define SMB1355_HICCUP_REG			0x1272
#define SMB1355_BATID_REG			0x1273
#define SMB1355_DIE_TEMP_REG			0x12c0
#define SMB1355_SPARE1_REG			0x12c2
#define SMB1355_VALLEY_COMP_REG			0x14c1
#define SMB1355_TEMP_STATUS_REG			0x1607
#define SMB1355_BANDGAP_REG			0x1642
#define SMB1355_WDOG_PET_REG			0x1643
#define SMB1355_CLOCK_REQUEST_REG		0x1644
#define SMB1355_WDOG_CFG_REG			0x1651
#define SMB1355_WDOG_TIMEOUT_REG		0x1653
#define SMB1355_RBIAS_REG			0x1655
#define SMB1355_THERMREG_REG			0x1670
#define SMB1355_ILIMIT_CFG_REG			0x16a1

#define SMB1355_CHARGING_ENABLED		BIT(3)
#define SMB1355_FAST_CHARGE			BIT(4)
#define SMB1355_DISABLE_CHARGING		BIT(3)
#define SMB1355_CHG_EN_SOURCE			BIT(7)
#define SMB1355_CHG_EN_POLARITY			BIT(6)
#define SMB1355_PARALLEL_SENSE			BIT(0)
#define SMB1355_BANDGAP_ENABLE			BIT(0)
#define SMB1355_WDOG_ENABLE			BIT(0)
#define SMB1355_WDOG_PET			BIT(0)
#define SMB1355_WDOG_DISABLE_CHARGING		BIT(7)
#define SMB1355_CLOCK_REQUEST			BIT(0)
#define SMB1355_EXT_BIAS_ENABLE			BIT(2)
#define SMB1355_DIE_TEMP_HYST			BIT(1)
#define SMB1355_RBIAS_ENABLE			BIT(2)
#define SMB1355_SKIN_TEMP_ENABLE		BIT(1)
#define SMB1355_DIE_TEMP_BYPASS			BIT(2)
#define SMB1355_DIE_TEMP_ENABLE			BIT(0)
#define SMB1355_TEMP_RST_HOT			BIT(2)
#define SMB1355_TEMP_UB_HOT			BIT(1)
#define SMB1355_TEMP_LB_HOT			BIT(0)

#define SMB1355_FCC_MIN_UA			0
#define SMB1355_FCC_MAX_UA			6000000
#define SMB1355_FCC_STEP_UA			25000
#define SMB1355_FV_MIN_UV			2450000
#define SMB1355_FV_MAX_UV			5000000
#define SMB1355_FV_STEP_UV			10000
#define SMB1355_DIE_TEMP_BASE_DECIC		340
#define SMB1355_DIE_TEMP_PERIOD_MS		10000
#define SMB1355_RETRY_DELAY_MS			3000
#define SMB1355_RETRY_COUNT			4
#define SMB1355_WDOG_PET_INTERVAL_MS		1000

struct smb1355 {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct delayed_work die_temp_work;
	struct delayed_work retry_work;
	struct delayed_work watchdog_work;
	/* Serialize secure access unlock and the following register update. */
	struct mutex lock;
	/* Serialize enable/disable sequences and failed-charge recovery. */
	struct mutex online_lock;
	int die_temp_decic;
	unsigned int retry_count;
	bool enabled;
	bool online_valid;
};

static const struct regmap_config smb1355_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

static bool smb1355_is_secure(unsigned int reg)
{
	return reg == SMB1355_CLOCK_REQUEST_REG ||
	       reg == SMB1355_I2C_SID_REG || (reg & 0xff) >= 0xa0;
}

static int smb1355_update_bits(struct smb1355 *chip, unsigned int reg,
			       unsigned int mask, unsigned int value)
{
	int ret;

	mutex_lock(&chip->lock);
	if (smb1355_is_secure(reg)) {
		ret = regmap_write(chip->regmap, (reg & 0xff00) | 0xd0, 0xa5);
		if (ret)
			goto out;
	}
	ret = regmap_update_bits(chip->regmap, reg, mask, value);
out:
	mutex_unlock(&chip->lock);

	return ret;
}

static int smb1355_write(struct smb1355 *chip, unsigned int reg,
			 unsigned int value)
{
	int ret;

	mutex_lock(&chip->lock);
	if (smb1355_is_secure(reg)) {
		ret = regmap_write(chip->regmap, (reg & 0xff00) | 0xd0, 0xa5);
		if (ret)
			goto out;
	}
	ret = regmap_write(chip->regmap, reg, value);
out:
	mutex_unlock(&chip->lock);

	return ret;
}

static int smb1355_set_param(struct smb1355 *chip, unsigned int reg,
			     int value, int min, int max, int step)
{
	if (value < min || value > max)
		return -EINVAL;

	return smb1355_write(chip, reg, (value - min) / step);
}

static int smb1355_get_param(struct smb1355 *chip, unsigned int reg,
			     int min, int max, int step, int *value)
{
	unsigned int raw;
	int ret;

	ret = regmap_read(chip->regmap, reg, &raw);
	if (!ret)
		*value = min + min_t(unsigned int, raw,
				     DIV_ROUND_DOWN_ULL(max - min, step)) * step;

	return ret;
}

static void smb1355_die_temp_work(struct work_struct *work)
{
	struct smb1355 *chip = container_of(to_delayed_work(work),
						struct smb1355, die_temp_work);
	unsigned int status;
	int i, ret;

	for (i = 0; i < BIT(5); i++) {
		if (!READ_ONCE(chip->enabled))
			return;

		ret = smb1355_update_bits(chip, SMB1355_DIE_TEMP_REG,
					  GENMASK(5, 0), i);
		if (ret)
			continue;

		/* The comparator output needs the factory 100 ms settling interval. */
		msleep(100);
		if (!READ_ONCE(chip->enabled))
			return;

		ret = regmap_read(chip->regmap, SMB1355_TEMP_STATUS_REG,
				  &status);
		if (ret)
			continue;
		if (!(status & SMB1355_TEMP_UB_HOT))
			break;
	}

	WRITE_ONCE(chip->die_temp_decic,
		   SMB1355_DIE_TEMP_BASE_DECIC + i * 10);
	if (READ_ONCE(chip->enabled))
		schedule_delayed_work(&chip->die_temp_work,
				      msecs_to_jiffies(SMB1355_DIE_TEMP_PERIOD_MS));
}

static int smb1355_charging_active(struct smb1355 *chip,
				   unsigned int *status, bool *active);
static int smb1355_set_online(struct smb1355 *chip, bool enable);

static int smb1355_read_temp_health(struct smb1355 *chip, int *health)
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

static void smb1355_watchdog_work(struct work_struct *work)
{
	struct smb1355 *chip = container_of(to_delayed_work(work),
						struct smb1355, watchdog_work);
	int ret;

	mutex_lock(&chip->online_lock);
	if (!chip->enabled)
		goto out;

	/*
	 * SMB1355 raises the bark through the i2c-pmic interrupt controller,
	 * which has no mainline driver, so pet the watchdog from a work item
	 * instead. If this work is starved the bite timer disables charging,
	 * which is the same failure handling as the bark handler.
	 */
	ret = smb1355_write(chip, SMB1355_WDOG_PET_REG, SMB1355_WDOG_PET);
	if (ret) {
		dev_warn_ratelimited(chip->dev, "Failed to pet SMB1355 watchdog: %d\n",
					 ret);
		/* The hardware bite will disable charging; keep software state conservative. */
		WRITE_ONCE(chip->enabled, false);
		WRITE_ONCE(chip->online_valid, false);
		WRITE_ONCE(chip->die_temp_decic, -EINVAL);
		chip->retry_count = 0;
		cancel_delayed_work(&chip->retry_work);
		cancel_delayed_work(&chip->die_temp_work);
		goto out;
	}
	mod_delayed_work(system_dfl_wq, &chip->watchdog_work,
			 msecs_to_jiffies(SMB1355_WDOG_PET_INTERVAL_MS));
out:
	mutex_unlock(&chip->online_lock);
}

static void smb1355_retry_work(struct work_struct *work)
{
	struct smb1355 *chip = container_of(to_delayed_work(work),
						struct smb1355, retry_work);
	unsigned int status;
	bool active, retry = false, toggle = false;
	int ret;

	mutex_lock(&chip->online_lock);
	if (!chip->enabled)
		goto out;

	ret = smb1355_charging_active(chip, &status, &active);
	if (ret || active) {
		chip->retry_count = 0;
		goto out;
	}

	/* The factory checks every 3 seconds and retries after four failures. */
	if (chip->retry_count < SMB1355_RETRY_COUNT) {
		chip->retry_count++;
		retry = true;
	} else {
		chip->retry_count = 0;
		toggle = true;
	}
out:
	mutex_unlock(&chip->online_lock);

	if (retry)
		mod_delayed_work(system_dfl_wq, &chip->retry_work,
				 msecs_to_jiffies(SMB1355_RETRY_DELAY_MS));
	if (toggle) {
		dev_warn(chip->dev,
			 "SMB1355 is not charging; retriggering parallel path\n");
		smb1355_set_online(chip, false);
		smb1355_set_online(chip, true);
	}
}

static int smb1355_set_online(struct smb1355 *chip, bool enable)
{
	bool restart_retry = false, restart_temp = false;
	bool stop_retry = false, stop_temp = false, stop_watchdog = false;
	bool restart_watchdog = false;
	int cleanup_ret, health, ret;

	mutex_lock(&chip->online_lock);
	if (chip->online_valid && chip->enabled == enable) {
		ret = 0;
		goto out;
	}
	if (enable) {
		/* Refuse to energize the secondary path if it is already reset-hot. */
		ret = smb1355_read_temp_health(chip, &health);
		if (ret)
			goto out;
		if (health == POWER_SUPPLY_HEALTH_OVERHEAT) {
			dev_warn_ratelimited(chip->dev,
					     "Refusing to enable overheated parallel charger\n");
			ret = -EAGAIN;
			goto out;
		}
	}

	ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				  SMB1355_WDOG_ENABLE,
				  enable ? SMB1355_WDOG_ENABLE : 0);
	if (ret)
		goto out;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_CFG2_REG,
				  SMB1355_CHG_EN_SOURCE |
				  SMB1355_CHG_EN_POLARITY,
				  enable ? SMB1355_CHG_EN_SOURCE : 0);
	if (ret)
		goto disable;
	ret = smb1355_update_bits(chip, SMB1355_BANDGAP_REG,
				  SMB1355_BANDGAP_ENABLE,
				  enable ? SMB1355_BANDGAP_ENABLE : 0);
	if (ret)
		goto disable;

	WRITE_ONCE(chip->enabled, enable);
	WRITE_ONCE(chip->online_valid, true);
	WRITE_ONCE(chip->die_temp_decic, -EINVAL);
	restart_temp = enable;
	restart_retry = enable;
	restart_watchdog = enable;
	stop_temp = stop_temp || !enable;
	stop_retry = !enable;
	stop_watchdog = !enable;
	ret = 0;
	goto out;

disable:
	cleanup_ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
					  SMB1355_WDOG_ENABLE, 0);
	if (!ret)
		ret = cleanup_ret;
	cleanup_ret = smb1355_update_bits(chip, SMB1355_CHGR_CFG2_REG,
					  SMB1355_CHG_EN_SOURCE |
					  SMB1355_CHG_EN_POLARITY, 0);
	if (!ret)
		ret = cleanup_ret;
	cleanup_ret = smb1355_update_bits(chip, SMB1355_BANDGAP_REG,
					  SMB1355_BANDGAP_ENABLE, 0);
	if (!ret)
		ret = cleanup_ret;
	WRITE_ONCE(chip->enabled, false);
	WRITE_ONCE(chip->online_valid, false);
	WRITE_ONCE(chip->die_temp_decic, -EINVAL);
	stop_temp = true;
	stop_retry = true;
	stop_watchdog = true;
out:
	if (stop_retry) {
		chip->retry_count = 0;
		cancel_delayed_work(&chip->retry_work);
	}
	if (stop_temp)
		cancel_delayed_work_sync(&chip->die_temp_work);
	mutex_unlock(&chip->online_lock);
	if (stop_watchdog)
		cancel_delayed_work_sync(&chip->watchdog_work);
	if (restart_temp)
		schedule_delayed_work(&chip->die_temp_work, 0);
	if (restart_retry && !delayed_work_pending(&chip->retry_work))
		schedule_delayed_work(&chip->retry_work,
				      msecs_to_jiffies(SMB1355_RETRY_DELAY_MS));
	if (restart_watchdog)
		mod_delayed_work(system_dfl_wq, &chip->watchdog_work, 0);
	return ret;
}

static int smb1355_hw_init(struct smb1355 *chip)
{
	int ret;

	ret = smb1355_update_bits(chip, SMB1355_CLOCK_REQUEST_REG,
				  SMB1355_CLOCK_REQUEST,
				  SMB1355_CLOCK_REQUEST);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_I2C_SID_REG, GENMASK(3, 0), 1);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_WDOG_CFG_REG,
				  BIT(6) | BIT(5) | BIT(1) | BIT(0),
				  BIT(6) | BIT(5));
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_WDOG_TIMEOUT_REG,
				  SMB1355_WDOG_DISABLE_CHARGING,
				  SMB1355_WDOG_DISABLE_CHARGING);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_ENABLE_CMD_REG, BIT(0), 0);
	if (ret)
		return ret;
	ret = smb1355_set_online(chip, false);
	if (ret)
		return ret;
	ret = smb1355_set_param(chip, SMB1355_FCC_REG, 0,
				SMB1355_FCC_MIN_UA, SMB1355_FCC_MAX_UA,
				SMB1355_FCC_STEP_UA);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_HICCUP_REG, GENMASK(5, 0), 0);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_CHGR_CFG_REG,
				  SMB1355_PARALLEL_SENSE,
				  SMB1355_PARALLEL_SENSE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_PRE_FAST_REG, GENMASK(2, 0), 0);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_THERMREG_REG,
				  SMB1355_DIE_TEMP_BYPASS |
				  SMB1355_DIE_TEMP_ENABLE,
				  SMB1355_DIE_TEMP_BYPASS |
				  SMB1355_DIE_TEMP_ENABLE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_SPARE1_REG,
				  SMB1355_DIE_TEMP_HYST, 0);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_VALLEY_COMP_REG, BIT(0), BIT(0));
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_ILIMIT_CFG_REG, BIT(3), BIT(3));
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_SPARE1_REG,
				  SMB1355_EXT_BIAS_ENABLE,
				  SMB1355_EXT_BIAS_ENABLE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_BATID_REG,
				  SMB1355_EXT_BIAS_ENABLE,
				  SMB1355_EXT_BIAS_ENABLE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_RBIAS_REG,
				  SMB1355_RBIAS_ENABLE,
				  SMB1355_RBIAS_ENABLE);
	if (ret)
		return ret;
	ret = smb1355_update_bits(chip, SMB1355_THERMREG_REG,
				  SMB1355_SKIN_TEMP_ENABLE,
				  SMB1355_SKIN_TEMP_ENABLE);
	if (ret)
		return ret;
	return 0;
}

static enum power_supply_property smb1355_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_TEMP,
};

static int smb1355_charging_active(struct smb1355 *chip,
				   unsigned int *status, bool *active)
{
	unsigned int pin_status;
	int ret;

	ret = regmap_read(chip->regmap, SMB1355_CHGR_STATUS_REG, status);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, SMB1355_BATTERY_STATUS_2_REG,
			  &pin_status);
	if (!ret)
		*active = (*status & SMB1355_CHARGING_ENABLED) &&
			  !(pin_status & SMB1355_DISABLE_CHARGING);

	return ret;
}

static int smb1355_get_property(struct power_supply *psy,
				enum power_supply_property prop,
				union power_supply_propval *val)
{
	struct smb1355 *chip = power_supply_get_drvdata(psy);
	unsigned int status;
	bool active;
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
	case POWER_SUPPLY_PROP_ONLINE:
		ret = smb1355_charging_active(chip, &status, &active);
		if (ret)
			return ret;
		if (prop == POWER_SUPPLY_PROP_STATUS)
			val->intval = active ?
				POWER_SUPPLY_STATUS_CHARGING :
				POWER_SUPPLY_STATUS_NOT_CHARGING;
		else if (prop == POWER_SUPPLY_PROP_CHARGE_TYPE)
			val->intval = !active ?
				POWER_SUPPLY_CHARGE_TYPE_NONE :
				(status & SMB1355_FAST_CHARGE ?
				 POWER_SUPPLY_CHARGE_TYPE_FAST :
				 POWER_SUPPLY_CHARGE_TYPE_TRICKLE);
		else
			val->intval = active;
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		if (!READ_ONCE(chip->enabled)) {
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
			return 0;
		}
		return smb1355_read_temp_health(chip, &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		return smb1355_get_param(chip, SMB1355_FCC_REG,
					 SMB1355_FCC_MIN_UA, SMB1355_FCC_MAX_UA,
					 SMB1355_FCC_STEP_UA,
					 &val->intval);
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		return smb1355_get_param(chip, SMB1355_BAT_OV_REG,
					 SMB1355_FV_MIN_UV, SMB1355_FV_MAX_UV,
					 SMB1355_FV_STEP_UV,
					 &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = READ_ONCE(chip->die_temp_decic);
		return 0;
	default:
		return -EINVAL;
	}
}

static int smb1355_set_property(struct power_supply *psy,
				enum power_supply_property prop,
				const union power_supply_propval *val)
{
	struct smb1355 *chip = power_supply_get_drvdata(psy);
	int ret;

	switch (prop) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = smb1355_set_online(chip, val->intval);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = smb1355_set_param(chip, SMB1355_FCC_REG, val->intval,
					SMB1355_FCC_MIN_UA, SMB1355_FCC_MAX_UA,
					SMB1355_FCC_STEP_UA);
		break;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		ret = smb1355_set_param(chip, SMB1355_BAT_OV_REG, val->intval,
					SMB1355_FV_MIN_UV, SMB1355_FV_MAX_UV,
					SMB1355_FV_STEP_UV);
		break;
	default:
		return -EINVAL;
	}
	if (!ret)
		power_supply_changed(psy);

	return ret;
}

static int smb1355_property_is_writeable(struct power_supply *psy,
					 enum power_supply_property prop)
{
	return prop == POWER_SUPPLY_PROP_ONLINE ||
	       prop == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX ||
	       prop == POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX;
}

static const struct power_supply_desc smb1355_desc = {
	.name = "smb1355-parallel",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = smb1355_properties,
	.num_properties = ARRAY_SIZE(smb1355_properties),
	.get_property = smb1355_get_property,
	.set_property = smb1355_set_property,
	.property_is_writeable = smb1355_property_is_writeable,
};

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
	mutex_init(&chip->online_lock);
	INIT_DELAYED_WORK(&chip->die_temp_work, smb1355_die_temp_work);
	INIT_DELAYED_WORK(&chip->retry_work, smb1355_retry_work);
	INIT_DELAYED_WORK(&chip->watchdog_work, smb1355_watchdog_work);
	chip->die_temp_decic = -EINVAL;
	i2c_set_clientdata(client, chip);

	chip->regmap = devm_regmap_init_i2c(client, &smb1355_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(chip->dev, PTR_ERR(chip->regmap),
				     "Failed to initialize regmap\n");
	ret = regmap_read(chip->regmap, SMB1355_MFG_ID_REG, &id);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to read chip ID\n");
	if (id != SMB1355_MFG_ID)
		return dev_err_probe(chip->dev, -ENODEV,
				     "Chip ID 0x%02x is not SMB1355\n", id);
	ret = smb1355_hw_init(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to initialize SMB1355\n");

	config.drv_data = chip;
	config.fwnode = dev_fwnode(chip->dev);
	chip->psy = devm_power_supply_register(chip->dev, &smb1355_desc,
					       &config);
	if (IS_ERR(chip->psy))
		return dev_err_probe(chip->dev, PTR_ERR(chip->psy),
				     "Failed to register parallel power supply\n");

	dev_info(chip->dev, "SMB1355 parallel charger initialized disabled\n");
	return 0;
}

static void smb1355_remove(struct i2c_client *client)
{
	struct smb1355 *chip = i2c_get_clientdata(client);

	smb1355_set_online(chip, false);
	cancel_delayed_work_sync(&chip->watchdog_work);
	cancel_delayed_work_sync(&chip->retry_work);
	cancel_delayed_work_sync(&chip->die_temp_work);
	smb1355_update_bits(chip, SMB1355_CLOCK_REQUEST_REG,
			    SMB1355_CLOCK_REQUEST, 0);
}

static void smb1355_shutdown(struct i2c_client *client)
{
	smb1355_remove(client);
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
	.remove = smb1355_remove,
	.shutdown = smb1355_shutdown,
};
module_i2c_driver(smb1355_driver);

MODULE_DESCRIPTION("Qualcomm SMB1355 parallel charger driver");
MODULE_LICENSE("GPL");
