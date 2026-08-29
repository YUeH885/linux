// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define PM8150B_CHGR_STATUS_1		0x1006
#define PM8150B_CHGR_STATUS_2		0x1007
#define PM8150B_CHGR_STATUS_7		0x100d
#define PM8150B_CHGR_FCC_CFG		0x1061
#define PM8150B_CHGR_FV_CFG		0x1070
#define PM8150B_CHGR_JEITA_CFG		0x1090
#define PM8150B_CHGR_HARD_JEITA_THR	0x1098
#define PM8150B_POWER_PATH_STATUS	0x110b
#define PM8150B_BATIF_INT_RT_STS	0x1210
#define PM8150B_APSD_STATUS		0x1307
#define PM8150B_APSD_RESULT		0x1308
#define PM8150B_USB_INT_RT_STS		0x1310
#define PM8150B_USB_ICL_CFG		0x1370
#define PM8150B_WDOG_PET		0x1643
#define PM8150B_WDOG_CFG		0x1651
#define PM8150B_WDOG_TIMEOUT_CFG	0x1653
#define PM8150B_THERMREG_SRC_CFG	0x1670
#define PM8150B_FG_MONOTONIC_SOC	0x4009
#define PM8150B_FG_VBATT		0x41a0
#define PM8150B_FG_IBATT		0x41a2
#define PM8150B_FG_VBATT_COPY		0x41a6
#define PM8150B_FG_IBATT_COPY		0x41a8
#define PM8150B_FG_ALG_STATUS		0x41ef
#define PM8150B_FG_MEM_INT_RT_STS	0x4310
#define PM8150B_FG_MEM_ARB_CFG		0x4340
#define PM8150B_FG_MEM_INTF_CFG	0x4350
#define PM8150B_FG_DMA_CTL		0x4371
#define PM8150B_FG_BATT_TEMP		0x4858

#define PM8150B_CHGR_STATUS_MASK	GENMASK(2, 0)
#define PM8150B_CHGR_BAT_OV		BIT(1)
#define PM8150B_BAT_TEMP_HOT_SOFT	BIT(5)
#define PM8150B_BAT_TEMP_COLD_SOFT	BIT(4)
#define PM8150B_BAT_TEMP_TOO_HOT	BIT(3)
#define PM8150B_BAT_TEMP_TOO_COLD	BIT(2)
#define PM8150B_USE_USBIN		BIT(4)
#define PM8150B_VALID_INPUT		BIT(0)
#define PM8150B_BAT_TERMINAL_MISSING	BIT(5)
#define PM8150B_BAT_THERM_MISSING	BIT(4)
#define PM8150B_APSD_DONE		BIT(0)
#define PM8150B_APSD_DCP		BIT(3)
#define PM8150B_APSD_CDP		BIT(2)
#define PM8150B_APSD_OCP		BIT(1)
#define PM8150B_USB_PLUGIN		BIT(4)
#define PM8150B_HARD_JEITA_ENABLE	BIT(4)
#define PM8150B_DIE_CMP_ENABLE		BIT(0)
#define PM8150B_WDOG_TRIGGER_AFP	BIT(7)
#define PM8150B_WDOG_BARK_IRQ_ENABLE	BIT(6)
#define PM8150B_WDOG_ENABLE_ON_PLUGIN	BIT(1)
#define PM8150B_WDOG_PET_BIT		BIT(0)
#define PM8150B_FG_ALG_ACTIVE		BIT(3)
#define PM8150B_FG_MEM_GRANT		BIT(3)
#define PM8150B_FG_MEM_ARB_REQUEST	BIT(0)
#define PM8150B_FG_LOW_LATENCY		BIT(1)
#define PM8150B_FG_CLEAR_LOG		BIT(2)
#define PM8150B_FG_MEM_ACCESS_REQUEST	BIT(7)
#define PM8150B_FG_IACS_SELECT		BIT(5)
#define PM8150B_FG_ADDR_KIND		BIT(1)

#define PM8150B_FCC_STEP_UA		50000
#define PM8150B_FV_MIN_UV		3600000
#define PM8150B_FV_STEP_UV		10000
#define PM8150B_USB_ICL_STEP_UA		50000
#define PM8150B_WDOG_TIMEOUT_64S	0x7b

