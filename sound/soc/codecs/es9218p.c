// SPDX-License-Identifier: GPL-2.0-only
/* ESS ES9218P stereo DAC and headphone amplifier. */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/property.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/soc-jack.h>

#include "es9218p.h"
#include "wcd934x.h"

enum es9218p_state {
	ES9218P_CLOSE = ESS_PS_CLOSE,
	ES9218P_OPEN = ESS_PS_OPEN,
	ES9218P_BYPASS = ESS_PS_BYPASS,
	ES9218P_HIFI = ESS_PS_HIFI,
	ES9218P_IDLE = ESS_PS_IDLE,
	ES9218P_ACTIVE = ESS_PS_ACTIVE,
};

enum es9218p_amp_mode {
	ES9218P_HIFI1,
	ES9218P_HIFI2,
};

struct es9218p_priv {
	struct i2c_client *i2c;
	struct gpio_desc *power;
	struct gpio_desc *mode;
	struct gpio_desc *reset;
	struct mutex lock;
	struct notifier_block jack_notifier;
	struct snd_soc_jack *jack;
	struct snd_soc_component *wcd_component;
	struct device *dev;
	enum es9218p_state state;
	enum es9218p_amp_mode amp_mode;
	unsigned int sample_rate;
	unsigned int sample_format;
	unsigned int master_volume;
	unsigned int avc_volume;
	unsigned int left_volume;
	unsigned int right_volume;
	bool aux_load;
	bool powered;
	bool use_internal_ldo;
	bool jack_present;
	bool playback_active;
	bool jack_notifier_registered;
};

static int es9218p_enter_bypass(struct es9218p_priv *es);
static void es9218p_power_off(struct es9218p_priv *es);
static int es9218p_amp_start(struct es9218p_priv *es);
static int es9218p_amp_stop(struct es9218p_priv *es);

static const u32 es9218p_master_trim[] = {
	0x7fffffff, 0x78d6fc9d, 0x721482bf, 0x6bb2d603, 0x65ac8c2e,
	0x5ffc888f, 0x5a9df7aa, 0x558c4b21, 0x50c335d3, 0x4c3ea838,
	0x47faccef, 0x43f4057e, 0x4026e73c, 0x3c90386f, 0x392ced8d,
	0x35fa26a9, 0x32f52cfe, 0x301b70a7, 0x2d6a866f, 0x2ae025c2,
	0x287a26c4, 0x26368073, 0x241346f5, 0x220ea9f3, 0x2026f30f,
	0x1e5a8471, 0x1ca7d767, 0x1b0d7b1b, 0x198a1357, 0x181c5761,
	0x16c310e3, 0x157d1ae1, 0x144960c5, 0x1326dd70, 0x12149a5f,
	0x1111aeda, 0x101d3f2d, 0x0f367bed, 0x0e5ca14c, 0x0d8ef66d,
	0x0ccccccc, 0x0c157fa9, 0x0b687379, 0x0ac51566, 0x0a2adad1,
	0x099940db, 0x090fcbf7, 0x088e0783, 0x08138561, 0x079fdd9f,
	0x0732ae17, 0x06cb9a26, 0x066a4a52, 0x060e6c0b, 0x05b7b15a,
	0x0565d0aa, 0x0518847f, 0x04cf8b43, 0x048aa70b, 0x04499d60,
	0x040c3713, 0x03d2400b, 0x039b8718, 0x0367ddcb, 0x0337184e,
	0x03090d3e, 0x02dd958a, 0x02b48c4f, 0x028dcebb, 0x02693bf0,
	0x0246b4e3, 0x02261c49, 0x0207567a, 0x01ea4958, 0x01cedc3c,
	0x01b4f7e2, 0x019c8651, 0x018572ca, 0x016fa9ba, 0x015b18a4,
	0x0147ae14,
};

static const u8 es9218p_avc_volume[] = {
	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51,
	0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
};

