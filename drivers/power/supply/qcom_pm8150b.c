// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeirq.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <soc/qcom/qcom-spmi-pmic.h>

#define PM8150B_CHGR_STATUS_1		0x1006
#define PM8150B_CHGR_STATUS_2		0x1007
#define PM8150B_CHGR_STATUS_7		0x100d
#define PM8150B_CHGR_ENABLE_CMD		0x1042
#define PM8150B_CHGR_CFG2		0x1051
#define PM8150B_CHGR_FCC_CFG		0x1061
#define PM8150B_CHGR_TERM_RCHG_CFG	0x106b
#define PM8150B_CHGR_ADC_TERM_CFG	0x106c
#define PM8150B_CHGR_FV_CFG		0x1070
#define PM8150B_CHGR_RCHG_SOC_CFG	0x107d
#define PM8150B_CHGR_JEITA_CFG		0x1090
#define PM8150B_CHGR_HARD_JEITA_THR	0x1098
#define PM8150B_AICL_ICL_STATUS		0x1108
#define PM8150B_DCDC_FSW_SEL		0x1150
#define PM8150B_AICL_5V_THRESHOLD	0x1381
#define PM8150B_AICL_CONT_THRESHOLD	0x1384
#define PM8150B_POWER_PATH_STATUS	0x110b
#define PM8150B_BATIF_INT_RT_STS	0x1210
#define PM8150B_APSD_STATUS		0x1307
#define PM8150B_APSD_RESULT		0x1308
#define PM8150B_USB_INT_RT_STS		0x1310
#define PM8150B_USBIN_CMD_IL		0x1340
#define PM8150B_CMD_ICL_OVERRIDE	0x1342
#define PM8150B_USB_ADAPTER_ALLOW_OVERRIDE	0x1344
#define PM8150B_USB_ADAPTER_ALLOW_CFG	0x1360
#define PM8150B_USB_OPTIONS_1_CFG	0x1362
#define PM8150B_USB_LOAD_CFG		0x1365
#define PM8150B_USB_ICL_OPTIONS		0x1366
#define PM8150B_USB_ICL_CFG		0x1370
#define PM8150B_USB_AICL_OPTIONS	0x1380
#define PM8150B_WDOG_PET		0x1643
#define PM8150B_AICL_CMD		0x1644
#define PM8150B_SMB_EN_CMD		0x1648
#define PM8150B_WDOG_CFG		0x1651
#define PM8150B_WDOG_TIMEOUT_CFG	0x1653
#define PM8150B_AICL_RERUN_TIME_CFG	0x1661
#define PM8150B_THERMREG_SRC_CFG	0x1670
#define PM8150B_SMB_CFG			0x1690
#define PM8150B_FG_MONOTONIC_SOC	0x4009
#define PM8150B_FG_VBATT		0x41a0
#define PM8150B_FG_IBATT		0x41a2
#define PM8150B_FG_VBATT_COPY		0x41a6
#define PM8150B_FG_IBATT_COPY		0x41a8
#define PM8150B_FG_ESR_FAST_CRG_CFG	0x416a
#define PM8150B_FG_CNV_CHAR_CFG	0x41b7
#define PM8150B_FG_PEEK_MUX4		0x41ee
#define PM8150B_FG_PEEK_RD		0x41ef
#define PM8150B_FG_MEM_INT_RT_STS	0x4310
#define PM8150B_FG_MEM_ARB_CFG		0x4340
#define PM8150B_FG_MEM_INTF_CFG	0x4350
#define PM8150B_FG_DMA_CTL		0x4371
#define PM8150B_FG_BATT_TEMP		0x4858

#define PM8150B_CHGR_STATUS_MASK	GENMASK(2, 0)
#define PM8150B_CHGR_BAT_OV		BIT(1)
#define PM8150B_CHARGING_ENABLE		BIT(0)
#define PM8150B_RECHG_MASK		GENMASK(2, 1)
#define PM8150B_SOC_BASED_RECHG		GENMASK(2, 1)
#define PM8150B_RCHG_SAMPLE_MASK	GENMASK(3, 2)
#define PM8150B_RERUN_AICL		BIT(0)
#define PM8150B_USBIN_SUSPEND		BIT(0)
#define PM8150B_BAT_TEMP_TOO_HOT	BIT(3)
#define PM8150B_BAT_TEMP_TOO_COLD	BIT(2)
#define PM8150B_USE_USBIN		BIT(4)
#define PM8150B_USBIN_SUSPEND_STS	BIT(6)
#define PM8150B_VALID_INPUT		BIT(0)
#define PM8150B_BAT_TERMINAL_MISSING	BIT(5)
#define PM8150B_BAT_THERM_MISSING	BIT(4)
#define PM8150B_APSD_DONE		BIT(0)
#define PM8150B_APSD_SDP		BIT(0)
#define PM8150B_APSD_DCP		BIT(3)
#define PM8150B_APSD_CDP		BIT(2)
#define PM8150B_APSD_OCP		BIT(1)
#define PM8150B_USB_PLUGIN		BIT(4)
#define PM8150B_ICL_OVERRIDE		BIT(0)
#define PM8150B_FORCE_5V		BIT(0)
#define PM8150B_FORCE_9V		BIT(1)
#define PM8150B_ALLOW_5V_OR_9V		0x03
#define PM8150B_HVDCP_AUTH_ENABLE	BIT(6)
#define PM8150B_HVDCP_AUTONOMOUS_ENABLE	BIT(5)
#define PM8150B_HVDCP_ENABLE		BIT(2)
#define PM8150B_HVDCP_MASK		(PM8150B_HVDCP_AUTH_ENABLE | \
					 PM8150B_HVDCP_AUTONOMOUS_ENABLE | \
					 PM8150B_HVDCP_ENABLE)
#define PM8150B_ICL_OVERRIDE_AFTER_APSD	BIT(4)
#define PM8150B_USB3P0_SEL		BIT(2)
#define PM8150B_USB51_MODE		BIT(1)
#define PM8150B_USBIN_MODE_CHG		BIT(0)
#define PM8150B_SUSPEND_ON_COLLAPSE	BIT(7)
#define PM8150B_AICL_PERIODIC_RERUN	BIT(4)
#define PM8150B_AICL_ADC_ENABLE		BIT(3)
#define PM8150B_AICL_ENABLE		BIT(2)
#define PM8150B_HARD_JEITA_ENABLE	BIT(4)
#define PM8150B_SOFT_JEITA_MASK		GENMASK(3, 0)
#define PM8150B_CHGR_ADC_TERM_SAMPLE_COUNT	BIT(0)
#define PM8150B_THERMREG_SW_ICL_ADJUST	BIT(7)
#define PM8150B_THERMREG_MITIGATION_MASK	(PM8150B_THERMREG_SW_ICL_ADJUST | \
					 GENMASK(4, 0))
#define PM8150B_WDOG_TRIGGER_AFP	BIT(7)
#define PM8150B_WDOG_BARK_IRQ_ENABLE	BIT(6)
#define PM8150B_WDOG_ENABLE_ON_PLUGIN	BIT(1)
#define PM8150B_WDOG_PET_BIT		BIT(0)
#define PM8150B_SMB_EN_OVERRIDE_VALUE	BIT(4)
#define PM8150B_SMB_EN_OVERRIDE		BIT(3)
#define PM8150B_EN_STAT_CMD		BIT(2)
#define PM8150B_SMB_EN_SEL		BIT(4)
#define PM8150B_AICL_RERUN_12S		0x01
#define PM8150B_FSW_5V			0x0f
#define PM8150B_FSW_6V_8V		0x0b
#define PM8150B_FSW_9V			0x08
#define PM8150B_FG_ALG_ACTIVE_PEEK_CFG	0xac
#define PM8150B_FG_ALG_ACTIVE		BIT(3)
#define PM8150B_FG_MEM_GRANT		BIT(3)
#define PM8150B_FG_MEM_ARB_REQUEST	BIT(0)
#define PM8150B_FG_LOW_LATENCY		BIT(1)
#define PM8150B_FG_CLEAR_LOG		BIT(2)
#define PM8150B_FG_ESR_FCC_MASK		(GENMASK(7, 4) | BIT(0))
#define PM8150B_FG_ESR_FCC_PARALLEL	BIT(0)
#define PM8150B_FG_ESR_FCC_SINGLE	BIT(5)
#define PM8150B_FG_SMB_MEASURE_EN	BIT(2)
#define PM8150B_FG_MEM_ACCESS_REQUEST	BIT(7)
#define PM8150B_FG_IACS_SELECT		BIT(5)
#define PM8150B_FG_ADDR_KIND		BIT(1)

#define PM8150B_FCC_STEP_UA		50000
#define PM8150B_FV_MIN_UV		3600000
#define PM8150B_FV_STEP_UV		10000
#define PM8150B_PARALLEL_FV_DELTA_UV	50000
#define PM8150B_USB_ICL_STEP_UA		50000
#define PM8150B_USB_ICL_UNKNOWN_UA	100000
#define PM8150B_USB_ICL_USB3_MIN_UA	150000
#define PM8150B_USB_ICL_SDP_INITIAL_UA	100000
#define PM8150B_USB_ICL_SDP_UA		500000
#define PM8150B_USB_ICL_USB3_MAX_UA	900000
#define PM8150B_USB_ICL_CDP_UA		1500000
#define PM8150B_USB_ICL_DCP_UA		1500000
#define PM8150B_USB_ICL_FAST_5V_UA	3000000
#define PM8150B_USB_ICL_FAST_9V_UA	1650000
#define PM8150B_USB_VOLTAGE_5V_UV	5000000
#define PM8150B_USB_VOLTAGE_9V_UV	9000000
#define PM8150B_PD_SETTLE_MS		500
#define PM8150B_POLICY_NORMAL_MS		5000
#define PM8150B_PARALLEL_ENABLE_DELAY_MS	30000
#define PM8150B_PARALLEL_AICL_SETTLE_MS	3000
#define PM8150B_POLICY_FCC_STEP_UA	100000
#define PM8150B_TAPER_STEP_MS		500
#define PM8150B_TAPER_STEP_UA		200000
#define PM8150B_BATVOLT_LIMIT_UV		4250000
#define PM8150B_BATVOLT_CLEAR_UV		4200000
#define PM8150B_BATVOLT_IBAT_PERCENT	60
#define PM8150B_BATVOLT_CV_IBAT_PERCENT	30
#define PM8150B_AICL_5V_MIN_MV		4000
#define PM8150B_AICL_5V_MAX_MV		4700
#define PM8150B_AICL_CONT_MIN_MV		4000
#define PM8150B_AICL_CONT_MAX_MV		11800
#define PM8150B_AICL_STORM_STEP_MV	200
#define PM8150B_AICL_STORM_MAX_MV	4800
#define PM8150B_UV_STORM_PERIOD_MS	3000
#define PM8150B_UV_STORM_COUNT		5
#define PM8150B_PARALLEL_MIN_FCC_UA	500000
#define PM8150B_PARALLEL_MIN_TOTAL_ICL_UA	1400000
#define PM8150B_PARALLEL_EFFICIENCY_PCT	80
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

