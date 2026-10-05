// SPDX-License-Identifier: GPL-2.0-only
/* NXP TFA9872 smart amplifier. */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/qcom/q6afe-tfadsp.h>

#include "tfa9872.h"

#define TFA9872_REVISION		0x03
#define TFA9872_SYS_CONTROL0		0x00

#define TFA9872_PWDN			BIT(0)
#define TFA9872_AMPE			BIT(3)
#define TFA9872_DCA			BIT(4)

#define TFA9872_SYS_CONTROL1		0x01
#define TFA9872_AUDIO_CONTROL		0x51
#define TFA9872_MTP			0xf0
#define TFA9872_RE25			0xf5
#define TFA9872_I2CR			BIT(1)
#define TFA9872_MANSCONF		BIT(2)
#define TFA9872_MANAOOSC		BIT(4)
#define TFA9872_INTSMUTE		BIT(1)
#define TFA9872_MTPEX			BIT(1)

static const char * const tfa9872_profiles[] = {
	"stereo",
	"boombox",
};

static const char * const tfa9872_channels[] = {
	"Left",
	"Right",
};

struct tfa9872_priv {
	struct device *dev;
	struct regmap *regmap;
	struct q6afe_port *afe_port;
	const char *firmware_name;
	struct tfa9872_config *config;
	unsigned int rate;
	unsigned int profile_idx;
	unsigned int channel;
	bool enabled;
	bool playback_active;
	struct notifier_block tfadsp_nb;
	/* Serialize DAI transitions with asynchronous DSP close events. */
	struct mutex lock;
};

static const struct regmap_config tfa9872_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static void tfa9872_release_config(void *data)
{
	struct tfa9872_priv *tfa = data;

	tfa9872_config_free(tfa->config);
}

static int tfa9872_load_config(struct tfa9872_priv *tfa)
{
	const struct firmware *fw;
	const char *profile = tfa9872_profiles[tfa->profile_idx];
	unsigned int mtp, resistance;
	int ret;

	if (tfa->config)
		return 0;
	ret = regmap_read(tfa->regmap, TFA9872_MTP, &mtp);
	if (ret)
		return ret;
	if (!(mtp & TFA9872_MTPEX))
		return -ENODATA;
	ret = regmap_read(tfa->regmap, TFA9872_RE25, &resistance);
	if (ret)
		return ret;
	ret = request_firmware(&fw, tfa->firmware_name, tfa->dev);
	if (ret)
		return ret;
	ret = tfa9872_container_parse(fw, to_i2c_client(tfa->dev)->addr,
				      tfa->rate, profile, 0, resistance, &tfa->config);
	release_firmware(fw);
	if (!ret)
		dev_info(tfa->dev, "%s configuration ready, calibration %u mOhm\n",
			 profile, resistance);
	return ret;
}

static int tfa9872_init(struct tfa9872_priv *tfa)
{
	/* NXP tfa_init.c: TFA9872 N1B2 register map, version 21. */
	static const struct reg_sequence settings[] = {
		{ 0x02, 0x2dc8 }, { 0x20, 0x0890 }, { 0x22, 0x043c },
		{ 0x23, 0x0001 }, { 0x51, 0x0000 }, { 0x52, 0x5a1c },
		{ 0x61, 0x0198 }, { 0x63, 0x0a9a }, { 0x65, 0x0a82 },
		{ 0x6f, 0x01e3 }, { 0x70, 0x06fd }, { 0x71, 0x307e },
		{ 0x74, 0xcc84 }, { 0x75, 0x1132 }, { 0x82, 0x01ed },
		{ 0x83, 0x001a },
	};
	unsigned int key;
	int ret;

	ret = regmap_write(tfa->regmap, TFA9872_SYS_CONTROL0,
			   TFA9872_PWDN | TFA9872_I2CR);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0x0f, 0x5a6b);
	if (ret)
		return ret;
	ret = regmap_read(tfa->regmap, 0xfb, &key);
	if (ret)
		return ret;
	ret = regmap_write(tfa->regmap, 0xa0, key ^ 0x005a);
	if (ret)
		return ret;
	ret = regmap_multi_reg_write(tfa->regmap, settings, ARRAY_SIZE(settings));
	if (ret)
		return ret;
	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL1,
				 TFA9872_MANAOOSC, TFA9872_MANAOOSC);
	if (ret)
		return ret;
	ret = regmap_update_bits(tfa->regmap, 0xb0, BIT(3), BIT(3));
	if (ret)
		return ret;
	return regmap_update_bits(tfa->regmap, TFA9872_AUDIO_CONTROL,
				  TFA9872_INTSMUTE, TFA9872_INTSMUTE);
}