enum pm8150b_charge_phase {
	PM8150B_INHIBIT_CHARGE,
	PM8150B_TRICKLE_CHARGE,
	PM8150B_PRE_CHARGE,
	PM8150B_FULLON_CHARGE,
	PM8150B_TAPER_CHARGE,
	PM8150B_TERMINATE_CHARGE,
	PM8150B_PAUSE_CHARGE,
	PM8150B_DISABLE_CHARGE,
};

struct pm8150b_charger {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *usb_in_i;
	struct iio_channel *usb_in_v;
	struct power_supply *battery;
	struct power_supply *usb;
	/* 串行化 Gen4 SRAM DMA 仲裁。 */
	struct mutex fg_lock;
	int technology;
	int charge_full_design_uah;
	int voltage_max_design_uv;
	int constant_charge_current_max_ua;
	int constant_charge_voltage_max_uv;
};

static int pm8150b_fg_release(struct pm8150b_charger *chip)
{
	int ret, ret2;

	ret = regmap_update_bits(chip->regmap, PM8150B_FG_MEM_INTF_CFG,
				 PM8150B_FG_MEM_ACCESS_REQUEST |
				 PM8150B_FG_IACS_SELECT, 0);
	ret2 = regmap_update_bits(chip->regmap, PM8150B_FG_MEM_ARB_CFG,
				  PM8150B_FG_MEM_ARB_REQUEST, 0);

	return ret ?: ret2;
}

static int pm8150b_fg_request(struct pm8150b_charger *chip)
{
	unsigned int val;
	int ret;

	/* ESR 脉冲期间等待算法释放 SRAM。 */
	/* 这是 PM8150B 早期版本 DMA 读取的硬件要求。 */
	ret = regmap_read_poll_timeout(chip->regmap, PM8150B_FG_ALG_STATUS,
				       val, !(val & PM8150B_FG_ALG_ACTIVE),
				       10000, 350000);
	if (ret)
		return ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_FG_MEM_ARB_CFG,
				 PM8150B_FG_MEM_ARB_REQUEST,
				 PM8150B_FG_MEM_ARB_REQUEST);
	if (ret)
		return ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_FG_MEM_INTF_CFG,
				 PM8150B_FG_MEM_ACCESS_REQUEST |
				 PM8150B_FG_IACS_SELECT,
				 PM8150B_FG_MEM_ACCESS_REQUEST);
	if (ret)
		goto release;

	usleep_range(40, 50);
	ret = regmap_read_poll_timeout(chip->regmap, PM8150B_FG_MEM_INT_RT_STS,
				       val, val & PM8150B_FG_MEM_GRANT,
				       10000, 500000);
	if (!ret)
		return 0;

release:
	pm8150b_fg_release(chip);
	return ret;
}

static int pm8150b_fg_init(struct pm8150b_charger *chip)
{
	int ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_FG_DMA_CTL,
				 PM8150B_FG_ADDR_KIND, PM8150B_FG_ADDR_KIND);
	if (ret)
		return ret;

	ret = pm8150b_fg_release(chip);
	if (ret)
		return ret;

	return regmap_update_bits(chip->regmap, PM8150B_FG_MEM_ARB_CFG,
				  PM8150B_FG_LOW_LATENCY |
				  PM8150B_FG_CLEAR_LOG,
				  PM8150B_FG_LOW_LATENCY);
}

static int pm8150b_fg_read_temp(struct pm8150b_charger *chip, int *temp)
{
	u8 buf[2];
	int ret, ret2;

	mutex_lock(&chip->fg_lock);
	ret = pm8150b_fg_request(chip);
	if (!ret)
		ret = regmap_bulk_read(chip->regmap, PM8150B_FG_BATT_TEMP,
				       buf, sizeof(buf));
	ret2 = pm8150b_fg_release(chip);
	mutex_unlock(&chip->fg_lock);
	if (ret)
		return ret;
	if (ret2)
		return ret2;

	*temp = sign_extend32(get_unaligned_le16(buf), 9) * 100 / 40;
	return 0;
}

static int pm8150b_fg_read_shadow(struct pm8150b_charger *chip,
				  unsigned int reg,
				  unsigned int copy_reg, u16 *raw)
{
	u8 buf[2], copy[2];
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_bulk_read(chip->regmap, reg, buf, sizeof(buf));
		if (ret)
			return ret;
		ret = regmap_bulk_read(chip->regmap, copy_reg, copy,
				       sizeof(copy));
		if (ret)
			return ret;
		if (!memcmp(buf, copy, sizeof(buf))) {
			*raw = get_unaligned_le16(buf);
			return 0;
		}
	}

	return -EIO;
}