static const u8 es9218p_normal_thd_left[] = { 0x78, 0x00, 0x9a, 0xfc };
static const u8 es9218p_normal_thd_right[] = { 0x1e, 0x00, 0x12, 0xfd };
static const u8 es9218p_advanced_thd_left[] = { 0x58, 0x02, 0x3c, 0x00 };
static const u8 es9218p_advanced_thd_right[] = { 0x21, 0x02, 0x64, 0x00 };
static const u8 es9218p_aux_thd_left[] = { 0x4a, 0x01, 0xe4, 0xfd };
static const u8 es9218p_aux_thd_right[] = { 0x04, 0x01, 0x0c, 0xfe };

static int es9218p_jack_status_changed(struct notifier_block *notifier,
					       unsigned long status, void *jack)
{
	struct es9218p_priv *es = container_of(notifier, struct es9218p_priv,
						       jack_notifier);
	enum es9218p_amp_mode mode = ES9218P_HIFI1;
	u32 left, right;
	bool aux_load = false;

	if (!(status & (SND_JACK_MECHANICAL | SND_JACK_HEADPHONE |
			SND_JACK_LINEOUT))) {
		mutex_lock(&es->lock);
		es->jack_present = false;
		if (es->state == ES9218P_HIFI || es->state == ES9218P_IDLE ||
		    es->state == ES9218P_ACTIVE)
			es9218p_amp_stop(es);
		es9218p_power_off(es);
		if (es->wcd_component)
			es->amp_mode = ES9218P_HIFI1;
		mutex_unlock(&es->lock);
		return NOTIFY_OK;
	}

	if (status & (SND_JACK_HEADPHONE | SND_JACK_LINEOUT)) {
		if (es->wcd_component &&
		    !wcd934x_get_impedance(es->wcd_component, &left, &right)) {
			aux_load = max(left, right) > 600;
			if (max(left, right) > 50 && !aux_load)
				mode = ES9218P_HIFI2;
		}
	}

	mutex_lock(&es->lock);
	es->amp_mode = mode;
	es->aux_load = aux_load;
	es->jack_present = true;
	if (es->state == ES9218P_CLOSE || es->state == ES9218P_OPEN)
		es9218p_enter_bypass(es);
	if ((status & (SND_JACK_HEADPHONE | SND_JACK_LINEOUT)) &&
	    es->playback_active &&
	    es->state == ES9218P_BYPASS) {
		int ret = es9218p_amp_start(es);

		if (ret)
			dev_err(es->dev, "Failed to start headphone amplifier: %d\n", ret);
	}
	mutex_unlock(&es->lock);

	return NOTIFY_OK;
}

static int es9218p_set_jack(struct snd_soc_component *component,
			    struct snd_soc_jack *jack, void *data)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(component);
	struct snd_soc_jack *old_jack;

	mutex_lock(&es->lock);
	old_jack = es->jack;
	mutex_unlock(&es->lock);

	if (es->jack_notifier_registered && old_jack) {
		snd_soc_jack_notifier_unregister(old_jack, &es->jack_notifier);
		es->jack_notifier_registered = false;
	}

	mutex_lock(&es->lock);
	es->jack = jack;
	es->wcd_component = jack ? data : NULL;
	mutex_unlock(&es->lock);

	if (jack) {
		snd_soc_jack_notifier_register(jack, &es->jack_notifier);
		es->jack_notifier_registered = true;
		es9218p_jack_status_changed(&es->jack_notifier, jack->status, jack);
	} else {
		mutex_lock(&es->lock);
		es->jack_present = false;
		es->playback_active = false;
		es->amp_mode = ES9218P_HIFI1;
		if (es->state == ES9218P_HIFI || es->state == ES9218P_IDLE ||
		    es->state == ES9218P_ACTIVE)
			es9218p_amp_stop(es);
		es9218p_power_off(es);
		mutex_unlock(&es->lock);
		return 0;
	}

	return 0;
}