enum pm8150b_temp_zone {
	PM8150B_TEMP_COLD,
	PM8150B_TEMP_COOL,
	PM8150B_TEMP_NORMAL,
	PM8150B_TEMP_WARM,
	PM8150B_TEMP_HOT,
};

struct pm8150b_policy_snapshot {
	int usb_type;
	int source_current_max_ua;
	int battery_temp_decic;
	int battery_voltage_uv;
	bool aicl_max_reached;
	int phase;
	bool present;
	bool online;
	bool battery_temp_limited;
};

struct pm8150b_policy_target {
	int input_current_ua;
	int fcc_ua;
	int fv_uv;
	bool disable_charging;
	bool suspend_input;
};

struct pm8150b_charger {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *usb_in_i;
	struct iio_channel *mid_chg;
	struct power_supply *battery;
	struct power_supply *parallel;
	struct power_supply *usb;
	struct fwnode_handle *typec_fwnode;
	struct notifier_block psy_nb;
	struct delayed_work policy_work;
	/* Serialize Gen4 SRAM DMA arbitration. */
	struct mutex fg_lock;
	/* Serialize ADC channel selection and reads. */
	struct mutex adc_lock;
	/* Serialize USB-C, PD, and PM8150B input limit updates. */
	struct mutex usb_lock;
	bool typec_online;
	bool pd_active;
	bool shutting_down;
	bool input_present;
	int aicl_5v_threshold_mv;
	int aicl_cont_threshold_mv;
	int default_aicl_5v_threshold_mv;
	int default_aicl_cont_threshold_mv;
	unsigned int usbin_uv_count;
	ktime_t usbin_uv_last;
	bool aicl_max_reached;
	int typec_current_max_ua;
	/* Gadget reset/configuration limits apply only while the source is SDP. */
	int gadget_current_limit_ua;
	bool gadget_current_limit_valid;
	int applied_icl_ua;
	int applied_fcc_ua;
	int applied_fv_uv;
	int policy_fcc_ua;
	int parallel_fcc_ua;
	int parallel_fv_uv;
	int bvp_fcc_ua;
	int parallel_percent;
	unsigned long parallel_enable_after;
	unsigned long bvp_next_step;
	unsigned long taper_next_step;
	int profile_fcc_ua;
	int profile_fv_uv;
	int taper_entry_fv_uv;
	int taper_fcc_ua;
	int usb_voltage_max_uv;
	int auto_recharge_soc;
	int charge_phase;
	int fg_smb_measure_enabled;
	bool fg_dma_alg_wait;
	enum pm8150b_temp_zone temp_zone;
	bool charge_phase_valid;
	bool charge_state_pending;
	bool taper_active;
	bool taper_step_pending;
	bool charging_enabled;
	bool charging_state_valid;
	bool parallel_enabled;
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

	/*
	 * PM8150B v1.0 cannot grant SRAM access while the fuel gauge algorithm
	 * runs; the ESR pulse holds it for up to ~320 ms.
	 */
	if (chip->fg_dma_alg_wait) {
		ret = regmap_read_poll_timeout(chip->regmap,
					       PM8150B_FG_PEEK_RD, val,
					       !(val & PM8150B_FG_ALG_ACTIVE),
					       10000, 350000);
		if (ret)
			return ret;
		usleep_range(1000, 1100);
	}

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
	const struct qcom_spmi_pmic *pmic;
	int ret;

	pmic = qcom_pmic_get(chip->dev);
	if (IS_ERR(pmic))
		return PTR_ERR(pmic);
	chip->fg_dma_alg_wait = pmic->major == 1;

	ret = regmap_update_bits(chip->regmap, PM8150B_FG_DMA_CTL,
				 PM8150B_FG_ADDR_KIND, PM8150B_FG_ADDR_KIND);
	if (ret)
		return ret;

	ret = pm8150b_fg_release(chip);
	if (ret)
		return ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_FG_MEM_ARB_CFG,
				 PM8150B_FG_LOW_LATENCY |
				 PM8150B_FG_CLEAR_LOG,
				 PM8150B_FG_LOW_LATENCY);
	if (ret)
		return ret;

	/* ALG_ACTIVE on PEEK_RD is needed by the v1.0 DMA wait above. */
	return regmap_write(chip->regmap, PM8150B_FG_PEEK_MUX4,
			    PM8150B_FG_ALG_ACTIVE_PEEK_CFG);
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

static int pm8150b_fg_set_smb_measure(struct pm8150b_charger *chip,
				      bool enable)
{
	int ret, ret2;

	if (chip->fg_smb_measure_enabled == enable)
		return 0;

	mutex_lock(&chip->fg_lock);
	ret = pm8150b_fg_request(chip);
	if (!ret)
		ret = regmap_update_bits(chip->regmap,
					 PM8150B_FG_CNV_CHAR_CFG,
					 PM8150B_FG_SMB_MEASURE_EN,
					 enable ? PM8150B_FG_SMB_MEASURE_EN : 0);
	ret2 = pm8150b_fg_release(chip);
	mutex_unlock(&chip->fg_lock);
	if (ret)
		return ret;
	if (ret2)
		return ret2;

	chip->fg_smb_measure_enabled = enable;
	return 0;
}

static int pm8150b_fg_read_shadow(struct pm8150b_charger *chip,
				  unsigned int reg,
				  unsigned int copy_reg, u16 *raw)
{
	u8 buf[2], copy[2];
	int i, ret;

	mutex_lock(&chip->fg_lock);
	for (i = 0; i < 3; i++) {
		ret = regmap_bulk_read(chip->regmap, reg, buf, sizeof(buf));
		if (ret)
			goto unlock;
		ret = regmap_bulk_read(chip->regmap, copy_reg, copy,
				       sizeof(copy));
		if (ret)
			goto unlock;
		if (!memcmp(buf, copy, sizeof(buf))) {
			*raw = get_unaligned_le16(buf);
			ret = 0;
			goto unlock;
		}
	}

	ret = -EIO;
unlock:
	mutex_unlock(&chip->fg_lock);
	return ret;
}

