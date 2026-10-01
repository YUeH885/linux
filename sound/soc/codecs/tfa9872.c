// SPDX-License-Identifier: GPL-2.0-only
/* NXP TFA9872 smart amplifier. */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/crc32.h>
#include <linux/unaligned.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/qcom/q6afe-tfadsp.h>

#define TFA9872_REVISION		0x03
#define TFA9872_SYS_CONTROL0		0x00
#define TFA9872_SAMPLE_RATE		0x02

#define TFA9872_PWDN			BIT(0)
#define TFA9872_AMPE			BIT(3)
#define TFA9872_DCA			BIT(4)
#define TFA9872_TDMFS_MASK		GENMASK(3, 0)

#define TFA9872_RATE_8000		0
#define TFA9872_RATE_16000		3
#define TFA9872_RATE_32000		6
#define TFA9872_RATE_44100		7
#define TFA9872_RATE_48000		8
#define TFA9872_RATE_96000		11
#define TFA9872_RATE_192000		13
#define TFA9872_CONTAINER_HEADER	46
#define TFA9872_DESC_PROFILE		1
#define TFA9872_DESC_FILE		4
#define TFA9872_FILE_MESSAGE		0x474d

struct tfa9872_priv {
	struct device *dev;
	struct regmap *regmap;
	struct q6afe_port *afe_port;
	const char *firmware_name;
	const struct firmware *firmware;
	unsigned int revision;
	unsigned int rate;
	u32 device_offset;
	u32 profile_offset;
	bool firmware_ready;
	bool stream_active;
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

static int tfa9872_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:
		return TFA9872_RATE_8000;
	case 16000:
		return TFA9872_RATE_16000;
	case 32000:
		return TFA9872_RATE_32000;
	case 44100:
		return TFA9872_RATE_44100;
	case 48000:
		return TFA9872_RATE_48000;
	case 96000:
		return TFA9872_RATE_96000;
	case 192000:
		return TFA9872_RATE_192000;
	default:
		return -EINVAL;
	}
}

static int tfa9872_convert_message(const u8 *src, size_t src_len, u8 *dst)
{
	size_t i;

	if (!src_len || src_len % 3)
		return -EINVAL;

	for (i = 0; i < src_len; i += 3) {
		s32 value = (src[i] << 16) | (src[i + 1] << 8) | src[i + 2];

		if (value & BIT(23))
			value |= ~GENMASK(23, 0);
		put_unaligned_le32(value, dst + (i / 3) * 4);
	}

	return (src_len / 3) * 4;
}

static int tfa9872_append_blob_message(const u8 *raw, size_t raw_len,
				       u8 *blob, size_t *blob_len)
{
	int converted;
	size_t converted_len;

	if (!raw_len || raw_len % 3)
		return -EINVAL;
	converted_len = raw_len / 3 * 4;
	if (*blob_len + converted_len + 2 > 64 * 1024)
		return -E2BIG;

	put_unaligned_be16(converted_len, blob + *blob_len);
	*blob_len += 2;
	converted = tfa9872_convert_message(raw, raw_len, blob + *blob_len);
	if (converted < 0)
		return converted;
	*blob_len += converted;
	return 0;
}

static bool tfa9872_container_range(const struct firmware *fw, u32 offset,
				    size_t size)
{
	return offset <= fw->size && size <= fw->size - offset;
}

static int tfa9872_find_profile(struct tfa9872_priv *tfa)
{
	const struct firmware *fw = tfa->firmware;
	const u8 *data = fw->data;
	u32 entry, offset, name;
	unsigned int i, count = data[tfa->device_offset];

	if (!tfa9872_container_range(fw, tfa->device_offset + 12, count * 4))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		entry = get_unaligned_le32(data + tfa->device_offset + 12 + i * 4);
		if ((entry >> 24) != TFA9872_DESC_PROFILE)
			continue;
		offset = entry & GENMASK(23, 0);
		if (!tfa9872_container_range(fw, offset, 8) || !data[offset])
			return -EINVAL;
		if (!tfa9872_container_range(fw, offset + 8, (data[offset] - 1) * 4))
			return -EINVAL;
		entry = get_unaligned_le32(data + offset + 4);
		name = entry & GENMASK(23, 0);
		if ((entry >> 24) != 3 || !tfa9872_container_range(fw, name, 1) ||
		    !memchr(data + name, 0, fw->size - name))
			return -EINVAL;
		if (!strcmp(data + name, "stereo")) {
			tfa->profile_offset = offset;
			return 0;
		}
	}
	return -ENOENT;
}