static int pm8150b_fg_capacity(struct pm8150b_charger *chip, int *capacity)
{
	u8 raw[2];
	int i, ret;

	for (i = 0; i < 3; i++) {
		ret = regmap_bulk_read(chip->regmap, PM8150B_FG_MONOTONIC_SOC,
				       raw, sizeof(raw));
		if (ret)
			return ret;
		if (raw[0] == raw[1])
			break;
	}
	if (i == 3)
		return -EIO;

	if (raw[0] == 255)
		*capacity = 100;
	else if (!raw[0])
		*capacity = 0;
	else
		*capacity = DIV_ROUND_CLOSEST((raw[0] - 1) * 98, 253) + 1;

	return 0;
}

static int pm8150b_fg_voltage(struct pm8150b_charger *chip, int *voltage)
{
	u16 raw;
	int ret;

	ret = pm8150b_fg_read_shadow(chip, PM8150B_FG_VBATT,
				     PM8150B_FG_VBATT_COPY, &raw);
	if (!ret)
		*voltage = div_u64((u64)raw * 122070, 1000);

	return ret;
}

static int pm8150b_fg_current(struct pm8150b_charger *chip, int *current_ua)
{
	u16 raw;
	int ret;

	ret = pm8150b_fg_read_shadow(chip, PM8150B_FG_IBATT,
				     PM8150B_FG_IBATT_COPY, &raw);
	if (!ret)
		*current_ua = div_s64((s64)(s16)raw * 488281, 1000);

	return ret;
}

static int pm8150b_usb_present(struct pm8150b_charger *chip, int *present)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, PM8150B_USB_INT_RT_STS, &val);
	if (!ret)
		*present = !!(val & PM8150B_USB_PLUGIN);

	return ret;
}

static int pm8150b_usb_online(struct pm8150b_charger *chip, int *online)
{
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, PM8150B_POWER_PATH_STATUS, &val);
	if (!ret)
		*online = (val & PM8150B_USE_USBIN) &&
			  (val & PM8150B_VALID_INPUT);

	return ret;
}

static int pm8150b_usb_type(struct pm8150b_charger *chip, int *type)
{
	unsigned int val;
	int online, ret;

	ret = pm8150b_usb_online(chip, &online);
	if (ret || !online) {
		*type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return ret;
	}

	ret = regmap_read(chip->regmap, PM8150B_APSD_STATUS, &val);
	if (ret)
		return ret;
	if (!(val & PM8150B_APSD_DONE)) {
		*type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return 0;
	}

	ret = regmap_read(chip->regmap, PM8150B_APSD_RESULT, &val);
	if (ret)
		return ret;
	if (val & PM8150B_APSD_CDP)
		*type = POWER_SUPPLY_USB_TYPE_CDP;
	else if (val & (PM8150B_APSD_DCP | PM8150B_APSD_OCP))
		*type = POWER_SUPPLY_USB_TYPE_DCP;
	else
		*type = POWER_SUPPLY_USB_TYPE_SDP;

	return 0;
}

static int pm8150b_battery_status(struct pm8150b_charger *chip, int *status)
{
	unsigned int val;
	int online, ret;

	ret = pm8150b_usb_online(chip, &online);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_1, &val);
	if (ret)
		return ret;
	val &= PM8150B_CHGR_STATUS_MASK;

	if (!online) {
		*status = val == PM8150B_TERMINATE_CHARGE ||
			  val == PM8150B_INHIBIT_CHARGE ?
			  POWER_SUPPLY_STATUS_FULL :
			  POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}

	switch (val) {
	case PM8150B_TRICKLE_CHARGE:
	case PM8150B_PRE_CHARGE:
	case PM8150B_FULLON_CHARGE:
	case PM8150B_TAPER_CHARGE:
		*status = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case PM8150B_TERMINATE_CHARGE:
	case PM8150B_INHIBIT_CHARGE:
		*status = POWER_SUPPLY_STATUS_FULL;
		break;
	case PM8150B_PAUSE_CHARGE:
	case PM8150B_DISABLE_CHARGE:
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		*status = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	return 0;
}