static int pm8150b_fg_capacity(struct pm8150b_charger *chip, int *capacity)
{
	u8 raw[2];
	int i, ret;

	mutex_lock(&chip->fg_lock);
	for (i = 0; i < 3; i++) {
		ret = regmap_bulk_read(chip->regmap, PM8150B_FG_MONOTONIC_SOC,
				       raw, sizeof(raw));
		if (ret)
			goto unlock;
		if (raw[0] == raw[1])
			break;
	}
	if (i == 3) {
		ret = -EIO;
		goto unlock;
	}

	if (raw[0] == 255)
		*capacity = 100;
	else if (!raw[0])
		*capacity = 0;
	else
		*capacity = DIV_ROUND_CLOSEST((raw[0] - 1) * 98, 253) + 1;

	ret = 0;
unlock:
	mutex_unlock(&chip->fg_lock);
	return ret;
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

static int pm8150b_read_adc(struct pm8150b_charger *chip,
			    struct iio_channel *channel, int *value)
{
	int ret;

	mutex_lock(&chip->adc_lock);
	ret = iio_read_channel_processed(channel, value);
	mutex_unlock(&chip->adc_lock);

	return ret;
}

static int pm8150b_read_mid_voltage(struct pm8150b_charger *chip, int *value)
{
	int present, ret;

	mutex_lock(&chip->adc_lock);
	ret = pm8150b_usb_present(chip, &present);
	if (ret)
		goto unlock;
	if (!present) {
		*value = 0;
		ret = 0;
		goto unlock;
	}

	ret = iio_read_channel_processed(chip->mid_chg, value);
	if (!ret && present && *value < 1000000)
		*value = READ_ONCE(chip->pd_active) ?
			READ_ONCE(chip->usb_voltage_max_uv) :
			PM8150B_USB_VOLTAGE_5V_UV;

unlock:
	mutex_unlock(&chip->adc_lock);

	return ret;
}

static int pm8150b_usb_source(struct pm8150b_charger *chip, int *type,
			      int *current_max_ua)
{
	unsigned int apsd_status, val;
	int online, ret;

	ret = pm8150b_usb_online(chip, &online);
	if (ret || !online) {
		*type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		*current_max_ua = PM8150B_USB_ICL_UNKNOWN_UA;
		return ret;
	}
	if (chip->pd_active) {
		*type = POWER_SUPPLY_USB_TYPE_PD;
		*current_max_ua = chip->typec_current_max_ua;
		return 0;
	}

	ret = regmap_read(chip->regmap, PM8150B_APSD_STATUS, &apsd_status);
	if (ret)
		return ret;
	if (!(apsd_status & PM8150B_APSD_DONE)) {
		*type = chip->typec_online ? POWER_SUPPLY_USB_TYPE_C :
			POWER_SUPPLY_USB_TYPE_UNKNOWN;
		*current_max_ua = chip->typec_online ?
			chip->typec_current_max_ua : PM8150B_USB_ICL_UNKNOWN_UA;
		return 0;
	}

	ret = regmap_read(chip->regmap, PM8150B_APSD_RESULT, &val);
	if (ret)
		return ret;
	if (val & PM8150B_APSD_CDP) {
		*type = POWER_SUPPLY_USB_TYPE_CDP;
		*current_max_ua = PM8150B_USB_ICL_CDP_UA;
	} else if (val & (PM8150B_APSD_DCP | PM8150B_APSD_OCP)) {
		*type = POWER_SUPPLY_USB_TYPE_DCP;
		*current_max_ua = PM8150B_USB_ICL_DCP_UA;
	} else if (val & PM8150B_APSD_SDP) {
		*type = POWER_SUPPLY_USB_TYPE_SDP;
		*current_max_ua = PM8150B_USB_ICL_SDP_UA;
	} else {
		*type = POWER_SUPPLY_USB_TYPE_UNKNOWN;
		*current_max_ua = PM8150B_USB_ICL_UNKNOWN_UA;
	}
	/*
	 * A Type-C source advertising Rp med/high may supply more than the
	 * BC1.2 result, so take the higher of the two.
	 */
	if (chip->typec_online)
		*current_max_ua = max(*current_max_ua,
				      chip->typec_current_max_ua);
	return 0;
}

static int pm8150b_rerun_aicl(struct pm8150b_charger *chip);

/*
 * USB51 mode only encodes the four BC1.2 SDP steps. Any other limit has to use
 * high-current mode, where USBIN_MODE_CHG keeps USB_ICL_CFG in control.
 */
static bool pm8150b_icl_is_usb51_step(int current_ua, unsigned int *icl_options)
{
	switch (current_ua) {
	case PM8150B_USB_ICL_UNKNOWN_UA:
		*icl_options = 0;
		return true;
	case PM8150B_USB_ICL_USB3_MIN_UA:
		*icl_options = PM8150B_USB3P0_SEL;
		return true;
	case PM8150B_USB_ICL_SDP_UA:
		*icl_options = PM8150B_USB51_MODE;
		return true;
	case PM8150B_USB_ICL_USB3_MAX_UA:
		*icl_options = PM8150B_USB3P0_SEL | PM8150B_USB51_MODE;
		return true;
	default:
		return false;
	}
}

static int pm8150b_set_usb_icl(struct pm8150b_charger *chip, int current_ua)
{
	unsigned int icl_options;
	int old_icl_ua = chip->applied_icl_ua;
	int ret;

	current_ua = clamp(current_ua, 0, PM8150B_USB_ICL_FAST_5V_UA);
	current_ua = rounddown(current_ua, PM8150B_USB_ICL_STEP_UA);
	if (current_ua == chip->applied_icl_ua)
		return 0;
	ret = regmap_write(chip->regmap, PM8150B_USB_ICL_CFG,
			   current_ua / PM8150B_USB_ICL_STEP_UA);
	if (ret)
		return ret;

	if (pm8150b_icl_is_usb51_step(current_ua, &icl_options)) {
		ret = regmap_update_bits(chip->regmap, PM8150B_USB_ICL_OPTIONS,
					 PM8150B_USB3P0_SEL |
					 PM8150B_USB51_MODE |
					 PM8150B_USBIN_MODE_CHG, icl_options);
		if (ret)
			return ret;
		ret = regmap_update_bits(chip->regmap,
					 PM8150B_CMD_ICL_OVERRIDE,
					 PM8150B_ICL_OVERRIDE,
					 PM8150B_ICL_OVERRIDE);
		if (ret)
			return ret;

		ret = regmap_update_bits(chip->regmap, PM8150B_USB_LOAD_CFG,
					 PM8150B_ICL_OVERRIDE_AFTER_APSD, 0);
		if (ret)
			return ret;
		chip->applied_icl_ua = current_ua;

		/* USB51 mode always needs AICL to re-evaluate the input. */
		return pm8150b_rerun_aicl(chip);
	}

	ret = regmap_update_bits(chip->regmap, PM8150B_USB_ICL_OPTIONS,
				 PM8150B_USBIN_MODE_CHG,
				 PM8150B_USBIN_MODE_CHG);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_CMD_ICL_OVERRIDE,
				 PM8150B_ICL_OVERRIDE, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_USB_LOAD_CFG,
				 PM8150B_ICL_OVERRIDE_AFTER_APSD,
				 PM8150B_ICL_OVERRIDE_AFTER_APSD);
	if (ret)
		return ret;
	chip->applied_icl_ua = current_ua;

	/* A settled input below the new limit only recovers after a rerun. */
	if (current_ua > old_icl_ua)
		ret = pm8150b_rerun_aicl(chip);

	return ret;
}

static int pm8150b_aicl_cont_from_raw(unsigned int raw)
{
	if (raw >= 16)
		return 5600 + (raw - 16) * 200;

	return PM8150B_AICL_CONT_MIN_MV + raw * 100;
}

static unsigned int pm8150b_aicl_cont_to_raw(int threshold_mv)
{
	if (threshold_mv >= 5600)
		return 16 + (threshold_mv - 5600) / 200;

	return (threshold_mv - PM8150B_AICL_CONT_MIN_MV) / 100;
}

static int pm8150b_set_aicl_thresholds(struct pm8150b_charger *chip,
				       int threshold_5v_mv,
				       int threshold_cont_mv)
{
	int reg_5v_mv, reg_cont_mv;
	int ret;

	reg_5v_mv = clamp(threshold_5v_mv, PM8150B_AICL_5V_MIN_MV,
			  PM8150B_AICL_5V_MAX_MV);
	reg_cont_mv = clamp(threshold_cont_mv, PM8150B_AICL_CONT_MIN_MV,
			    PM8150B_AICL_CONT_MAX_MV);
	ret = regmap_write(chip->regmap, PM8150B_AICL_5V_THRESHOLD,
			   (reg_5v_mv - PM8150B_AICL_5V_MIN_MV) / 100);
	if (ret)
		return ret;
	ret = regmap_write(chip->regmap, PM8150B_AICL_CONT_THRESHOLD,
			   pm8150b_aicl_cont_to_raw(reg_cont_mv));
	if (!ret) {
		/*
		 * Track the requested value, not the register clamp, so the UV
		 * storm walk terminates after the same number of steps as the
		 * hardware allows.
		 */
		chip->aicl_5v_threshold_mv = threshold_5v_mv;
		chip->aicl_cont_threshold_mv = threshold_cont_mv;
	}

	return ret;
}

static int pm8150b_read_aicl_thresholds(struct pm8150b_charger *chip)
{
	unsigned int raw_5v, raw_cont;
	int ret;

	ret = regmap_read(chip->regmap, PM8150B_AICL_5V_THRESHOLD,
			  &raw_5v);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, PM8150B_AICL_CONT_THRESHOLD,
			  &raw_cont);
	if (ret)
		return ret;

	chip->default_aicl_5v_threshold_mv =
		clamp(PM8150B_AICL_5V_MIN_MV + (int)raw_5v * 100,
		      PM8150B_AICL_5V_MIN_MV, PM8150B_AICL_5V_MAX_MV);
	chip->default_aicl_cont_threshold_mv =
		clamp(pm8150b_aicl_cont_from_raw(raw_cont),
		      PM8150B_AICL_CONT_MIN_MV, PM8150B_AICL_CONT_MAX_MV);
	chip->aicl_5v_threshold_mv = chip->default_aicl_5v_threshold_mv;
	chip->aicl_cont_threshold_mv = chip->default_aicl_cont_threshold_mv;

	return 0;
}

static void pm8150b_reset_aicl_state(struct pm8150b_charger *chip)
{
	int ret;

	chip->usbin_uv_count = 0;
	chip->usbin_uv_last = 0;
	chip->aicl_max_reached = false;
	ret = pm8150b_set_aicl_thresholds(chip,
					  chip->default_aicl_5v_threshold_mv,
					  chip->default_aicl_cont_threshold_mv);
	if (ret)
		dev_warn(chip->dev, "Failed to restore AICL thresholds: %d\n", ret);
}

static int pm8150b_set_fcc(struct pm8150b_charger *chip, int current_ua)
{
	int ret;

	current_ua = clamp(current_ua, PM8150B_FCC_STEP_UA,
			   chip->profile_fcc_ua);
	current_ua = rounddown(current_ua, PM8150B_FCC_STEP_UA);
	if (current_ua == chip->applied_fcc_ua)
		return 0;
	ret = regmap_write(chip->regmap, PM8150B_CHGR_FCC_CFG,
			   current_ua / PM8150B_FCC_STEP_UA);
	if (!ret)
		chip->applied_fcc_ua = current_ua;

	return ret;
}

static int pm8150b_set_fv(struct pm8150b_charger *chip, int voltage_uv)
{
	int ret;

	voltage_uv = clamp(voltage_uv, PM8150B_FV_MIN_UV,
			   chip->profile_fv_uv);
	voltage_uv = rounddown(voltage_uv - PM8150B_FV_MIN_UV,
			       PM8150B_FV_STEP_UV) + PM8150B_FV_MIN_UV;
	if (voltage_uv == chip->applied_fv_uv)
		return 0;
	ret = regmap_write(chip->regmap, PM8150B_CHGR_FV_CFG,
			   (voltage_uv - PM8150B_FV_MIN_UV) /
			   PM8150B_FV_STEP_UV);
	if (!ret)
		chip->applied_fv_uv = voltage_uv;

	return ret;
}

static int pm8150b_rerun_aicl(struct pm8150b_charger *chip)
{
	unsigned int val;
	int ret;

	/* Rerunning AICL while the input is suspended latches a zero limit. */
	ret = regmap_read(chip->regmap, PM8150B_POWER_PATH_STATUS, &val);
	if (ret)
		return ret;
	if (val & PM8150B_USBIN_SUSPEND_STS)
		return 0;

	return regmap_update_bits(chip->regmap, PM8150B_AICL_CMD,
				  PM8150B_RERUN_AICL,
				  PM8150B_RERUN_AICL);
}

static int pm8150b_set_fsw(struct pm8150b_charger *chip, int voltage_uv)
{
	unsigned int fsw;

	if (voltage_uv <= PM8150B_USB_VOLTAGE_5V_UV)
		fsw = PM8150B_FSW_5V;
	else if (voltage_uv < PM8150B_USB_VOLTAGE_9V_UV)
		fsw = PM8150B_FSW_6V_8V;
	else
		fsw = PM8150B_FSW_9V;

	return regmap_write(chip->regmap, PM8150B_DCDC_FSW_SEL, fsw);
}