static int es9218p_write(struct es9218p_priv *es, unsigned int reg, u8 value)
{
	int ret;

	ret = i2c_smbus_write_byte_data(es->i2c, reg, value);
	if (ret)
		dev_err(es->dev, "Failed to write register %#x: %d\n", reg, ret);

	return ret;
}

static int es9218p_write_sequence(struct es9218p_priv *es,
				  const struct reg_sequence *seq,
				  size_t count)
{
	int ret;

	while (count--) {
		ret = es9218p_write(es, seq->reg, seq->def);
		if (ret)
			return ret;
		if (seq->delay_us)
			usleep_range(seq->delay_us, seq->delay_us + 100);
		seq++;
	}
	return 0;
}

static int es9218p_write_master_trim(struct es9218p_priv *es)
{
	u32 trim = es9218p_master_trim[es->master_volume];
	int i, ret;

	for (i = 0; i < 4; i++) {
		ret = es9218p_write(es, ES9218P_REG_MASTER_TRIM + i,
				    (trim >> (i * 8)) & 0xff);
		if (ret)
			return ret;
	}
	return 0;
}

static int es9218p_write_digital_volumes(struct es9218p_priv *es)
{
	int ret;

	ret = es9218p_write(es, ES9218P_REG_LEFT_VOLUME, es->left_volume);
	if (ret)
		return ret;
	ret = es9218p_write(es, ES9218P_REG_RIGHT_VOLUME, es->right_volume);
	if (ret)
		return ret;
	return es9218p_write_master_trim(es);
}

static int es9218p_enter_bypass(struct es9218p_priv *es)
{
	gpiod_set_value_cansleep(es->reset, 0);
	gpiod_set_value_cansleep(es->mode, 0);
	if (!es->powered) {
		gpiod_set_value_cansleep(es->power, 1);
		es->powered = true;
		usleep_range(1000, 1100);
	}
	/* Reset low and mode high select low power bypass. */
	gpiod_set_value_cansleep(es->mode, 1);
	es->state = ES9218P_BYPASS;
	return 0;
}