static int tfa9872_apply_config(struct tfa9872_priv *tfa)
{
	unsigned int i;
	int ret;

	for (i = 0; i < tfa->config->num_regs; i++) {
		const struct tfa9872_reg_setting *setting = &tfa->config->regs[i];
		u16 mask = setting->mask;

		/* Power and mute follow the stream state, after DSP configuration. */
		if (setting->reg == TFA9872_SYS_CONTROL0)
			mask &= ~(TFA9872_PWDN | TFA9872_AMPE | TFA9872_DCA);
		if (setting->reg == TFA9872_AUDIO_CONTROL)
			mask &= ~TFA9872_INTSMUTE;
		if (!mask)
			continue;
		ret = regmap_update_bits(tfa->regmap, setting->reg, mask, setting->value);
		if (ret)
			return ret;
	}
	return 0;
}

static int tfa9872_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_MASTER_MASK) != SND_SOC_DAIFMT_CBC_CFC ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	return 0;
}

static int tfa9872_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(dai->component);
	int ret;

	if (params_channels(params) != 2 || params_rate(params) != 48000)
		return -EINVAL;
	if (params_width(params) != 16 && params_width(params) != 24 &&
	    params_width(params) != 32)
		return -EINVAL;

	mutex_lock(&tfa->lock);
	tfa->rate = params_rate(params);
	ret = tfa9872_load_config(tfa);
	mutex_unlock(&tfa->lock);

	return ret;
}

static int tfa9872_configure(struct tfa9872_priv *tfa)
{
	int ret;

	if (!tfa->config) {
		ret = tfa9872_load_config(tfa);
		if (ret)
			return ret;
	}

	if (!tfa->afe_port)
		return -EAGAIN;

	ret = tfa9872_init(tfa);
	if (ret)
		return ret;
	ret = tfa9872_apply_config(tfa);
	if (ret)
		return ret;
	ret = regmap_update_bits(tfa->regmap, 0x26, 0x00ff,
				 (tfa->channel == 1) ? 0x0001 : 0x0010);
	if (ret)
		return ret;
	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL0,
				 TFA9872_PWDN, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL1,
				 TFA9872_MANSCONF, TFA9872_MANSCONF);
	if (ret)
		return ret;
	ret = q6afe_tfadsp_send_msg(tfa->afe_port, tfa->config->messages,
				    tfa->config->message_size);
	if (ret) {
		dev_err(tfa->dev, "TFADSP profile messages failed: %d\n", ret);
		return ret;
	}
	ret = q6afe_tfadsp_wait_configured(tfa->afe_port);
	if (ret) {
		dev_err(tfa->dev, "TFADSP configuration incomplete: %d\n", ret);
		return ret;
	}
	return 0;
}

static int tfa9872_stop(struct tfa9872_priv *tfa)
{
	int ret, mute_ret;

	mute_ret = regmap_update_bits(tfa->regmap, TFA9872_AUDIO_CONTROL,
				      TFA9872_INTSMUTE, TFA9872_INTSMUTE);
	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL0,
				 TFA9872_AMPE | TFA9872_DCA | TFA9872_PWDN,
				 TFA9872_PWDN);
	return mute_ret ?: ret;
}

static int tfa9872_unmute(struct tfa9872_priv *tfa)
{
	int ret;

	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL0,
				 TFA9872_PWDN | TFA9872_DCA | TFA9872_AMPE,
				 TFA9872_DCA | TFA9872_AMPE);
	if (!ret)
		ret = regmap_update_bits(tfa->regmap, TFA9872_AUDIO_CONTROL,
					 TFA9872_INTSMUTE, 0);
	return ret;
}

static int tfa9872_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(dai->component);
	int ret = 0;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	mutex_lock(&tfa->lock);
	tfa->playback_active = !mute;
	if (mute || !tfa->enabled) {
		ret = tfa9872_stop(tfa);
	} else {
		ret = tfa9872_unmute(tfa);
		if (ret) {
			int stop_ret = tfa9872_stop(tfa);

			if (stop_ret)
				dev_err(tfa->dev, "failed to power down amplifier: %d\n",
					stop_ret);
		}
	}
	mutex_unlock(&tfa->lock);
	return ret;
}

static int tfa9872_prepare(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(dai->component);
	int ret, stop_ret;

	guard(mutex)(&tfa->lock);
	if (!tfa->enabled)
		return 0;

	/* Prepare errors reach ALSA; ASoC does not propagate mute callback errors. */
	ret = tfa9872_configure(tfa);
	if (ret) {
		stop_ret = tfa9872_stop(tfa);
		if (stop_ret)
			dev_err(tfa->dev, "failed to power down amplifier: %d\n", stop_ret);
	}
	return ret;
}

static const struct snd_soc_dai_ops tfa9872_dai_ops = {
	.hw_params = tfa9872_hw_params,
	.prepare = tfa9872_prepare,
	.set_fmt = tfa9872_set_fmt,
	.mute_stream = tfa9872_mute_stream,
};