static int pm8150b_set_pd_input_voltage(struct pm8150b_charger *chip,
					int voltage_uv)
{
	int ret;

	ret = pm8150b_set_fsw(chip, voltage_uv);
	if (ret)
		return ret;

	return regmap_write(chip->regmap, PM8150B_USB_ADAPTER_ALLOW_OVERRIDE,
			    voltage_uv > PM8150B_USB_VOLTAGE_5V_UV ?
			    PM8150B_FORCE_9V : PM8150B_FORCE_5V);
}

static int pm8150b_configure_recharge(struct pm8150b_charger *chip)
{
	unsigned int cfg2, samples, soc;
	unsigned int raw_soc = DIV_ROUND_CLOSEST(chip->auto_recharge_soc * 255,
						 100);
	int ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_CHGR_CFG2,
				 PM8150B_RECHG_MASK, PM8150B_SOC_BASED_RECHG);
	if (ret)
		return ret;
	ret = regmap_write(chip->regmap, PM8150B_CHGR_RCHG_SOC_CFG, raw_soc);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_CHGR_TERM_RCHG_CFG,
				 PM8150B_RCHG_SAMPLE_MASK, 0);
	if (ret)
		return ret;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_CFG2, &cfg2);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, PM8150B_CHGR_RCHG_SOC_CFG, &soc);
	if (ret)
		return ret;
	ret = regmap_read(chip->regmap, PM8150B_CHGR_TERM_RCHG_CFG, &samples);
	if (ret)
		return ret;
	if ((cfg2 & PM8150B_RECHG_MASK) != PM8150B_SOC_BASED_RECHG ||
	    soc != raw_soc || (samples & PM8150B_RCHG_SAMPLE_MASK))
		return -EIO;

	return 0;
}

static int pm8150b_parallel_set(struct pm8150b_charger *chip,
				enum power_supply_property property, int value)
{
	union power_supply_propval val = { .intval = value };

	return power_supply_set_property(chip->parallel, property, &val);
}

static int pm8150b_set_esr_fcc_control(struct pm8150b_charger *chip,
				       bool parallel)
{
	int ret;

	mutex_lock(&chip->fg_lock);
	ret = regmap_update_bits(chip->regmap, PM8150B_FG_ESR_FAST_CRG_CFG,
				 PM8150B_FG_ESR_FCC_MASK,
				 parallel ? PM8150B_FG_ESR_FCC_PARALLEL :
				 PM8150B_FG_ESR_FCC_SINGLE);
	mutex_unlock(&chip->fg_lock);

	return ret;
}

static int pm8150b_disable_parallel(struct pm8150b_charger *chip)
{
	int ret;

	if (!chip->parallel_enabled)
		return pm8150b_set_esr_fcc_control(chip, false);

	ret = pm8150b_parallel_set(chip, POWER_SUPPLY_PROP_ONLINE, 0);
	if (!ret) {
		chip->parallel_enabled = false;
		chip->parallel_fcc_ua = 0;
		chip->parallel_fv_uv = 0;
		if (chip->input_present)
			chip->parallel_enable_after = jiffies +
				msecs_to_jiffies(PM8150B_PARALLEL_AICL_SETTLE_MS);
	}
	if (!ret)
		ret = pm8150b_set_esr_fcc_control(chip, false);

	return ret;
}

static int pm8150b_apply_conservative_limits(struct pm8150b_charger *chip)
{
	int ret, ret2;

	ret = pm8150b_set_fcc(chip, PM8150B_PARALLEL_MIN_FCC_UA);
	ret2 = pm8150b_set_usb_icl(chip, PM8150B_USB_ICL_UNKNOWN_UA);
	if (!ret)
		ret = ret2;

	return ret;
}

static void pm8150b_reset_taper(struct pm8150b_charger *chip)
{
	chip->taper_active = false;
	chip->taper_step_pending = false;
	chip->taper_entry_fv_uv = 0;
	chip->taper_fcc_ua = 0;
	chip->taper_next_step = 0;
}

static void pm8150b_apply_taper(struct pm8150b_charger *chip,
				const struct pm8150b_policy_snapshot *s,
				struct pm8150b_policy_target *target)
{
	int parallel_fcc_ua;

	chip->taper_step_pending = false;
	if (!s->present) {
		pm8150b_reset_taper(chip);
		return;
	}

	if (chip->taper_active && target->fv_uv > chip->taper_entry_fv_uv) {
		pm8150b_reset_taper(chip);
		return;
	}

	if (s->phase == PM8150B_TAPER_CHARGE) {
		if (!chip->taper_active) {
			chip->taper_active = true;
			chip->taper_entry_fv_uv = target->fv_uv;
			chip->taper_fcc_ua = chip->profile_fcc_ua;
			chip->taper_next_step = jiffies;
		}
		if (time_after_eq(jiffies, chip->taper_next_step)) {
			chip->taper_fcc_ua = max(chip->taper_fcc_ua -
						    PM8150B_TAPER_STEP_UA, 0);
			chip->taper_next_step = jiffies +
				msecs_to_jiffies(PM8150B_TAPER_STEP_MS);
		}
	} else if (!chip->taper_active) {
		return;
	} else if (s->phase == PM8150B_FULLON_CHARGE) {
		chip->taper_next_step = 0;
	} else {
		if (!chip->taper_next_step)
			chip->taper_next_step = jiffies +
				msecs_to_jiffies(PM8150B_TAPER_STEP_MS);
		if (time_after_eq(jiffies, chip->taper_next_step)) {
			pm8150b_reset_taper(chip);
			return;
		}
		chip->taper_step_pending = true;
	}

	target->fcc_ua = min(target->fcc_ua,
			     max(chip->taper_fcc_ua, PM8150B_FCC_STEP_UA));
	parallel_fcc_ua = target->fcc_ua * chip->parallel_percent / 100;
	if (s->phase == PM8150B_TAPER_CHARGE)
		chip->taper_step_pending =
			parallel_fcc_ua >= PM8150B_PARALLEL_MIN_FCC_UA;
}

static int pm8150b_apply_limits(struct pm8150b_charger *chip,
				const struct pm8150b_policy_snapshot *s,
				const struct pm8150b_policy_target *target,
				bool allow_parallel)
{
	union power_supply_propval val;
	unsigned int settled_icl_raw;
	u64 battery_current_limit_ua;
	int main_fcc_ua, parallel_fcc_ua;
	int settled_icl_ua, settled_voltage_uv;
	int total_fcc_ua, total_icl_ua, fv_uv;
	int disable_ret, esr_ret, fallback_ret, ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_USBIN_CMD_IL,
				 PM8150B_USBIN_SUSPEND,
				 target->suspend_input ? PM8150B_USBIN_SUSPEND : 0);
	if (ret)
		return ret;
	if (!chip->charging_state_valid ||
	    chip->charging_enabled == target->disable_charging) {
		ret = regmap_update_bits(chip->regmap, PM8150B_CHGR_ENABLE_CMD,
					 PM8150B_CHARGING_ENABLE,
					 target->disable_charging ? 0 :
					 PM8150B_CHARGING_ENABLE);
		if (ret)
			return ret;
		chip->charging_enabled = !target->disable_charging;
		chip->charging_state_valid = true;
	}
	if (target->disable_charging) {
		ret = pm8150b_disable_parallel(chip);
		if (ret) {
			fallback_ret = pm8150b_apply_conservative_limits(chip);
			if (fallback_ret)
				dev_err(chip->dev,
					"Failed to apply conservative limits after disabling charge: %d\n",
					fallback_ret);
			return ret;
		}
	}

	total_fcc_ua = target->fcc_ua;
	total_icl_ua = target->input_current_ua;
	fv_uv = target->fv_uv;

	allow_parallel = allow_parallel && !target->disable_charging &&
		!target->suspend_input && s->present && s->online &&
		time_after_eq(jiffies, chip->parallel_enable_after) &&
		(s->phase == PM8150B_FULLON_CHARGE ||
		 s->phase == PM8150B_TAPER_CHARGE) &&
		!s->battery_temp_limited &&
		total_icl_ua > PM8150B_PARALLEL_MIN_TOTAL_ICL_UA;
	if (!allow_parallel)
		goto single;

	if (chip->parallel_enabled) {
		ret = power_supply_get_property(chip->parallel,
						POWER_SUPPLY_PROP_ONLINE, &val);
		if (ret)
			goto parallel_failed;
		if (!val.intval)
			goto single;
		ret = power_supply_get_property(chip->parallel,
						POWER_SUPPLY_PROP_HEALTH, &val);
		if (ret)
			goto parallel_failed;
		if (val.intval == POWER_SUPPLY_HEALTH_OVERHEAT)
			goto single;
	}

	ret = regmap_read(chip->regmap, PM8150B_AICL_ICL_STATUS,
			  &settled_icl_raw);
	if (ret)
		goto parallel_failed;
	settled_icl_ua = settled_icl_raw * PM8150B_USB_ICL_STEP_UA;
	/* Without a settled input there is no power budget to split. */
	if (!settled_icl_ua)
		goto single;
	ret = pm8150b_read_mid_voltage(chip, &settled_voltage_uv);
	if (ret || settled_voltage_uv <= 0)
		goto parallel_failed;

	/*
	 * SMB1355 shares the MID node with the main path, so the whole input
	 * limit stays on PM8150B and only FCC is split. The settled input
	 * power, derated for converter efficiency, bounds the battery current
	 * the parallel path may take.
	 */
	battery_current_limit_ua = div64_u64((u64)settled_icl_ua *
		settled_voltage_uv * PM8150B_PARALLEL_EFFICIENCY_PCT,
		(u64)fv_uv * 100);
	parallel_fcc_ua = min_t(u64, total_fcc_ua,
				battery_current_limit_ua) *
		chip->parallel_percent / 100;
	parallel_fcc_ua = rounddown(parallel_fcc_ua, PM8150B_FCC_STEP_UA);
	if (parallel_fcc_ua < PM8150B_PARALLEL_MIN_FCC_UA)
		goto single;

	main_fcc_ua = total_fcc_ua - parallel_fcc_ua;

	ret = pm8150b_set_fv(chip, fv_uv);
	if (ret)
		goto parallel_failed;
	if (!chip->parallel_enabled || fv_uv != chip->parallel_fv_uv) {
		/*
		 * Bias the parallel float voltage above the main path so the
		 * main charger, not SMB1355, decides when CV starts.
		 */
		ret = pm8150b_parallel_set(chip,
					   POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX,
					   fv_uv + PM8150B_PARALLEL_FV_DELTA_UV);
		if (ret)
			goto parallel_failed;
		chip->parallel_fv_uv = fv_uv;
	}

	/*
	 * Raise the shrinking side first so the combined FCC never exceeds the
	 * total: main FCC before a growing parallel share, parallel FCC before
	 * a growing main share.
	 */
	if (!chip->parallel_enabled ||
	    parallel_fcc_ua > chip->parallel_fcc_ua) {
		ret = pm8150b_set_fcc(chip, main_fcc_ua);
		if (ret)
			goto parallel_failed;
		ret = pm8150b_parallel_set(chip,
					   POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
					   parallel_fcc_ua);
		if (ret)
			goto parallel_failed;
	} else {
		ret = pm8150b_parallel_set(chip,
					   POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
					   parallel_fcc_ua);
		if (ret)
			goto parallel_failed;
		ret = pm8150b_set_fcc(chip, main_fcc_ua);
		if (ret)
			goto parallel_failed;
	}
	ret = pm8150b_set_esr_fcc_control(chip, true);
	if (ret)
		goto parallel_failed;
	ret = pm8150b_set_usb_icl(chip, total_icl_ua);
	if (ret)
		goto parallel_failed;
	if (!chip->parallel_enabled) {
		ret = pm8150b_parallel_set(chip, POWER_SUPPLY_PROP_ONLINE, 1);
		if (ret)
			goto parallel_failed;
	}

	chip->parallel_enabled = true;
	chip->parallel_fcc_ua = parallel_fcc_ua;
	chip->policy_fcc_ua = total_fcc_ua;
	return 0;