static void es9218p_power_off(struct es9218p_priv *es)
{
	gpiod_set_value_cansleep(es->mode, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(es->reset, 0);
	gpiod_set_value_cansleep(es->power, 0);
	es->powered = false;
	es->state = ES9218P_CLOSE;
}

static int es9218p_amp_start(struct es9218p_priv *es)
{
	const u8 *left_thd = es->aux_load ? es9218p_aux_thd_left :
		es->amp_mode == ES9218P_HIFI2 ?
		es9218p_advanced_thd_left : es9218p_normal_thd_left;
	const u8 *right_thd = es->aux_load ? es9218p_aux_thd_right :
		es->amp_mode == ES9218P_HIFI2 ?
		es9218p_advanced_thd_right : es9218p_normal_thd_right;
	unsigned int avc_volume;
	int i, ret;

	if (es->state != ES9218P_BYPASS)
		return -EINVAL;
	gpiod_set_value_cansleep(es->reset, 1);
	usleep_range(2000, 2100);

	{
		static const struct reg_sequence common_init[] = {
			{ ES9218P_REG_OVERCURRENT, 0x90 },
			{ ES9218P_REG_DPLL, 0x8a },
			{ ES9218P_REG_THD, 0x00 },
			{ ES9218P_REG_SOFT_START, 0x07 },
			{ ES9218P_REG_GPIO_INPUT, 0x0f },
			{ ES9218P_REG_AUTO_CLK_GEAR, 0x37 },
			{ ES9218P_REG_CP_CLOCK_2, 0x30 },
		};
		static const struct reg_sequence pcm_init[] = {
			{ ES9218P_REG_SYSTEM, 0x00 },
			{ ES9218P_REG_AUTOMUTE_CONFIG, 0x34 },
			{ ES9218P_REG_AUTOMUTE_TIME, 0x00 },
			{ ES9218P_REG_RATE, 0x43 },
			{ ES9218P_REG_MASTER_MODE, 0x02 },
			{ ES9218P_REG_GENERAL, 0x06 },
		};

		ret = es9218p_write(es, ES9218P_REG_CP_SOFT_START, 0xc4);
		if (ret)
			goto err_bypass;
		ret = es9218p_write_sequence(es, common_init,
					     ARRAY_SIZE(common_init));
		if (ret)
			goto err_bypass;
		ret = es9218p_write_sequence(es, pcm_init,
					     ARRAY_SIZE(pcm_init));
		if (ret)
			goto err_bypass;
	}

	ret = es9218p_write(es, ES9218P_REG_INPUT, es->sample_format);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_RATE,
			    es->sample_rate == 48000 ? 0x44 : 0x43);
	if (ret)
		goto err_bypass;
	for (i = 0; i < 4; i++) {
		ret = es9218p_write(es, ES9218P_REG_THD_LEFT_0 + i, left_thd[i]);
		if (ret)
			goto err_bypass;
		ret = es9218p_write(es, ES9218P_REG_THD_RIGHT_0 + i,
				    right_thd[i]);
		if (ret)
			goto err_bypass;
	}

	/* Program the ESS factory override sequence for the amplifier and pumps. */
	ret = es9218p_write(es, ES9218P_REG_OUTPUT_CONFIG, 0x07);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_DIGITAL_OVERRIDE,
			    es->use_internal_ldo ? 0x80 : 0x84);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_AMP_CONFIG,
			    es->amp_mode == ES9218P_HIFI2 ? 0x03 : 0x02);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_ANALOG_VOLUME, 0x18);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_AUX_OVERRIDE, 0x08);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_OUTPUT_OVERRIDE, 0x08);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_AUX_OVERRIDE, 0x68);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_OUTPUT_OVERRIDE, 0x1c);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_AUX_OVERRIDE, 0x78);
	if (ret)
		goto err_bypass;
	usleep_range(5000, 5100);
	ret = es9218p_write(es, ES9218P_REG_OUTPUT_OVERRIDE, 0x7c);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_DIGITAL_OVERRIDE,
			    es->use_internal_ldo ? 0x81 : 0x85);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_OUTPUT_CONFIG, 0x47);
	if (ret)
		goto err_bypass;

	if (es->amp_mode == ES9218P_HIFI2) {
		static const struct reg_sequence hifi2_transition[] = {
			{ ES9218P_REG_DIGITAL_OVERRIDE, 0x80 },
			{ ES9218P_REG_AUX_OVERRIDE, 0x7d },
			{ ES9218P_REG_DIGITAL_OVERRIDE, 0x81 },
			{ ES9218P_REG_AUX_OVERRIDE, 0x7f },
		};

		for (i = 0; i < ARRAY_SIZE(hifi2_transition); i++) {
			unsigned int value = hifi2_transition[i].def;

			if (hifi2_transition[i].reg == ES9218P_REG_DIGITAL_OVERRIDE &&
			    !es->use_internal_ldo)
				value |= 0x04;
			ret = es9218p_write(es, hifi2_transition[i].reg, value);
			if (ret)
				goto err_bypass;
		}
	}
	avc_volume = es9218p_avc_volume[es->avc_volume];
	ret = es9218p_write(es, ES9218P_REG_ANALOG_VOLUME, avc_volume);
	if (ret)
		goto err_bypass;
	ret = es9218p_write(es, ES9218P_REG_DIGITAL_OVERRIDE,
			    es->use_internal_ldo ? 0x03 : 0x07);
	if (ret)
		goto err_bypass;
	ret = es9218p_write_digital_volumes(es);
	if (ret)
		goto err_bypass;
	es->state = ES9218P_HIFI;
	return 0;

err_bypass:
	gpiod_set_value_cansleep(es->reset, 0);
	es->state = ES9218P_BYPASS;
	return ret;
}