static int pm8150b_battery_health(struct pm8150b_charger *chip, int *health)
{
	unsigned int val;
	int voltage, ret;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_2, &val);
	if (ret)
		return ret;
	if (val & PM8150B_CHGR_BAT_OV) {
		ret = pm8150b_fg_voltage(chip, &voltage);
		if (ret || voltage >= chip->constant_charge_voltage_max_uv + 40000) {
			*health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
			return 0;
		}
	}

	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_7, &val);
	if (ret)
		return ret;
	if (val & PM8150B_BAT_TEMP_TOO_COLD)
		*health = POWER_SUPPLY_HEALTH_COLD;
	else if (val & PM8150B_BAT_TEMP_TOO_HOT)
		*health = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (val & PM8150B_BAT_TEMP_COLD_SOFT)
		*health = POWER_SUPPLY_HEALTH_COOL;
	else if (val & PM8150B_BAT_TEMP_HOT_SOFT)
		*health = POWER_SUPPLY_HEALTH_WARM;
	else
		*health = POWER_SUPPLY_HEALTH_GOOD;

	return 0;
}

static enum power_supply_property pm8150b_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

static int pm8150b_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct pm8150b_charger *chip = power_supply_get_drvdata(psy);
	unsigned int reg;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		return pm8150b_battery_status(chip, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_1, &reg);
		if (ret)
			return ret;
		switch (reg & PM8150B_CHGR_STATUS_MASK) {
		case PM8150B_TRICKLE_CHARGE:
		case PM8150B_PRE_CHARGE:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
			break;
		case PM8150B_FULLON_CHARGE:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
			break;
		case PM8150B_TAPER_CHARGE:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_STANDARD;
			break;
		default:
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		}
		return 0;
	case POWER_SUPPLY_PROP_HEALTH:
		return pm8150b_battery_health(chip, &val->intval);
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(chip->regmap, PM8150B_BATIF_INT_RT_STS, &reg);
		if (!ret)
			val->intval = !(reg & (PM8150B_BAT_TERMINAL_MISSING |
						 PM8150B_BAT_THERM_MISSING));
		return ret;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = chip->technology;
		return 0;
	case POWER_SUPPLY_PROP_CAPACITY:
		return pm8150b_fg_capacity(chip, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return pm8150b_fg_voltage(chip, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		return pm8150b_fg_current(chip, &val->intval);
	case POWER_SUPPLY_PROP_TEMP:
		return pm8150b_fg_read_temp(chip, &val->intval);
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = chip->charge_full_design_uah;
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = chip->voltage_max_design_uv;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		val->intval = chip->constant_charge_current_max_ua;
		return 0;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = chip->constant_charge_voltage_max_uv;
		return 0;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "LGE BL-T41";
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc pm8150b_battery_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = pm8150b_battery_properties,
	.num_properties = ARRAY_SIZE(pm8150b_battery_properties),
	.get_property = pm8150b_battery_get_property,
};

static enum power_supply_property pm8150b_usb_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static int pm8150b_usb_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct pm8150b_charger *chip = power_supply_get_drvdata(psy);
	struct iio_channel *channel;
	unsigned int reg;
	int present, ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		return pm8150b_usb_present(chip, &val->intval);
	case POWER_SUPPLY_PROP_ONLINE:
		return pm8150b_usb_online(chip, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = pm8150b_usb_present(chip, &present);
		if (ret || !present) {
			val->intval = 0;
			return ret;
		}
		channel = psp == POWER_SUPPLY_PROP_VOLTAGE_NOW ?
			  chip->usb_in_v : chip->usb_in_i;
		ret = iio_read_channel_processed(channel, &val->intval);
		if (!ret && psp == POWER_SUPPLY_PROP_CURRENT_NOW)
			val->intval *= 5;
		return ret;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = regmap_read(chip->regmap, PM8150B_USB_ICL_CFG, &reg);
		if (!ret)
			val->intval = reg * PM8150B_USB_ICL_STEP_UA;
		return ret;
	case POWER_SUPPLY_PROP_USB_TYPE:
		return pm8150b_usb_type(chip, &val->intval);
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc pm8150b_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP),
	.properties = pm8150b_usb_properties,
	.num_properties = ARRAY_SIZE(pm8150b_usb_properties),
	.get_property = pm8150b_usb_get_property,
};