parallel_failed:
	dev_warn(chip->dev, "SMB1355 parallel path failed; falling back to PM8150B: %d\n",
		 ret);
	disable_ret = pm8150b_parallel_set(chip, POWER_SUPPLY_PROP_ONLINE, 0);
	if (!disable_ret) {
		chip->parallel_enabled = false;
		chip->parallel_fcc_ua = 0;
		chip->parallel_fv_uv = 0;
		if (chip->input_present)
			chip->parallel_enable_after = jiffies +
				msecs_to_jiffies(PM8150B_PARALLEL_AICL_SETTLE_MS);
	}
	esr_ret = pm8150b_set_esr_fcc_control(chip, false);
	if (!disable_ret)
		disable_ret = esr_ret;
	if (disable_ret) {
		dev_err(chip->dev,
			"Failed to confirm SMB1355 is disabled: %d\n",
			disable_ret);
		fallback_ret = pm8150b_apply_conservative_limits(chip);
		if (fallback_ret)
			dev_err(chip->dev,
				"Failed to apply conservative single-path limits: %d\n",
				fallback_ret);
		return disable_ret;
	}
	goto single_limits;

single:
	if (!target->disable_charging) {
		ret = pm8150b_disable_parallel(chip);
		if (ret) {
			fallback_ret = pm8150b_apply_conservative_limits(chip);
			if (fallback_ret)
				dev_err(chip->dev,
					"Failed to apply conservative single-path limits: %d\n",
					fallback_ret);
			return ret;
		}
	}

single_limits:
	ret = pm8150b_set_fcc(chip, total_fcc_ua);
	if (ret)
		return ret;
	ret = pm8150b_set_fv(chip, fv_uv);
	if (ret)
		return ret;
	ret = pm8150b_set_usb_icl(chip, total_icl_ua);
	if (!ret)
		chip->policy_fcc_ua = total_fcc_ua;

	return ret;
}

static enum pm8150b_temp_zone
pm8150b_next_temp_zone(enum pm8150b_temp_zone zone, int temp_decic)
{
	if (temp_decic < 0)
		return PM8150B_TEMP_COLD;
	if (temp_decic >= 550)
		return PM8150B_TEMP_HOT;

	switch (zone) {
	case PM8150B_TEMP_COLD:
		return temp_decic >= 30 ? PM8150B_TEMP_COOL : zone;
	case PM8150B_TEMP_COOL:
		return temp_decic >= 120 ? PM8150B_TEMP_NORMAL : zone;
	case PM8150B_TEMP_NORMAL:
		if (temp_decic < 100)
			return PM8150B_TEMP_COOL;
		return temp_decic >= 450 ? PM8150B_TEMP_WARM : zone;
	case PM8150B_TEMP_WARM:
		if (temp_decic < 430)
			return PM8150B_TEMP_NORMAL;
		return zone;
	case PM8150B_TEMP_HOT:
		return temp_decic < 520 ? PM8150B_TEMP_WARM : zone;
	}

	return PM8150B_TEMP_NORMAL;
}

static int pm8150b_read_policy_snapshot(struct pm8150b_charger *chip,
					struct pm8150b_policy_snapshot *s)
{
	unsigned int status7;
	int online, present, ret;

	memset(s, 0, sizeof(*s));
	ret = pm8150b_usb_present(chip, &present);
	if (ret)
		return ret;
	s->present = present;
	ret = pm8150b_usb_online(chip, &online);
	if (ret)
		return ret;
	s->online = online;
	ret = pm8150b_usb_source(chip, &s->usb_type,
				 &s->source_current_max_ua);
	if (ret)
		return ret;
	ret = pm8150b_fg_read_temp(chip, &s->battery_temp_decic);
	if (ret)
		return ret;
	ret = pm8150b_fg_voltage(chip, &s->battery_voltage_uv);
	if (ret)
		return ret;

	s->aicl_max_reached = chip->aicl_max_reached;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_7, &status7);
	if (ret)
		return ret;
	s->battery_temp_limited = status7 &
		(PM8150B_BAT_TEMP_TOO_HOT | PM8150B_BAT_TEMP_TOO_COLD);

	return 0;
}

static int pm8150b_read_charge_phase(struct pm8150b_charger *chip, int *phase)
{
	unsigned int status;
	int ret;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_1, &status);
	if (!ret)
		*phase = status & PM8150B_CHGR_STATUS_MASK;

	return ret;
}

static void pm8150b_calculate_policy(struct pm8150b_charger *chip,
				     const struct pm8150b_policy_snapshot *s,
				     struct pm8150b_policy_target *target)
{
	int batvolt_threshold_ua;

	target->disable_charging = false;
	target->suspend_input = false;
	chip->temp_zone = pm8150b_next_temp_zone(chip->temp_zone,
						 s->battery_temp_decic);
	target->fcc_ua = chip->profile_fcc_ua;
	target->fv_uv = chip->profile_fv_uv;

	switch (chip->temp_zone) {
	case PM8150B_TEMP_COLD:
	case PM8150B_TEMP_HOT:
		target->disable_charging = true;
		break;
	case PM8150B_TEMP_COOL:
		target->fcc_ua = min(target->fcc_ua,
				     s->battery_voltage_uv >= 4000000 ? 500000 :
				     chip->charge_full_design_uah * 30 / 100);
		break;
	case PM8150B_TEMP_WARM:
		target->fcc_ua = min(target->fcc_ua,
				     chip->charge_full_design_uah / 2);
		target->fv_uv = min(target->fv_uv, 4000000);
		break;
	case PM8150B_TEMP_NORMAL:
		break;
	}
	if (s->battery_temp_limited)
		target->disable_charging = true;

	if (s->battery_temp_decic >= 0 && s->battery_temp_decic < 150)
		target->fcc_ua = min(target->fcc_ua,
				     s->battery_voltage_uv >= 4000000 ? 1000000 :
				     chip->charge_full_design_uah / 2);
	if (chip->bvp_next_step &&
	    time_after_eq(jiffies, chip->bvp_next_step)) {
		batvolt_threshold_ua = chip->charge_full_design_uah *
			(s->phase == PM8150B_TAPER_CHARGE ?
			 PM8150B_BATVOLT_CV_IBAT_PERCENT :
			 PM8150B_BATVOLT_IBAT_PERCENT) / 100;
		if (s->battery_voltage_uv <= PM8150B_BATVOLT_LIMIT_UV) {
			if (chip->bvp_fcc_ua &&
			    s->battery_voltage_uv <= PM8150B_BATVOLT_CLEAR_UV)
				chip->bvp_fcc_ua = 0;
		} else if (chip->policy_fcc_ua > batvolt_threshold_ua) {
			chip->bvp_fcc_ua = rounddown(chip->policy_fcc_ua -
						     PM8150B_POLICY_FCC_STEP_UA,
						     PM8150B_POLICY_FCC_STEP_UA);
		}
		chip->bvp_next_step = jiffies +
			msecs_to_jiffies(PM8150B_POLICY_NORMAL_MS);
	}
	if (chip->bvp_fcc_ua)
		target->fcc_ua = min(target->fcc_ua, chip->bvp_fcc_ua);

	target->input_current_ua = s->source_current_max_ua;
	target->input_current_ua = clamp(target->input_current_ua,
					 PM8150B_USB_ICL_UNKNOWN_UA,
					 PM8150B_USB_ICL_FAST_5V_UA);
	target->input_current_ua = rounddown(target->input_current_ua,
					     PM8150B_USB_ICL_STEP_UA);
	if (s->usb_type == POWER_SUPPLY_USB_TYPE_SDP) {
		int gadget_limit = chip->gadget_current_limit_ua;

		/* USB 2.0 SDP is limited to 100 mA until configuration completes. */
		if (!chip->gadget_current_limit_valid)
			gadget_limit = PM8150B_USB_ICL_SDP_INITIAL_UA;
		else
			gadget_limit = clamp(gadget_limit,
					     0,
					     PM8150B_USB_ICL_FAST_5V_UA);
		/* A zero draw request releases the gadget-specific limit. */
		if (gadget_limit > 0) {
			/* The PMIC cannot represent a live limit below 100 mA. */
			gadget_limit = max(gadget_limit,
					   PM8150B_USB_ICL_SDP_INITIAL_UA);
			target->input_current_ua = min(target->input_current_ua,
						gadget_limit);
			target->input_current_ua = rounddown(target->input_current_ua,
						     PM8150B_USB_ICL_STEP_UA);
		}
	}
	if (s->aicl_max_reached)
		target->input_current_ua = PM8150B_USB_ICL_UNKNOWN_UA;

	pm8150b_apply_taper(chip, s, target);
}