static int es9218p_amp_stop(struct es9218p_priv *es)
{
	int ret;

	if (es->state != ES9218P_HIFI && es->state != ES9218P_IDLE &&
	    es->state != ES9218P_ACTIVE)
		return 0;
	ret = es9218p_write(es, ES9218P_REG_AMP_CONFIG, 0x00);
	if (!ret)
		msleep(100);
	gpiod_set_value_cansleep(es->reset, 0);
	es->state = ES9218P_BYPASS;
	return ret;
}

static int es9218p_state_get(struct snd_kcontrol *control,
			     struct snd_ctl_elem_value *value)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(
		snd_kcontrol_chip(control));

	mutex_lock(&es->lock);
	value->value.enumerated.item[0] = es->state;
	mutex_unlock(&es->lock);
	return 0;
}

static const char * const es9218p_state_text[] = {
	"Close", "Open", "Bypass", "Hifi", "Idle", "Active",
};

static SOC_ENUM_SINGLE_EXT_DECL(es9218p_state_enum, es9218p_state_text);

static int es9218p_amp_mode_get(struct snd_kcontrol *control,
				struct snd_ctl_elem_value *value)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(
		snd_kcontrol_chip(control));

	mutex_lock(&es->lock);
	value->value.enumerated.item[0] = es->amp_mode;
	mutex_unlock(&es->lock);
	return 0;
}

static int es9218p_amp_mode_put(struct snd_kcontrol *control,
				struct snd_ctl_elem_value *value)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(
		snd_kcontrol_chip(control));
	unsigned int mode = value->value.enumerated.item[0];
	int ret = 0;

	if (mode > ES9218P_HIFI2)
		return -EINVAL;
	mutex_lock(&es->lock);
	if (es->state == ES9218P_HIFI || es->state == ES9218P_IDLE ||
	    es->state == ES9218P_ACTIVE)
		ret = -EBUSY;
	else if (es->amp_mode != mode) {
		es->amp_mode = mode;
		ret = 1;
	}
	mutex_unlock(&es->lock);
	return ret;
}

static const char * const es9218p_amp_mode_text[] = { "HiFi1", "HiFi2" };
static SOC_ENUM_SINGLE_EXT_DECL(es9218p_amp_mode_enum,
				es9218p_amp_mode_text);

static int es9218p_volume_get(struct snd_kcontrol *control,
			      struct snd_ctl_elem_value *value)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(
		snd_kcontrol_chip(control));
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)control->private_value;

	mutex_lock(&es->lock);
	switch (mc->shift) {
	case 0:
		value->value.integer.value[0] = es->left_volume;
		break;
	case 1:
		value->value.integer.value[0] = es->right_volume;
		break;
	case 2:
		value->value.integer.value[0] = es->master_volume;
		break;
	default:
		value->value.integer.value[0] = es->avc_volume;
		break;
	}
	mutex_unlock(&es->lock);
	return 0;
}

static int es9218p_volume_put(struct snd_kcontrol *control,
			      struct snd_ctl_elem_value *value)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(
		snd_kcontrol_chip(control));
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)control->private_value;
	unsigned int id = mc->shift;
	unsigned int volume = value->value.integer.value[0];
	unsigned int max = id == 2 ? ARRAY_SIZE(es9218p_master_trim) - 1 :
			   id == 3 ? ARRAY_SIZE(es9218p_avc_volume) - 1 : 255;
	unsigned int reg, old_volume;
	unsigned int *cached_volume;
	int ret = 0;

	if (volume > max)
		return -EINVAL;
	switch (id) {
	case 0:
		reg = ES9218P_REG_LEFT_VOLUME;
		break;
	case 1:
		reg = ES9218P_REG_RIGHT_VOLUME;
		break;
	case 2:
		reg = ES9218P_REG_MASTER_TRIM;
		break;
	case 3:
		reg = ES9218P_REG_ANALOG_VOLUME;
		break;
	default:
		return -EINVAL;
	}
	mutex_lock(&es->lock);
	switch (id) {
	case 0:
		cached_volume = &es->left_volume;
		break;
	case 1:
		cached_volume = &es->right_volume;
		break;
	case 2:
		cached_volume = &es->master_volume;
		break;
	default:
		cached_volume = &es->avc_volume;
		break;
	}
	old_volume = *cached_volume;
	if (old_volume == volume)
		goto out;
	*cached_volume = volume;
	if (es->state == ES9218P_HIFI || es->state == ES9218P_IDLE ||
	    es->state == ES9218P_ACTIVE) {
		if (id == 2)
			ret = es9218p_write_master_trim(es);
		else
			ret = es9218p_write(es, reg, id == 3 ?
					    es9218p_avc_volume[volume] : volume);
	}
	if (ret)
		*cached_volume = old_volume;
	else
		ret = 1;