static int pm8150b_load_battery_info(struct pm8150b_charger *chip)
{
	struct power_supply_battery_info *info;
	int ret;

	ret = power_supply_get_battery_info(chip->usb, &info);
	if (ret)
		return ret;

	chip->technology = info->technology;
	chip->charge_full_design_uah = info->charge_full_design_uah;
	chip->voltage_max_design_uv = info->voltage_max_design_uv;
	chip->constant_charge_current_max_ua =
		info->constant_charge_current_max_ua;
	chip->constant_charge_voltage_max_uv =
		info->constant_charge_voltage_max_uv;
	power_supply_put_battery_info(chip->usb, info);

	if (chip->constant_charge_current_max_ua <= 0 ||
	    chip->constant_charge_voltage_max_uv < PM8150B_FV_MIN_UV)
		return -EINVAL;

	return 0;
}

static int pm8150b_clamp_profile(struct pm8150b_charger *chip)
{
	unsigned int raw, limit;
	int ret;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_FCC_CFG, &raw);
	if (ret)
		return ret;
	limit = chip->constant_charge_current_max_ua / PM8150B_FCC_STEP_UA;
	if (raw > limit) {
		ret = regmap_write(chip->regmap, PM8150B_CHGR_FCC_CFG, limit);
		if (ret)
			return ret;
		dev_warn(chip->dev, "充电电流已从 %u uA 向下钳制到 %u uA\n",
			 raw * PM8150B_FCC_STEP_UA,
			 limit * PM8150B_FCC_STEP_UA);
	}

	ret = regmap_read(chip->regmap, PM8150B_CHGR_FV_CFG, &raw);
	if (ret)
		return ret;
	limit = (chip->constant_charge_voltage_max_uv - PM8150B_FV_MIN_UV) /
		 PM8150B_FV_STEP_UV;
	if (raw > limit) {
		ret = regmap_write(chip->regmap, PM8150B_CHGR_FV_CFG, limit);
		if (ret)
			return ret;
		dev_warn(chip->dev, "充电电压已从 %u uV 向下钳制到 %u uV\n",
			 PM8150B_FV_MIN_UV + raw * PM8150B_FV_STEP_UV,
			 PM8150B_FV_MIN_UV + limit * PM8150B_FV_STEP_UV);
	}

	return 0;
}

static int pm8150b_enable_safety(struct pm8150b_charger *chip)
{
	u8 threshold[4];
	u16 hot, cold;
	int ret;

	ret = regmap_bulk_read(chip->regmap, PM8150B_CHGR_HARD_JEITA_THR,
			       threshold, sizeof(threshold));
	if (ret)
		return ret;
	hot = get_unaligned_be16(threshold);
	cold = get_unaligned_be16(threshold + 2);
	if (!hot || hot == U16_MAX || !cold || cold == U16_MAX || hot >= cold)
		return dev_err_probe(chip->dev, -EINVAL,
				     "ABL hard JEITA 阈值无效：hot=%u cold=%u\n",
				     hot, cold);

	ret = regmap_update_bits(chip->regmap, PM8150B_CHGR_JEITA_CFG,
				 PM8150B_HARD_JEITA_ENABLE,
				 PM8150B_HARD_JEITA_ENABLE);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_THERMREG_SRC_CFG,
				 PM8150B_DIE_CMP_ENABLE,
				 PM8150B_DIE_CMP_ENABLE);
	if (ret)
		return ret;

	ret = regmap_write(chip->regmap, PM8150B_WDOG_TIMEOUT_CFG,
			   PM8150B_WDOG_TIMEOUT_64S);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_WDOG_CFG,
				 PM8150B_WDOG_TRIGGER_AFP |
				 PM8150B_WDOG_BARK_IRQ_ENABLE |
				 PM8150B_WDOG_ENABLE_ON_PLUGIN,
				 PM8150B_WDOG_BARK_IRQ_ENABLE |
				 PM8150B_WDOG_ENABLE_ON_PLUGIN);
	if (ret)
		return ret;

	return regmap_write(chip->regmap, PM8150B_WDOG_PET,
			    PM8150B_WDOG_PET_BIT);
}

static irqreturn_t pm8150b_changed_irq(int irq, void *data)
{
	struct pm8150b_charger *chip = data;

	power_supply_changed(chip->battery);
	power_supply_changed(chip->usb);
	return IRQ_HANDLED;
}