/*
 * Leave the input in the state the next boot stage expects: no parallel path,
 * no PD voltage override, 5V switching frequency and a BC1.2 CDP/DCP limit the
 * hardware can sustain without software supervision.
 */
static int pm8150b_restore_input_baseline(struct pm8150b_charger *chip)
{
	int ret, ret2;

	mutex_lock(&chip->usb_lock);
	ret = pm8150b_disable_parallel(chip);
	ret2 = pm8150b_set_usb_icl(chip, PM8150B_USB_ICL_SDP_UA);
	if (!ret)
		ret = ret2;
	ret2 = pm8150b_set_fsw(chip, PM8150B_USB_VOLTAGE_5V_UV);
	if (!ret)
		ret = ret2;
	ret2 = regmap_write(chip->regmap,
			    PM8150B_USB_ADAPTER_ALLOW_OVERRIDE, 0);
	if (!ret)
		ret = ret2;
	mutex_unlock(&chip->usb_lock);

	return ret;
}

static int pm8150b_update_usb_input(struct pm8150b_charger *chip)
{
	struct pm8150b_policy_snapshot s;
	struct pm8150b_policy_target target;
	int ret;

	ret = pm8150b_read_policy_snapshot(chip, &s);
	if (ret)
		return ret;
	if (s.present != chip->input_present) {
		chip->input_present = s.present;
		chip->gadget_current_limit_ua = -1;
		chip->gadget_current_limit_valid = false;
		chip->charge_state_pending = true;
		pm8150b_reset_taper(chip);
		if (s.present) {
			chip->parallel_enable_after = jiffies +
				msecs_to_jiffies(PM8150B_PARALLEL_ENABLE_DELAY_MS);
			chip->bvp_next_step = jiffies;
		} else {
			chip->parallel_enable_after = 0;
			chip->bvp_next_step = 0;
			chip->bvp_fcc_ua = 0;
		}
	}
	if (chip->charge_state_pending || !chip->charge_phase_valid) {
		ret = pm8150b_read_charge_phase(chip, &chip->charge_phase);
		if (ret)
			return ret;
		chip->charge_phase_valid = true;
		chip->charge_state_pending = false;
	}
	s.phase = chip->charge_phase;
	ret = pm8150b_fg_set_smb_measure(chip,
					 s.present &&
					 s.phase != PM8150B_TERMINATE_CHARGE);
	if (ret)
		return ret;
	pm8150b_calculate_policy(chip, &s, &target);

	if (!s.present) {
		chip->gadget_current_limit_ua = -1;
		chip->gadget_current_limit_valid = false;
		pm8150b_reset_aicl_state(chip);
		chip->bvp_next_step = 0;
		chip->bvp_fcc_ua = 0;
		/* Fall back to the BC1.2 unknown-charger limit until APSD runs. */
		target.input_current_ua = PM8150B_USB_ICL_UNKNOWN_UA;
	} else if (s.usb_type != POWER_SUPPLY_USB_TYPE_SDP) {
		/* Non-SDP sources must not inherit a USB gadget limit. */
		chip->gadget_current_limit_ua = -1;
		chip->gadget_current_limit_valid = false;
	}
	if (s.usb_type != POWER_SUPPLY_USB_TYPE_PD)
		chip->usb_voltage_max_uv = PM8150B_USB_VOLTAGE_5V_UV;

	return pm8150b_apply_limits(chip, &s, &target, true);
}

static void pm8150b_policy_work(struct work_struct *work)
{
	struct pm8150b_charger *chip =
		container_of(work, struct pm8150b_charger, policy_work.work);
	unsigned long delay_ms = PM8150B_POLICY_NORMAL_MS;
	int present = 0, ret;

	if (READ_ONCE(chip->shutting_down))
		return;
	mutex_lock(&chip->usb_lock);
	ret = pm8150b_update_usb_input(chip);
	if (chip->taper_step_pending) {
		if (time_after_eq(jiffies, chip->taper_next_step))
			delay_ms = 0;
		else
			delay_ms = jiffies_to_msecs(chip->taper_next_step -
						   jiffies);
	}
	if (chip->input_present &&
	    time_before(jiffies, chip->parallel_enable_after))
		delay_ms = min_t(unsigned long, delay_ms,
				 jiffies_to_msecs(chip->parallel_enable_after -
						   jiffies));
	if (chip->bvp_next_step) {
		if (time_after_eq(jiffies, chip->bvp_next_step))
			delay_ms = 0;
		else
			delay_ms = min_t(unsigned long, delay_ms,
					 jiffies_to_msecs(chip->bvp_next_step -
							   jiffies));
	}
	mutex_unlock(&chip->usb_lock);
	if (ret)
		dev_err(chip->dev, "Failed to update charging policy: %d\n", ret);
	if (pm8150b_usb_present(chip, &present))
		present = 0;

	power_supply_changed(chip->usb);
	power_supply_changed(chip->battery);
	if (!READ_ONCE(chip->shutting_down) && (ret || present))
		mod_delayed_work(system_dfl_wq, &chip->policy_work,
				 msecs_to_jiffies(delay_ms));
}

static int pm8150b_init_protocols(struct pm8150b_charger *chip)
{
	unsigned int aicl_mask = PM8150B_SUSPEND_ON_COLLAPSE |
		PM8150B_AICL_PERIODIC_RERUN | PM8150B_AICL_ADC_ENABLE |
		PM8150B_AICL_ENABLE;
	unsigned int aicl_value = PM8150B_SUSPEND_ON_COLLAPSE |
		PM8150B_AICL_PERIODIC_RERUN | PM8150B_AICL_ENABLE;
	int ret;

	ret = regmap_update_bits(chip->regmap, PM8150B_USB_AICL_OPTIONS,
				 aicl_mask, aicl_value);
	if (ret)
		return ret;
	ret = regmap_write(chip->regmap, PM8150B_AICL_RERUN_TIME_CFG,
			   PM8150B_AICL_RERUN_12S);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_USB_OPTIONS_1_CFG,
				 PM8150B_HVDCP_MASK, 0);
	if (ret)
		return ret;
	ret = regmap_write(chip->regmap,
			   PM8150B_USB_ADAPTER_ALLOW_OVERRIDE, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_SMB_CFG,
				 PM8150B_SMB_EN_SEL, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_SMB_EN_CMD,
				 PM8150B_EN_STAT_CMD |
				 PM8150B_SMB_EN_OVERRIDE_VALUE |
				 PM8150B_SMB_EN_OVERRIDE,
				 PM8150B_EN_STAT_CMD);
	if (ret)
		return ret;
	ret = regmap_write(chip->regmap, PM8150B_USB_ADAPTER_ALLOW_CFG,
			   PM8150B_ALLOW_5V_OR_9V);
	if (ret)
		return ret;

	return pm8150b_set_fsw(chip, PM8150B_USB_VOLTAGE_5V_UV);
}

static bool pm8150b_is_pd_type(int type)
{
	return type == POWER_SUPPLY_USB_TYPE_PD ||
	       type == POWER_SUPPLY_USB_TYPE_PD_DRP ||
	       type == POWER_SUPPLY_USB_TYPE_PD_PPS ||
	       type == POWER_SUPPLY_USB_TYPE_PD_SPR_AVS ||
	       type == POWER_SUPPLY_USB_TYPE_PD_PPS_SPR_AVS;
}

static int pm8150b_typec_notifier(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct pm8150b_charger *chip =
		container_of(nb, struct pm8150b_charger, psy_nb);
	struct power_supply *psy = data;
	union power_supply_propval online, type, voltage, current_max;
	int current_ua, ret;

	if (READ_ONCE(chip->shutting_down))
		return NOTIFY_DONE;
	if (event != PSY_EVENT_PROP_CHANGED || !psy->dev.parent ||
	    dev_fwnode(psy->dev.parent) != chip->typec_fwnode)
		return NOTIFY_DONE;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE,
					&online);
	if (ret)
		return NOTIFY_DONE;
	if (!online.intval) {
		mutex_lock(&chip->usb_lock);
		if (READ_ONCE(chip->shutting_down)) {
			mutex_unlock(&chip->usb_lock);
			return NOTIFY_DONE;
		}
		chip->typec_online = false;
		chip->pd_active = false;
		chip->typec_current_max_ua = 0;
		chip->usb_voltage_max_uv = PM8150B_USB_VOLTAGE_5V_UV;
		ret = regmap_write(chip->regmap,
				   PM8150B_USB_ADAPTER_ALLOW_OVERRIDE, 0);
		if (ret)
			dev_warn(chip->dev, "Failed to clear PD voltage override: %d\n",
				 ret);
		mod_delayed_work(system_dfl_wq, &chip->policy_work, 0);
		mutex_unlock(&chip->usb_lock);
		return NOTIFY_OK;
	}

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_USB_TYPE, &type);
	if (ret)
		return NOTIFY_DONE;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CURRENT_MAX,
					&current_max);
	if (ret)
		current_max.intval = 0;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
					&voltage);
	if (ret)
		voltage.intval = PM8150B_USB_VOLTAGE_5V_UV;

	current_ua = clamp(current_max.intval, 0,
			   PM8150B_USB_ICL_FAST_5V_UA);
	if (voltage.intval > PM8150B_USB_VOLTAGE_5V_UV)
		current_ua = min(current_ua, PM8150B_USB_ICL_FAST_9V_UA);

	mutex_lock(&chip->usb_lock);
	if (READ_ONCE(chip->shutting_down)) {
		mutex_unlock(&chip->usb_lock);
		return NOTIFY_DONE;
	}
	chip->typec_online = true;
	chip->typec_current_max_ua = current_ua;
	chip->pd_active = pm8150b_is_pd_type(type.intval);
	if (chip->pd_active) {
		chip->usb_voltage_max_uv =
			clamp(voltage.intval, PM8150B_USB_VOLTAGE_5V_UV,
			      PM8150B_USB_VOLTAGE_9V_UV);
		ret = pm8150b_set_pd_input_voltage(chip,
						   chip->usb_voltage_max_uv);
		if (ret)
			dev_warn(chip->dev, "Failed to set PD input voltage: %d\n",
				 ret);
	} else {
		chip->usb_voltage_max_uv = PM8150B_USB_VOLTAGE_5V_UV;
		ret = pm8150b_set_fsw(chip, PM8150B_USB_VOLTAGE_5V_UV);
		if (!ret)
			ret = regmap_write(chip->regmap,
					   PM8150B_USB_ADAPTER_ALLOW_OVERRIDE, 0);
		if (ret)
			dev_warn(chip->dev, "Failed to restore non-PD 5V input: %d\n", ret);
	}
	/* Merge contract and charger state notifications during PD voltage changes. */
	mod_delayed_work(system_dfl_wq, &chip->policy_work,
			 msecs_to_jiffies(chip->pd_active ?
					    PM8150B_PD_SETTLE_MS : 0));
	mutex_unlock(&chip->usb_lock);

	return NOTIFY_OK;
}