out:
	mutex_unlock(&es->lock);
	return ret;
}

static const struct snd_kcontrol_new es9218p_controls[] = {
	SOC_ENUM_EXT_ACC("Es9018 State", es9218p_state_enum,
			 es9218p_state_get, NULL, SNDRV_CTL_ELEM_ACCESS_READ),
	SOC_ENUM_EXT("Es9218 Amplifier Mode", es9218p_amp_mode_enum,
		     es9218p_amp_mode_get, es9218p_amp_mode_put),
	SOC_SINGLE_EXT("Es9018 Left Volume", SND_SOC_NOPM, 0, 255, 0,
		       es9218p_volume_get, es9218p_volume_put),
	SOC_SINGLE_EXT("Es9018 Right Volume", SND_SOC_NOPM, 1, 255, 0,
		       es9218p_volume_get, es9218p_volume_put),
	SOC_SINGLE_EXT("Es9018 Master Volume", SND_SOC_NOPM, 2, 80, 0,
		       es9218p_volume_get, es9218p_volume_put),
	SOC_SINGLE_EXT("Es9018 AVC Volume", SND_SOC_NOPM, 3, 24, 0,
		       es9218p_volume_get, es9218p_volume_put),
};

static int es9218p_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct es9218p_priv *es = snd_soc_component_get_drvdata(dai->component);
	unsigned int rate = params_rate(params);
	unsigned int format = params_format(params);
	unsigned int input;
	int ret = 0;

	switch (format) {
	case SNDRV_PCM_FORMAT_S16_LE:
	case SNDRV_PCM_FORMAT_S16_BE:
		input = ES9218P_INPUT_16BIT;
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
	case SNDRV_PCM_FORMAT_S20_3BE:
	case SNDRV_PCM_FORMAT_S24_LE:
	case SNDRV_PCM_FORMAT_S24_BE:
	case SNDRV_PCM_FORMAT_S32_LE:
	case SNDRV_PCM_FORMAT_S32_BE:
		input = ES9218P_INPUT_32BIT;
		break;
	default:
		return -EINVAL;
	}
	mutex_lock(&es->lock);
	es->sample_rate = rate;
	es->sample_format = input;
	if (es->state == ES9218P_HIFI || es->state == ES9218P_IDLE ||
	    es->state == ES9218P_ACTIVE) {
		ret = es9218p_write(es, ES9218P_REG_INPUT, input);
		if (!ret)
			ret = es9218p_write(es, ES9218P_REG_RATE,
					    rate == 48000 ? ES9218P_RATE_48000 :
					    ES9218P_RATE_DEFAULT);
	}
	mutex_unlock(&es->lock);
	return ret;
}

static int es9218p_aif_event(struct snd_soc_dapm_widget *widget,
			   struct snd_kcontrol *control, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(widget->dapm);
	struct es9218p_priv *es = snd_soc_component_get_drvdata(component);
	int ret = 0;

	mutex_lock(&es->lock);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (es->jack_present && es->state == ES9218P_BYPASS)
			ret = es9218p_amp_start(es);
		es->playback_active = !ret;
		break;
	case SND_SOC_DAPM_POST_PMD:
		es->playback_active = false;
		ret = es9218p_amp_stop(es);
		break;
	}
	mutex_unlock(&es->lock);

	return ret;
}

static int es9218p_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBC_CFC ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	return 0;
}