static irqreturn_t pm8150b_wdog_bark_irq(int irq, void *data)
{
	struct pm8150b_charger *chip = data;
	int ret;

	ret = regmap_write(chip->regmap, PM8150B_WDOG_PET,
			   PM8150B_WDOG_PET_BIT);
	if (ret)
		dev_err(chip->dev, "充电 watchdog 喂狗失败：%d\n", ret);

	return IRQ_HANDLED;
}

static int pm8150b_request_irq(struct platform_device *pdev, const char *name,
			       irq_handler_t handler, int *irq_out)
{
	int irq, ret;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0)
		return irq;
	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL, handler,
					IRQF_ONESHOT, name, platform_get_drvdata(pdev));
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "无法申请 %s IRQ\n", name);
	if (irq_out)
		*irq_out = irq;

	return 0;
}

static int pm8150b_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct pm8150b_charger *chip;
	int ret, wdog_irq;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &pdev->dev;
	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV, "找不到 PMIC regmap\n");
	mutex_init(&chip->fg_lock);
	platform_set_drvdata(pdev, chip);

	chip->usb_in_i = devm_iio_channel_get(chip->dev, "usbin_i");
	if (IS_ERR(chip->usb_in_i))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_i),
				     "无法取得 usbin_i IIO channel\n");
	chip->usb_in_v = devm_iio_channel_get(chip->dev, "usbin_v");
	if (IS_ERR(chip->usb_in_v))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_v),
				     "无法取得 usbin_v IIO channel\n");

	ret = pm8150b_fg_init(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "FG DMA 初始化失败\n");

	config.drv_data = chip;
	config.fwnode = dev_fwnode(chip->dev);
	chip->usb = devm_power_supply_register(chip->dev, &pm8150b_usb_desc,
					       &config);
	if (IS_ERR(chip->usb))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb),
				     "注册 usb power_supply 失败\n");

	ret = pm8150b_load_battery_info(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "读取 BL-T41 参数失败\n");
	ret = pm8150b_clamp_profile(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "钳制 BL-T41 充电上限失败\n");

	chip->battery = devm_power_supply_register(chip->dev,
						   &pm8150b_battery_desc,
						   &config);
	if (IS_ERR(chip->battery))
		return dev_err_probe(chip->dev, PTR_ERR(chip->battery),
				     "注册 battery power_supply 失败\n");

	ret = pm8150b_request_irq(pdev, "chg-state-change",
				  pm8150b_changed_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "bat-temp", pm8150b_changed_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "usbin-plugin",
				  pm8150b_changed_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "soc-update", pm8150b_changed_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "batt-temp-delta",
				  pm8150b_changed_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "wdog-bark",
				  pm8150b_wdog_bark_irq, &wdog_irq);
	if (ret)
		return ret;

	ret = devm_device_init_wakeup(chip->dev);
	if (ret)
		return ret;
	ret = devm_pm_set_wake_irq(chip->dev, wdog_irq);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "无法将 watchdog IRQ 设为唤醒源\n");

	ret = pm8150b_enable_safety(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "启用充电安全配置失败\n");

	dev_info(chip->dev,
		 "PM8150B power_supply 已启用，BL-T41 上限 %d uA/%d uV\n",
		 chip->constant_charge_current_max_ua,
		 chip->constant_charge_voltage_max_uv);
	return 0;
}

static void pm8150b_remove(struct platform_device *pdev)
{
	struct pm8150b_charger *chip = platform_get_drvdata(pdev);

	regmap_update_bits(chip->regmap, PM8150B_WDOG_CFG,
			   PM8150B_WDOG_TRIGGER_AFP |
			   PM8150B_WDOG_BARK_IRQ_ENABLE |
			   PM8150B_WDOG_ENABLE_ON_PLUGIN, 0);
}

static const struct of_device_id pm8150b_match_table[] = {
	{ .compatible = "qcom,pm8150b-charger" },
	{}
};
MODULE_DEVICE_TABLE(of, pm8150b_match_table);

static struct platform_driver pm8150b_driver = {
	.probe = pm8150b_probe,
	.remove = pm8150b_remove,
	.driver = {
		.name = "qcom-pm8150b-charger",
		.of_match_table = pm8150b_match_table,
	},
};
module_platform_driver(pm8150b_driver);

MODULE_DESCRIPTION("Qualcomm PM8150B charger and fuel gauge driver");
MODULE_LICENSE("GPL");