static int tfa9872_resistance_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *value)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);
	/* Mono GetRe25C returns RPC status followed by two Q16 resistances. */
	const u8 command[] = { 'm', 'm', 0, 6, 0, 4, 0x85, 0x81, 4, 0 };
	__le32 result[3];
	u32 status, resistance;
	int ret;

	guard(mutex)(&tfa->lock);
	ret = q6afe_tfadsp_read_msg(tfa->afe_port, command, sizeof(command),
				    result, sizeof(result));
	if (ret)
		return ret;
	status = le32_to_cpu(result[0]);
	if (status) {
		dev_err(tfa->dev, "TFADSP resistance query failed: status=%u\n", status);
		return -EIO;
	}
	resistance = le32_to_cpu(result[1]);
	if (!resistance)
		return -ENODATA;
	resistance = DIV_ROUND_CLOSEST((u64)resistance * 1000, 65536);
	if (resistance > U16_MAX)
		return -ERANGE;
	put_unaligned_le32(resistance, value->value.bytes.data);
	return 0;
}

static const struct soc_enum tfa9872_profile_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tfa9872_profiles), tfa9872_profiles);

static int tfa9872_profile_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);

	ucontrol->value.enumerated.item[0] = tfa->profile_idx;
	return 0;
}

static int tfa9872_profile_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);
	unsigned int item = ucontrol->value.enumerated.item[0];

	if (item >= ARRAY_SIZE(tfa9872_profiles))
		return -EINVAL;

	guard(mutex)(&tfa->lock);
	if (tfa->profile_idx == item)
		return 0;

	tfa->profile_idx = item;
	if (tfa->config) {
		tfa9872_config_free(tfa->config);
		tfa->config = NULL;
	}
	return 1;
}

static int tfa9872_enable_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = tfa->enabled;
	return 0;
}

static int tfa9872_enable_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);
	bool val = !!ucontrol->value.integer.value[0];
	int ret = 0;

	guard(mutex)(&tfa->lock);
	if (tfa->enabled == val)
		return 0;

	if (val && tfa->playback_active) {
		/* ACP can enable the amplifier after the shared MI2S backend starts. */
		ret = tfa9872_configure(tfa);
		if (!ret)
			ret = tfa9872_unmute(tfa);
		if (ret) {
			int stop_ret = tfa9872_stop(tfa);

			if (stop_ret)
				dev_err(tfa->dev, "failed to power down amplifier: %d\n",
					stop_ret);
		}
	} else if (!val) {
		ret = tfa9872_stop(tfa);
	}
	if (ret)
		return ret;
	tfa->enabled = val;
	return 1;
}

static const struct soc_enum tfa9872_channel_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tfa9872_channels), tfa9872_channels);

static int tfa9872_channel_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);

	ucontrol->value.enumerated.item[0] = tfa->channel;
	return 0;
}

static int tfa9872_channel_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(component);
	unsigned int item = ucontrol->value.enumerated.item[0];

	if (item >= ARRAY_SIZE(tfa9872_channels))
		return -EINVAL;

	guard(mutex)(&tfa->lock);
	if (tfa->channel == item)
		return 0;

	tfa->channel = item;
	regmap_update_bits(tfa->regmap, 0x26, 0x00ff,
			   (tfa->channel == 1) ? 0x0001 : 0x0010);
	return 1;
}

static const struct snd_kcontrol_new tfa9872_controls[] = {
	SOC_ENUM_EXT("TFA9872 Profile", tfa9872_profile_enum,
		     tfa9872_profile_get, tfa9872_profile_put),
	SOC_ENUM_EXT("TFA9872 Channel", tfa9872_channel_enum,
		     tfa9872_channel_get, tfa9872_channel_put),
	SOC_SINGLE_BOOL_EXT("TFA9872 Switch", 0,
			    tfa9872_enable_get, tfa9872_enable_put),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "TFA9872 DSP Resistance",
		.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		/* Byte diagnostics are excluded from ALSA simple-mixer volume probing. */
		.info = snd_soc_bytes_info_ext,
		.get = tfa9872_resistance_get,
		.private_value = (unsigned long)&(struct soc_bytes_ext) {
			.max = sizeof(__le32),
		},
	},
};

static const struct snd_soc_dapm_widget tfa9872_widgets[] = {
	SND_SOC_DAPM_AIF_IN("TFA9872 AIF", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("TFA9872 Speaker"),
};

static const struct snd_soc_dapm_route tfa9872_routes[] = {
	{ "TFA9872 Speaker", NULL, "TFA9872 AIF" },
};

static struct snd_soc_dai_driver tfa9872_dai = {
	.name = "tfa9872-hifi",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &tfa9872_dai_ops,
};

static const struct snd_soc_component_driver tfa9872_component = {
	.controls = tfa9872_controls,
	.num_controls = ARRAY_SIZE(tfa9872_controls),
	.dapm_widgets = tfa9872_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tfa9872_widgets),
	.dapm_routes = tfa9872_routes,
	.num_dapm_routes = ARRAY_SIZE(tfa9872_routes),
	.use_pmdown_time = 1,
};