static int tfa9872_send_profile_messages(struct tfa9872_priv *tfa)
{
	const struct firmware *fw = tfa->firmware;
	const u8 *data = fw->data;
	u32 entry, offset, length, file_size;
	unsigned int i, count = data[tfa->profile_offset] - 1;

	u8 *blob __free(kfree) = kmalloc(SZ_64K, GFP_KERNEL);
	size_t blob_len = 4;
	int ret;

	if (!blob)
		return -ENOMEM;
	blob[0] = 'm';
	blob[1] = 'm';
	for (i = 0; i < count; i++) {
		entry = get_unaligned_le32(data + tfa->profile_offset + 8 + i * 4);
		offset = entry & GENMASK(23, 0);
		if ((entry >> 24) == 17)
			break;
		if ((entry >> 24) == 21) {
			if (!tfa9872_container_range(fw, offset, 2))
				return -EINVAL;
			length = get_unaligned_le16(data + offset);
			if (!tfa9872_container_range(fw, offset + 2, length))
				return -EINVAL;
			ret = tfa9872_append_blob_message(data + offset + 2, length,
							  blob, &blob_len);
			if (ret)
				return ret;
		} else if ((entry >> 24) == TFA9872_DESC_FILE) {
			if (!tfa9872_container_range(fw, offset, 8 + 36))
				return -EINVAL;
			file_size = get_unaligned_le32(data + offset + 4);
			length = get_unaligned_le16(data + offset + 14);
			if (length < 36 || length > file_size ||
			    !tfa9872_container_range(fw, offset + 8, file_size))
				return -EINVAL;
			if (get_unaligned_le16(data + offset + 8) != TFA9872_FILE_MESSAGE)
				continue;
			ret = tfa9872_append_blob_message(data + offset + 44,
							  length - 36, blob, &blob_len);
			if (ret)
				return ret;
		}
	}
	if (blob_len == 4)
		return -ENOENT;
	put_unaligned_be16(blob_len - 4, blob + 2);
	return q6afe_tfadsp_send_msg(tfa->afe_port, blob, blob_len);
}

static void tfa9872_release_firmware(void *data)
{
	struct tfa9872_priv *tfa = data;

	if (tfa->firmware)
		release_firmware(tfa->firmware);
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
	int rate = tfa9872_rate_code(params_rate(params));
	int ret;

	if (params_channels(params) != 2 || rate < 0)
		return -EINVAL;
	if (params_width(params) != 16 && params_width(params) != 24 &&
	    params_width(params) != 32)
		return -EINVAL;

	mutex_lock(&tfa->lock);
	tfa->rate = params_rate(params);
	ret = regmap_update_bits(tfa->regmap, TFA9872_SAMPLE_RATE,
				 TFA9872_TDMFS_MASK, rate);
	mutex_unlock(&tfa->lock);

	return ret;
}

static int tfa9872_start(struct tfa9872_priv *tfa)
{
	int ret;

	if (!tfa->firmware_ready || !tfa->afe_port)
		return -EAGAIN;

	ret = tfa9872_send_profile_messages(tfa);
	if (ret) {
		dev_err(tfa->dev, "TFADSP profile messages failed: %d\n", ret);
		return ret;
	}
	ret = q6afe_tfadsp_wait_configured(tfa->afe_port);
	if (ret) {
		dev_err(tfa->dev, "TFADSP configuration incomplete: %d\n", ret);
		return ret;
	}

	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL0,
				 TFA9872_PWDN | TFA9872_DCA | TFA9872_AMPE,
				 TFA9872_DCA | TFA9872_AMPE);
	if (!ret)
		tfa->stream_active = true;

	return ret;
}

static int tfa9872_stop(struct tfa9872_priv *tfa)
{
	int ret;

	ret = regmap_update_bits(tfa->regmap, TFA9872_SYS_CONTROL0,
				 TFA9872_AMPE | TFA9872_DCA | TFA9872_PWDN,
				 TFA9872_PWDN);
	tfa->stream_active = false;
	return ret;
}