static void pm8150b_unregister_typec_notifier(void *data)
{
	struct pm8150b_charger *chip = data;

	mutex_lock(&chip->usb_lock);
	chip->shutting_down = true;
	mutex_unlock(&chip->usb_lock);
	power_supply_unreg_notifier(&chip->psy_nb);
	cancel_delayed_work_sync(&chip->policy_work);
}

static void pm8150b_put_typec_fwnode(void *data)
{
	fwnode_handle_put(data);
}

static int pm8150b_sync_typec_supply(struct pm8150b_charger *chip)
{
	struct power_supply *psy;

	psy = power_supply_get_by_reference(dev_fwnode(chip->dev),
					    "qcom,usb-c-port");
	if (IS_ERR(psy))
		return PTR_ERR(psy);
	if (!psy)
		/* Type-C may probe after the charger because DWC3 consumes usb. */
		return 0;

	pm8150b_typec_notifier(&chip->psy_nb, PSY_EVENT_PROP_CHANGED, psy);
	power_supply_put(psy);

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
	enum pm8150b_temp_zone zone;
	unsigned int val;
	int voltage, ret;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_2, &val);
	if (ret)
		return ret;
	if (val & PM8150B_CHGR_BAT_OV) {
		ret = pm8150b_fg_voltage(chip, &voltage);
		if (!ret &&
		    voltage >= chip->constant_charge_voltage_max_uv + 40000) {
			*health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
			return 0;
		}
	}

	ret = regmap_read(chip->regmap, PM8150B_CHGR_STATUS_7, &val);
	if (ret)
		return ret;
	if (val & PM8150B_BAT_TEMP_TOO_COLD) {
		*health = POWER_SUPPLY_HEALTH_COLD;
	} else if (val & PM8150B_BAT_TEMP_TOO_HOT) {
		*health = POWER_SUPPLY_HEALTH_OVERHEAT;
	} else {
		zone = READ_ONCE(chip->temp_zone);
		switch (zone) {
		case PM8150B_TEMP_COLD:
			*health = POWER_SUPPLY_HEALTH_COLD;
			break;
		case PM8150B_TEMP_COOL:
			*health = POWER_SUPPLY_HEALTH_COOL;
			break;
		case PM8150B_TEMP_WARM:
			*health = POWER_SUPPLY_HEALTH_WARM;
			break;
		case PM8150B_TEMP_HOT:
			*health = POWER_SUPPLY_HEALTH_OVERHEAT;
			break;
		case PM8150B_TEMP_NORMAL:
			*health = POWER_SUPPLY_HEALTH_GOOD;
			break;
		}
	}

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
		ret = regmap_read(chip->regmap, PM8150B_CHGR_FCC_CFG, &reg);
		if (!ret)
			val->intval = reg * PM8150B_FCC_STEP_UA;
		return ret;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_VOLTAGE_MAX:
		val->intval = chip->applied_fv_uv;
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
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static int pm8150b_usb_get_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    union power_supply_propval *val)
{
	struct pm8150b_charger *chip = power_supply_get_drvdata(psy);
	unsigned int reg;
	int present, ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		return pm8150b_usb_present(chip, &val->intval);
	case POWER_SUPPLY_PROP_ONLINE:
		return pm8150b_usb_online(chip, &val->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return pm8150b_read_mid_voltage(chip, &val->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = pm8150b_usb_present(chip, &present);
		if (ret || !present) {
			val->intval = 0;
			return ret;
		}
		ret = pm8150b_read_adc(chip, chip->usb_in_i, &val->intval);
		if (!ret)
			val->intval *= 5;
		return ret;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = chip->usb_voltage_max_uv;
		return 0;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = regmap_read(chip->regmap, PM8150B_USB_ICL_CFG, &reg);
		if (!ret)
			val->intval = reg * PM8150B_USB_ICL_STEP_UA;
		return ret;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		ret = regmap_read(chip->regmap, PM8150B_USB_ICL_CFG, &reg);
		if (!ret)
			val->intval = reg * PM8150B_USB_ICL_STEP_UA;
		return ret;
	case POWER_SUPPLY_PROP_USB_TYPE:
		return pm8150b_usb_source(chip, &val->intval, &present);
	default:
		return -EINVAL;
	}
}

static int pm8150b_usb_set_property(struct power_supply *psy,
				    enum power_supply_property psp,
				    const union power_supply_propval *val)
{
	struct pm8150b_charger *chip = power_supply_get_drvdata(psy);

	if (psp != POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT || val->intval < 0)
		return -EINVAL;

	mutex_lock(&chip->usb_lock);
	if (chip->shutting_down) {
		mutex_unlock(&chip->usb_lock);
		return -ESHUTDOWN;
	}
	chip->gadget_current_limit_ua = min(val->intval,
						PM8150B_USB_ICL_FAST_5V_UA);
	chip->gadget_current_limit_valid = true;
	mod_delayed_work(system_dfl_wq, &chip->policy_work, 0);
	mutex_unlock(&chip->usb_lock);

	return 0;
}

static int pm8150b_usb_property_is_writeable(struct power_supply *psy,
					     enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT;
}

static const struct power_supply_desc pm8150b_usb_desc = {
	.name = "usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP) |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP) |
		     BIT(POWER_SUPPLY_USB_TYPE_C) |
		     BIT(POWER_SUPPLY_USB_TYPE_PD),
	.properties = pm8150b_usb_properties,
	.num_properties = ARRAY_SIZE(pm8150b_usb_properties),
	.get_property = pm8150b_usb_get_property,
	.set_property = pm8150b_usb_set_property,
	.property_is_writeable = pm8150b_usb_property_is_writeable,
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
		dev_warn(chip->dev, "Charging current clamped from %u uA to %u uA\n",
			 raw * PM8150B_FCC_STEP_UA,
			 limit * PM8150B_FCC_STEP_UA);
		raw = limit;
	}
	/* Runtime limit registers survive warm reset; they are not the battery profile. */
	chip->profile_fcc_ua = chip->constant_charge_current_max_ua;
	chip->applied_fcc_ua = raw * PM8150B_FCC_STEP_UA;

	ret = regmap_read(chip->regmap, PM8150B_CHGR_FV_CFG, &raw);
	if (ret)
		return ret;
	limit = (chip->constant_charge_voltage_max_uv - PM8150B_FV_MIN_UV) /
		 PM8150B_FV_STEP_UV;
	if (raw > limit) {
		ret = regmap_write(chip->regmap, PM8150B_CHGR_FV_CFG, limit);
		if (ret)
			return ret;
		dev_warn(chip->dev, "Charging voltage clamped from %u uV to %u uV\n",
			 PM8150B_FV_MIN_UV + raw * PM8150B_FV_STEP_UV,
			 PM8150B_FV_MIN_UV + limit * PM8150B_FV_STEP_UV);
		raw = limit;
	}
	chip->profile_fv_uv = chip->constant_charge_voltage_max_uv;
	chip->applied_fv_uv = PM8150B_FV_MIN_UV +
		raw * PM8150B_FV_STEP_UV;

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
				     "Invalid ABL hard JEITA thresholds: hot=%u cold=%u\n",
				     hot, cold);

	/*
	 * Hard JEITA keeps the ABL thresholds as the last-resort cutoff, while
	 * the soft JEITA FCC/FV compensation is left to the software policy.
	 */
	ret = regmap_update_bits(chip->regmap, PM8150B_CHGR_JEITA_CFG,
				 PM8150B_HARD_JEITA_ENABLE |
				 PM8150B_SOFT_JEITA_MASK,
				 PM8150B_HARD_JEITA_ENABLE);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_CHGR_ADC_TERM_CFG,
				 PM8150B_CHGR_ADC_TERM_SAMPLE_COUNT,
				 PM8150B_CHGR_ADC_TERM_SAMPLE_COUNT);
	if (ret)
		return ret;
	ret = regmap_update_bits(chip->regmap, PM8150B_THERMREG_SRC_CFG,
				 PM8150B_THERMREG_MITIGATION_MASK,
				 PM8150B_THERMREG_SW_ICL_ADJUST);
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

static irqreturn_t pm8150b_usb_update_irq(int irq, void *data)
{
	struct pm8150b_charger *chip = data;

	if (!READ_ONCE(chip->shutting_down))
		mod_delayed_work(system_dfl_wq, &chip->policy_work, 0);

	return pm8150b_changed_irq(irq, data);
}

static irqreturn_t pm8150b_charge_state_irq(int irq, void *data)
{
	struct pm8150b_charger *chip = data;

	mutex_lock(&chip->usb_lock);
	if (!chip->shutting_down) {
		chip->charge_state_pending = true;
		mod_delayed_work(system_dfl_wq, &chip->policy_work, 0);
	}
	mutex_unlock(&chip->usb_lock);
	power_supply_changed(chip->battery);

	return IRQ_HANDLED;
}