static const struct snd_soc_dai_ops es9218p_dai_ops = {
	.hw_params = es9218p_hw_params,
	.set_fmt = es9218p_set_fmt,
};

static const struct snd_soc_dapm_widget es9218p_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("ES9218P AIF", "Playback", 0,
			      SND_SOC_NOPM, 0, 0, es9218p_aif_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("ES9218P OUT"),
};

static const struct snd_soc_dapm_route es9218p_dapm_routes[] = {
	{ "ES9218P OUT", NULL, "ES9218P AIF" },
};

static struct snd_soc_dai_driver es9218p_dai = {
	.name = "es9218-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
			 SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 |
			 SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |
			 SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_176400 |
			 SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE |
			   SNDRV_PCM_FMTBIT_S20_3LE | SNDRV_PCM_FMTBIT_S20_3BE |
			   SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE |
			   SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE,
	},
	.ops = &es9218p_dai_ops,
};

static const struct snd_soc_component_driver es9218p_component = {
	.controls = es9218p_controls,
	.num_controls = ARRAY_SIZE(es9218p_controls),
	.dapm_widgets = es9218p_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(es9218p_dapm_widgets),
	.dapm_routes = es9218p_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(es9218p_dapm_routes),
	.name = "es9218p",
	.set_jack = es9218p_set_jack,
};

static int es9218p_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct es9218p_priv *es;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;
	es = devm_kzalloc(dev, sizeof(*es), GFP_KERNEL);
	if (!es)
		return -ENOMEM;
	es->i2c = client;
	es->dev = dev;
	es->state = ES9218P_CLOSE;
	es->amp_mode = ES9218P_HIFI1;
	es->sample_rate = 48000;
	es->sample_format = ES9218P_INPUT_16BIT;
	es->master_volume = 4;
	es->avc_volume = 0;
	es->use_internal_ldo = device_property_read_bool(dev,
							 "ess,use-internal-ldo");
	mutex_init(&es->lock);
	es->power = devm_gpiod_get(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(es->power))
		return dev_err_probe(dev, PTR_ERR(es->power),
				     "failed to get power GPIO\n");
	es->mode = devm_gpiod_get(dev, "mode", GPIOD_OUT_LOW);
	if (IS_ERR(es->mode))
		return dev_err_probe(dev, PTR_ERR(es->mode),
				     "failed to get mode GPIO\n");
	es->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(es->reset))
		return dev_err_probe(dev, PTR_ERR(es->reset),
				     "failed to get reset GPIO\n");
	es->jack_notifier.notifier_call = es9218p_jack_status_changed;
	i2c_set_clientdata(client, es);
	ret = devm_snd_soc_register_component(dev, &es9218p_component,
					     &es9218p_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register component\n");
	return 0;
}

static void es9218p_remove(struct i2c_client *client)
{
	struct es9218p_priv *es = i2c_get_clientdata(client);

	if (es->jack_notifier_registered && es->jack) {
		snd_soc_jack_notifier_unregister(es->jack, &es->jack_notifier);
		es->jack_notifier_registered = false;
	}
	mutex_lock(&es->lock);
	es9218p_amp_stop(es);
	es9218p_power_off(es);
	mutex_unlock(&es->lock);
}

static const struct of_device_id es9218p_of_match[] = {
	{ .compatible = "ess,es9218p" },
	{}
};
MODULE_DEVICE_TABLE(of, es9218p_of_match);

static const struct i2c_device_id es9218p_i2c_id[] = {
	{ "es9218p", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, es9218p_i2c_id);

static struct i2c_driver es9218p_i2c_driver = {
	.driver = {
		.name = "es9218p",
		.of_match_table = es9218p_of_match,
	},
	.probe = es9218p_probe,
	.remove = es9218p_remove,
	.id_table = es9218p_i2c_id,
};
module_i2c_driver(es9218p_i2c_driver);

MODULE_DESCRIPTION("ESS ES9218P stereo DAC and headphone amplifier");
MODULE_LICENSE("GPL");