static int tfa9872_tfadsp_event(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct tfa9872_priv *tfa = container_of(nb, struct tfa9872_priv,
						tfadsp_nb);

	mutex_lock(&tfa->lock);
	switch (event) {
	case Q6AFE_TFADSP_EVENT_CLOSE:
		/* An XRUN may create a new RX instance before this work acquires the lock. */
		if (q6afe_tfadsp_is_ready(tfa->afe_port))
			break;
		fallthrough;
	case Q6AFE_TFADSP_EVENT_RX_DISABLED:
	case Q6AFE_TFADSP_EVENT_TX_DISABLED:
		if (tfa9872_stop(tfa))
			dev_err(tfa->dev, "failed to power down after DSP close\n");
		break;
	default:
		break;
	}
	mutex_unlock(&tfa->lock);

	return NOTIFY_OK;
}

static void tfa9872_release_afe(void *data);

static int tfa9872_probe(struct i2c_client *client)
{
	struct tfa9872_priv *tfa;
	unsigned int revision;
	int ret;

	tfa = devm_kzalloc(&client->dev, sizeof(*tfa), GFP_KERNEL);
	if (!tfa)
		return -ENOMEM;
	tfa->dev = &client->dev;
	ret = device_property_read_string(&client->dev, "firmware-name",
					  &tfa->firmware_name);
	if (ret)
		return ret;
	mutex_init(&tfa->lock);
	tfa->enabled = true;
	tfa->profile_idx = 0;
	tfa->channel = 1;
	tfa->regmap = devm_regmap_init_i2c(client, &tfa9872_regmap_config);
	if (IS_ERR(tfa->regmap))
		return PTR_ERR(tfa->regmap);

	ret = regmap_read(tfa->regmap, TFA9872_REVISION, &revision);
	if (ret)
		return ret;
	if (revision != 0x1b72 && revision != 0x2b72 && revision != 0x3b72)
		return -ENODEV;

	tfa->afe_port = q6afe_tfadsp_get_port(&client->dev, SECONDARY_MI2S_RX);
	if (IS_ERR(tfa->afe_port))
		return PTR_ERR(tfa->afe_port);
	tfa->tfadsp_nb.notifier_call = tfa9872_tfadsp_event;
	ret = q6afe_tfadsp_register_notifier(tfa->afe_port, &tfa->tfadsp_nb);
	if (ret) {
		q6afe_tfadsp_put_port(tfa->afe_port);
		return ret;
	}

	i2c_set_clientdata(client, tfa);
	ret = devm_add_action_or_reset(&client->dev, tfa9872_release_afe, tfa);
	if (ret)
		return ret;
	ret = tfa9872_stop(tfa);
	if (ret)
		return ret;
	dev_info(&client->dev, "revision %#x\n", revision);
	ret = devm_add_action_or_reset(&client->dev, tfa9872_release_config, tfa);
	if (ret)
		return ret;
	return devm_snd_soc_register_component(&client->dev, &tfa9872_component,
					       &tfa9872_dai, 1);
}

static void tfa9872_shutdown(struct i2c_client *client)
{
	struct tfa9872_priv *tfa = i2c_get_clientdata(client);
	int ret;

	guard(mutex)(&tfa->lock);
	ret = tfa9872_stop(tfa);
	if (ret)
		dev_err(tfa->dev, "failed to power down amplifier: %d\n", ret);
}

static void tfa9872_release_afe(void *data)
{
	struct tfa9872_priv *tfa = data;

	q6afe_tfadsp_unregister_notifier(tfa->afe_port, &tfa->tfadsp_nb);
	q6afe_tfadsp_put_port(tfa->afe_port);
}

static const struct of_device_id tfa9872_of_match[] = {
	{ .compatible = "nxp,tfa9872" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa9872_of_match);

static const struct i2c_device_id tfa9872_i2c_id[] = {
	{ "tfa9872", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfa9872_i2c_id);

static struct i2c_driver tfa9872_i2c_driver = {
	.driver = {
		.name = "tfa9872",
		.of_match_table = tfa9872_of_match,
	},
	.probe = tfa9872_probe,
	.remove = tfa9872_shutdown,
	.shutdown = tfa9872_shutdown,
	.id_table = tfa9872_i2c_id,
};
module_i2c_driver(tfa9872_i2c_driver);

MODULE_DESCRIPTION("NXP TFA9872 smart amplifier");
MODULE_LICENSE("GPL");