static irqreturn_t pm8150b_usbin_uv_irq(int irq, void *data)
{
	struct pm8150b_charger *chip = data;
	ktime_t now;
	int present, ret = 0, ret2;

	if (READ_ONCE(chip->shutting_down))
		return IRQ_HANDLED;
	mutex_lock(&chip->usb_lock);
	ret = pm8150b_usb_present(chip, &present);
	if (ret)
		goto out;
	if (!present) {
		pm8150b_reset_aicl_state(chip);
		goto out;
	}
	now = ktime_get_boottime();
	if (chip->usbin_uv_last &&
	    ktime_to_ms(ktime_sub(now, chip->usbin_uv_last)) <
	    PM8150B_UV_STORM_PERIOD_MS)
		chip->usbin_uv_count++;
	else
		chip->usbin_uv_count = 0;
	chip->usbin_uv_last = now;
	if (chip->usbin_uv_count <= PM8150B_UV_STORM_COUNT)
		goto out;

	chip->usbin_uv_count = 0;
	/* Suspend the input while the AICL thresholds are moved. */
	ret = regmap_update_bits(chip->regmap, PM8150B_USBIN_CMD_IL,
				 PM8150B_USBIN_SUSPEND, PM8150B_USBIN_SUSPEND);
	if (ret)
		goto out;
	/* Let VASHDN deglitch before touching the thresholds. */
	msleep(20);
	if (chip->aicl_5v_threshold_mv > PM8150B_AICL_STORM_MAX_MV) {
		chip->aicl_max_reached = true;
	} else {
		ret = pm8150b_set_aicl_thresholds(chip,
						  chip->aicl_5v_threshold_mv +
						  PM8150B_AICL_STORM_STEP_MV,
						  chip->aicl_cont_threshold_mv +
						  PM8150B_AICL_STORM_STEP_MV);
	}
	if (!ret && chip->aicl_max_reached)
		ret = pm8150b_set_usb_icl(chip, PM8150B_USB_ICL_UNKNOWN_UA);
	/* AICL only reruns on a live input, so resume before restarting it. */
	ret2 = regmap_update_bits(chip->regmap, PM8150B_USBIN_CMD_IL,
				  PM8150B_USBIN_SUSPEND, 0);
	if (!ret)
		ret = ret2;
	if (!ret)
		ret = pm8150b_rerun_aicl(chip);
	if (ret)
		dev_warn(chip->dev, "Failed to handle USBIN UV storm: %d\n", ret);

	mod_delayed_work(system_dfl_wq, &chip->policy_work, 0);
out:
	mutex_unlock(&chip->usb_lock);
	return IRQ_HANDLED;
}

static irqreturn_t pm8150b_wdog_bark_irq(int irq, void *data)
{
	struct pm8150b_charger *chip = data;
	int ret;

	ret = regmap_write(chip->regmap, PM8150B_WDOG_PET,
			   PM8150B_WDOG_PET_BIT);
	if (ret)
		dev_err(chip->dev, "Failed to pet charger watchdog: %d\n", ret);

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
				     "Failed to request %s IRQ\n", name);
	if (irq_out)
		*irq_out = irq;

	return 0;
}

static int pm8150b_probe(struct platform_device *pdev)
{
	struct power_supply_config config = {};
	struct pm8150b_charger *chip;
	unsigned int raw;
	int ret, wdog_irq;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	chip->dev = &pdev->dev;
	chip->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chip->regmap)
		return dev_err_probe(chip->dev, -ENODEV, "PMIC regmap not found\n");
	mutex_init(&chip->fg_lock);
	mutex_init(&chip->adc_lock);
	mutex_init(&chip->usb_lock);
	INIT_DELAYED_WORK(&chip->policy_work, pm8150b_policy_work);
	chip->usb_voltage_max_uv = PM8150B_USB_VOLTAGE_5V_UV;
	chip->gadget_current_limit_ua = -1;
	chip->gadget_current_limit_valid = false;
	chip->fg_smb_measure_enabled = -1;
	chip->temp_zone = PM8150B_TEMP_NORMAL;
	platform_set_drvdata(pdev, chip);

	chip->typec_fwnode = fwnode_find_reference(dev_fwnode(chip->dev),
						   "qcom,usb-c-port", 0);
	if (IS_ERR(chip->typec_fwnode))
		return dev_err_probe(chip->dev, PTR_ERR(chip->typec_fwnode),
				     "USB-C port not found\n");
	ret = devm_add_action_or_reset(chip->dev, pm8150b_put_typec_fwnode,
				       chip->typec_fwnode);
	if (ret)
		return ret;

	chip->usb_in_i = devm_iio_channel_get(chip->dev, "usbin_i");
	if (IS_ERR(chip->usb_in_i))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb_in_i),
				     "Failed to get usbin_i IIO channel\n");
	chip->mid_chg = devm_iio_channel_get(chip->dev, "mid_chg");
	if (IS_ERR(chip->mid_chg))
		return dev_err_probe(chip->dev, PTR_ERR(chip->mid_chg),
				     "Failed to get mid_chg IIO channel\n");
	chip->parallel = devm_power_supply_get_by_reference(chip->dev,
							    "qcom,parallel-charger");
	if (IS_ERR(chip->parallel))
		return dev_err_probe(chip->dev, PTR_ERR(chip->parallel),
				     "Failed to get SMB1355 power_supply\n");
	if (!chip->parallel)
		return dev_err_probe(chip->dev, -EPROBE_DEFER,
				     "SMB1355 power_supply not registered\n");
	ret = device_property_read_u32(chip->dev, "qcom,parallel-percent", &raw);
	if (ret || raw < 1 || raw > 99)
		return dev_err_probe(chip->dev, ret ?: -EINVAL,
				     "Invalid qcom,parallel-percent\n");
	chip->parallel_percent = raw;
	ret = device_property_read_u32(chip->dev, "qcom,auto-recharge-soc",
				       &raw);
	if (ret || raw > 100)
		return dev_err_probe(chip->dev, ret ?: -EINVAL,
				     "Invalid qcom,auto-recharge-soc\n");
	chip->auto_recharge_soc = raw;

	ret = pm8150b_fg_init(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to initialize FG DMA\n");

	config.drv_data = chip;
	config.fwnode = dev_fwnode(chip->dev);
	chip->usb = devm_power_supply_register(chip->dev, &pm8150b_usb_desc,
					       &config);
	if (IS_ERR(chip->usb))
		return dev_err_probe(chip->dev, PTR_ERR(chip->usb),
				     "Failed to register USB power_supply\n");

	ret = pm8150b_load_battery_info(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to read BL-T41 parameters\n");
	ret = pm8150b_clamp_profile(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to clamp BL-T41 charging limits\n");
	chip->policy_fcc_ua = chip->applied_fcc_ua;

	chip->battery = devm_power_supply_register(chip->dev,
						   &pm8150b_battery_desc,
						   &config);
	if (IS_ERR(chip->battery))
		return dev_err_probe(chip->dev, PTR_ERR(chip->battery),
				     "Failed to register battery power_supply\n");

	ret = pm8150b_request_irq(pdev, "chg-state-change",
				  pm8150b_charge_state_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "bat-temp",
				  pm8150b_usb_update_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "usbin-plugin",
				  pm8150b_usb_update_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "usbin-src-change",
				  pm8150b_usb_update_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "usbin-uv",
				  pm8150b_usbin_uv_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "soc-update", pm8150b_changed_irq, NULL);
	if (ret)
		return ret;
	ret = pm8150b_request_irq(pdev, "batt-temp-delta",
				  pm8150b_usb_update_irq, NULL);
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
				     "Failed to set watchdog IRQ as wake source\n");

	ret = pm8150b_enable_safety(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to enable charger safety configuration\n");
	ret = pm8150b_configure_recharge(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to configure automatic recharge\n");
	ret = pm8150b_init_protocols(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to initialize USB input\n");
	ret = pm8150b_read_aicl_thresholds(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret, "Failed to read AICL thresholds\n");

	chip->psy_nb.notifier_call = pm8150b_typec_notifier;
	ret = power_supply_reg_notifier(&chip->psy_nb);
	if (ret)
		return dev_err_probe(chip->dev, ret,
					     "Failed to register USB-C power_supply notifier\n");
	ret = devm_add_action_or_reset(chip->dev,
				       pm8150b_unregister_typec_notifier, chip);
	if (ret)
		return ret;
	ret = pm8150b_sync_typec_supply(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "USB-C power_supply not registered\n");

	mutex_lock(&chip->usb_lock);
	ret = pm8150b_update_usb_input(chip);
	mutex_unlock(&chip->usb_lock);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "Failed to configure USB input limits\n");

	mod_delayed_work(system_dfl_wq, &chip->policy_work,
			 msecs_to_jiffies(PM8150B_POLICY_NORMAL_MS));

	dev_info(chip->dev,
		 "PM8150B power_supply enabled, BL-T41 limits %d uA/%d uV, %d%% recharge\n",
		 chip->constant_charge_current_max_ua,
		 chip->constant_charge_voltage_max_uv,
		 chip->auto_recharge_soc);
	return 0;
}

static void pm8150b_remove(struct platform_device *pdev)
{
	struct pm8150b_charger *chip = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&chip->usb_lock);
	chip->shutting_down = true;
	mutex_unlock(&chip->usb_lock);
	cancel_delayed_work_sync(&chip->policy_work);
	ret = pm8150b_restore_input_baseline(chip);
	if (ret)
		dev_err(chip->dev, "Failed to restore input 5V on remove: %d\n", ret);
	mutex_lock(&chip->usb_lock);
	regmap_update_bits(chip->regmap, PM8150B_WDOG_CFG,
			   PM8150B_WDOG_TRIGGER_AFP |
			   PM8150B_WDOG_BARK_IRQ_ENABLE |
			   PM8150B_WDOG_ENABLE_ON_PLUGIN, 0);
	mutex_unlock(&chip->usb_lock);
}

static void pm8150b_shutdown(struct platform_device *pdev)
{
	struct pm8150b_charger *chip = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&chip->usb_lock);
	chip->shutting_down = true;
	mutex_unlock(&chip->usb_lock);
	cancel_delayed_work_sync(&chip->policy_work);
	ret = pm8150b_restore_input_baseline(chip);
	if (ret)
		dev_err(chip->dev, "Failed to restore input 5V on shutdown: %d\n", ret);
}

static const struct of_device_id pm8150b_match_table[] = {
	{ .compatible = "qcom,pm8150b-charger" },
	{}
};
MODULE_DEVICE_TABLE(of, pm8150b_match_table);

static struct platform_driver pm8150b_driver = {
	.probe = pm8150b_probe,
	.remove = pm8150b_remove,
	.shutdown = pm8150b_shutdown,
	.driver = {
		.name = "qcom-pm8150b-charger",
		.of_match_table = pm8150b_match_table,
	},
};
module_platform_driver(pm8150b_driver);

MODULE_DESCRIPTION("Qualcomm PM8150B charger and fuel gauge driver");
MODULE_LICENSE("GPL");