static int tfa9872_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct tfa9872_priv *tfa = snd_soc_component_get_drvdata(dai->component);
	int ret = 0;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	mutex_lock(&tfa->lock);
	if (mute) {
		ret = tfa9872_stop(tfa);
	} else {
		ret = tfa9872_start(tfa);
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

static const struct snd_soc_dai_ops tfa9872_dai_ops = {
	.hw_params = tfa9872_hw_params,
	.set_fmt = tfa9872_set_fmt,
	.mute_stream = tfa9872_mute_stream,
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
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
			 SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			 SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &tfa9872_dai_ops,
};

static const struct snd_soc_component_driver tfa9872_component = {
	.dapm_widgets = tfa9872_widgets,
	.num_dapm_widgets = ARRAY_SIZE(tfa9872_widgets),
	.dapm_routes = tfa9872_routes,
	.num_dapm_routes = ARRAY_SIZE(tfa9872_routes),
	.use_pmdown_time = 1,
};

static int tfa9872_container_validate(const struct firmware *fw)
{
	const u8 *data = fw->data;
	u32 size, crc, calculated;

	if (fw->size < 50 || fw->size > 256 * 1024 || data[0] != 'P' ||
	    data[1] != 'M')
		return -EINVAL;

	size = get_unaligned_le32(data + 6);
	crc = get_unaligned_le32(data + 10);
	if (size < 46 || size != fw->size)
		return -EINVAL;

	calculated = ~crc32_le(~0U, data + 14, size - 14);
	if (calculated != crc)
		return -EBADMSG;

	return 0;
}

static int tfa9872_container_find_device(struct tfa9872_priv *tfa,
					 const struct firmware *fw, u8 address)
{
	const u8 *data = fw->data;
	u32 count, index, offset, type;
	u16 devices;
	u32 i;

	devices = get_unaligned_le16(data + 40);
	count = get_unaligned_le16(data + 40) +
		get_unaligned_le16(data + 42) +
		get_unaligned_le16(data + 44);
	if (devices > 4 || count > 4 + 16 * 4 || 46 + count * 4 > fw->size)
		return -EINVAL;

	for (i = 0; i < devices; i++) {
		index = get_unaligned_le32(data + 46 + i * sizeof(u32));
		offset = index & GENMASK(23, 0);
		type = index >> 24;
		if (type || offset > fw->size - 12)
			return -EINVAL;
		if (!tfa9872_container_range(fw, offset + 12, data[offset] * 4))
			return -EINVAL;
		if (data[offset + 2] == address) {
			tfa->device_offset = offset;
			return 0;
		}
	}

	return -ENODEV;
}

static int tfa9872_load_firmware(struct tfa9872_priv *tfa)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, tfa->firmware_name, tfa->dev);
	if (ret)
		return ret;

	ret = tfa9872_container_validate(fw);
	if (!ret)
		ret = tfa9872_container_find_device(tfa, fw, to_i2c_client(tfa->dev)->addr);
	if (!ret) {
		tfa->firmware = fw;
		ret = tfa9872_find_profile(tfa);
		if (!ret) {
			tfa->firmware_ready = true;
			return 0;
		}
		tfa->firmware = NULL;
	}
	release_firmware(fw);
	return ret;
}

static int tfa9872_tfadsp_event(struct notifier_block *nb,
				unsigned long event, void *data)
{
	struct tfa9872_priv *tfa = container_of(nb, struct tfa9872_priv,
						tfadsp_nb);

	mutex_lock(&tfa->lock);
	switch (event) {
	case Q6AFE_TFADSP_EVENT_CLOSE:
		tfa9872_stop(tfa);
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
	tfa->regmap = devm_regmap_init_i2c(client, &tfa9872_regmap_config);
	if (IS_ERR(tfa->regmap))
		return PTR_ERR(tfa->regmap);

	ret = regmap_read(tfa->regmap, TFA9872_REVISION, &revision);
	if (ret)
		return ret;
	if ((revision & 0xff) != 0x72)
		return -ENODEV;
	tfa->revision = revision;

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
	ret = tfa9872_load_firmware(tfa);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to load %s\n", tfa->firmware_name);
	dev_info(&client->dev, "revision %#x, stereo profile ready\n", revision);
	ret = devm_add_action_or_reset(&client->dev, tfa9872_release_firmware, tfa);
	if (ret)
		return ret;
	return devm_snd_soc_register_component(&client->dev, &tfa9872_component,
					       &tfa9872_dai, 1);
}

static void tfa9872_shutdown(struct i2c_client *client)
{
	struct tfa9872_priv *tfa = i2c_get_clientdata(client);

	guard(mutex)(&tfa->lock);
	tfa9872_stop(tfa);
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
