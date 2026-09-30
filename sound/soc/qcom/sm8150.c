// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <dt-bindings/sound/qcom,q6dsp-lpass-ports.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <sound/core.h>
#include <sound/soc-jack.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-card.h>
#include <uapi/linux/input-event-codes.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define DRIVER_NAME		"sm8150"
#define DEFAULT_SAMPLE_RATE_48K	48000
#define SLIM_MAX_TX_PORTS 16
#define SLIM_MAX_RX_PORTS 13
#define WCD934X_DEFAULT_MCLK_RATE	9600000
#define TERT_MI2S_BCLK_RATE		1536000

struct sm8150_snd_data {
	struct snd_soc_jack jack;
	struct snd_soc_component *wcd_component;
	struct snd_soc_component *es_component;
	struct notifier_block jack_notifier;
	struct gpio_desc *hph_en0;
	struct gpio_desc *hph_en1;
	struct pinctrl *pinctrl;
	struct pinctrl_state *tert_mi2s_active;
	struct pinctrl_state *tert_mi2s_sleep;
	unsigned int tert_mi2s_clk_count;
	bool jack_setup;
	bool slim_port_setup;
	bool jack_notifier_registered;
};

static struct snd_soc_jack_pin sm8150_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE | SND_JACK_LINEOUT,
	},
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int sm8150_jack_status_changed(struct notifier_block *notifier,
				       unsigned long status, void *jack)
{
	struct sm8150_snd_data *data = container_of(notifier,
							    struct sm8150_snd_data,
							    jack_notifier);
	bool present = status & (SND_JACK_HEADPHONE | SND_JACK_LINEOUT);

	/* The external DAC serves headphones and auxiliary loads alike. */
	snd_soc_dapm_disable_pin(data->jack.card->dapm, "WCD Headphone Jack");
	if (present)
		snd_soc_dapm_enable_pin(data->jack.card->dapm,
				       "ES9218P Headphone Jack");
	else
		snd_soc_dapm_disable_pin(data->jack.card->dapm,
					"ES9218P Headphone Jack");
	snd_soc_dapm_sync(data->jack.card->dapm);

	return NOTIFY_OK;
}

static int sm8150_late_probe(struct snd_soc_card *card)
{
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *es_dai;
	int ret;

	if (!data->wcd_component)
		return -ENODEV;

	es_dai = snd_soc_card_get_codec_dai(card, "es9218-hifi");
	if (es_dai) {
		data->es_component = es_dai->component;
		ret = snd_soc_component_set_jack(data->es_component, &data->jack,
						data->wcd_component);
		if (ret)
			return ret;
	}

	data->jack_notifier.notifier_call = sm8150_jack_status_changed;
	snd_soc_jack_notifier_register(&data->jack, &data->jack_notifier);
	data->jack_notifier_registered = true;
	sm8150_jack_status_changed(&data->jack_notifier, 0, &data->jack);

	return snd_soc_component_set_jack(data->wcd_component, &data->jack, NULL);
}

static void sm8150_dai_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	if (data->jack_notifier_registered) {
		snd_soc_component_set_jack(data->wcd_component, NULL, NULL);
		snd_soc_jack_notifier_unregister(&data->jack, &data->jack_notifier);
		data->jack_notifier_registered = false;
	}
	if (data->es_component) {
		snd_soc_component_set_jack(data->es_component, NULL, NULL);
		data->es_component = NULL;
	}
	data->wcd_component = NULL;
	data->jack_setup = false;
	data->slim_port_setup = false;
}

static int sm8150_hifi_switch_event(struct snd_soc_dapm_widget *widget,
				    struct snd_kcontrol *control, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(widget->dapm);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(card);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (data->hph_en1)
			gpiod_set_value_cansleep(data->hph_en1, 1);
		usleep_range(5000, 5010);
		if (data->hph_en0)
			gpiod_set_value_cansleep(data->hph_en0, 1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (data->hph_en1)
			gpiod_set_value_cansleep(data->hph_en1, 0);
		if (data->hph_en0)
			gpiod_set_value_cansleep(data->hph_en0, 0);
		break;
	}

	return 0;
}

static int sm8150_tert_mi2s_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	int width = params_width(params);
	int ret;

	if (params_rate(params) != DEFAULT_SAMPLE_RATE_48K ||
	    params_channels(params) != 2 || width != 16)
		return -EINVAL;

	ret = snd_soc_dai_set_fmt(cpu_dai,
				 SND_SOC_DAIFMT_BP_FP |
				 SND_SOC_DAIFMT_NB_NF |
				 SND_SOC_DAIFMT_I2S);
	if (ret)
		return ret;

	return snd_soc_dai_set_fmt(codec_dai,
				   SND_SOC_DAIFMT_BC_FC |
				   SND_SOC_DAIFMT_NB_NF |
				   SND_SOC_DAIFMT_I2S);
}

static int sm8150_tert_mi2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret = 0;
	bool first = data->tert_mi2s_clk_count == 0;

	if (data->tert_mi2s_active) {
		ret = pinctrl_select_state(data->pinctrl, data->tert_mi2s_active);
		if (ret)
			return ret;
	}
	if (first)
		ret = snd_soc_dai_set_sysclk(cpu_dai,
					     Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
					     TERT_MI2S_BCLK_RATE,
					     SNDRV_PCM_STREAM_PLAYBACK);
	if (ret < 0) {
		if (first) {
			snd_soc_dai_set_sysclk(cpu_dai,
					      Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT, 0,
					      SNDRV_PCM_STREAM_PLAYBACK);
			if (data->tert_mi2s_sleep)
				pinctrl_select_state(data->pinctrl,
						     data->tert_mi2s_sleep);
		}
		return ret;
	}
	data->tert_mi2s_clk_count++;

	return ret;
}

static void sm8150_tert_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	if (data->tert_mi2s_clk_count && --data->tert_mi2s_clk_count == 0) {
		snd_soc_dai_set_sysclk(cpu_dai,
				      Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT, 0,
				      SNDRV_PCM_STREAM_PLAYBACK);
		if (data->tert_mi2s_sleep)
			pinctrl_select_state(data->pinctrl, data->tert_mi2s_sleep);
	}
}

static const struct snd_soc_ops sm8150_tert_mi2s_ops = {
	.hw_params = sm8150_tert_mi2s_hw_params,
	.startup = sm8150_tert_mi2s_startup,
	.shutdown = sm8150_tert_mi2s_shutdown,
};

static int sm8150_slim_snd_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0, i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);

		if (ret != 0 && ret != -ENOTSUPP) {
			pr_err("failed to get codec chan map, err:%d\n", ret);
			return ret;
		} else if (ret == -ENOTSUPP) {
			/* Ignore unsupported */
			continue;
		}

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
							  rx_ch_cnt, rx_ch);
		else
			ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt,
							  tx_ch, 0, NULL);
		if (ret != 0 && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "failed to set cpu chan map, err:%d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int sm8150_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct sm8150_snd_data *pdata = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai_link *link = rtd->dai_link;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_jack *jack;
	/*
	 * Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13
	 * TX14, TX15, TX16
	 */
	unsigned int rx_ch[SLIM_MAX_RX_PORTS] = {144, 145, 146, 147, 148, 149,
					150, 151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[SLIM_MAX_TX_PORTS] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};
	int rval, i;

	if (!pdata->jack_setup) {
		pdata->jack.status = 0;
		rval = snd_soc_card_jack_new_pins(card, "Headset Jack",
						  SND_JACK_HEADSET |
						  SND_JACK_HEADPHONE |
						  SND_JACK_LINEOUT |
						  SND_JACK_MECHANICAL |
						  SND_JACK_BTN_0 | SND_JACK_BTN_1 |
						  SND_JACK_BTN_2 | SND_JACK_BTN_3,
						  &pdata->jack,
						  sm8150_jack_pins,
						  ARRAY_SIZE(sm8150_jack_pins));

		if (rval < 0) {
			dev_err(card->dev, "Unable to add Headphone Jack\n");
			return rval;
		}

		jack = pdata->jack.jack;
		snd_jack_set_key(jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
		snd_jack_set_key(jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
		pdata->jack_setup = true;
	}

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		/* setting up wcd multiple times for slim port is redundant */
		if (pdata->slim_port_setup || !link->no_pcm)
			return 0;

		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			rval = snd_soc_dai_set_channel_map(codec_dai,
							  ARRAY_SIZE(tx_ch),
							  tx_ch,
							  ARRAY_SIZE(rx_ch),
							  rx_ch);
			if (rval != 0 && rval != -ENOTSUPP)
				return rval;

			snd_soc_dai_set_sysclk(codec_dai, 0,
					       WCD934X_DEFAULT_MCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);

			if (strstr(dev_name(codec_dai->component->dev), "wcd934x"))
				pdata->wcd_component = codec_dai->component;
		}
		pdata->slim_port_setup = true;

		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_ops sm8150_be_ops = {
	.hw_params = sm8150_slim_snd_hw_params,
};

static int sm8150_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 2;
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8150_slim_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 1;
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8150_mi2s_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					 struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							 SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params,
						SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 2;
	snd_mask_none(format);
	snd_mask_set_format(format, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static const struct snd_soc_dapm_widget sm8150_snd_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_HP("WCD Headphone Jack", NULL),
	SND_SOC_DAPM_HP("ES9218P Headphone Jack",
			 sm8150_hifi_switch_event),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Handset 2nd Mic", NULL),
	SND_SOC_DAPM_MIC("Handset 3rd Mic", NULL),
	SND_SOC_DAPM_MIC("Digital Mic0", NULL),
};

static const struct snd_kcontrol_new sm8150_snd_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("WCD Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("ES9218P Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Handset Mic"),
};

static void sm8150_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->id == TERTIARY_MI2S_RX) {
			link->ops = &sm8150_tert_mi2s_ops;
			link->be_hw_params_fixup = sm8150_mi2s_be_hw_params_fixup;
		} else if (link->no_pcm == 1) {
			link->ops = &sm8150_be_ops;
			if (link->id == SLIMBUS_0_TX)
				link->be_hw_params_fixup =
					sm8150_slim_tx_be_hw_params_fixup;
			else
				link->be_hw_params_fixup =
					sm8150_be_hw_params_fixup;
		}
		link->init = sm8150_dai_init;
		link->exit = sm8150_dai_exit;
	}
}

static int sm8150_snd_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct sm8150_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	/* Allocate the private data */
	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->hph_en0 = devm_gpiod_get_optional(dev, "hph-en0",
						 GPIOD_OUT_LOW);
	if (IS_ERR(data->hph_en0))
		return dev_err_probe(dev, PTR_ERR(data->hph_en0),
				     "failed to get HPH enable GPIO0\n");
	data->hph_en1 = devm_gpiod_get_optional(dev, "hph-en1",
						 GPIOD_OUT_LOW);
	if (IS_ERR(data->hph_en1))
		return dev_err_probe(dev, PTR_ERR(data->hph_en1),
				     "failed to get HPH enable GPIO1\n");
	if (!!data->hph_en0 != !!data->hph_en1)
		return dev_err_probe(dev, -EINVAL,
				     "both HPH enable GPIOs must be provided\n");

	data->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(data->pinctrl)) {
		if (PTR_ERR(data->pinctrl) == -EPROBE_DEFER)
			return PTR_ERR(data->pinctrl);
		data->pinctrl = NULL;
	} else {
		data->tert_mi2s_active = pinctrl_lookup_state(data->pinctrl,
							    "tert-mi2s-active");
		data->tert_mi2s_sleep = pinctrl_lookup_state(data->pinctrl,
							   "tert-mi2s-sleep");
		if (IS_ERR(data->tert_mi2s_active) ||
		    IS_ERR(data->tert_mi2s_sleep))
			return dev_err_probe(dev, -EINVAL,
					     "missing tertiary MI2S pin states\n");
	}

	card->late_probe = sm8150_late_probe;
	card->driver_name = DRIVER_NAME;
	card->dapm_widgets = sm8150_snd_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(sm8150_snd_widgets);
	card->controls = sm8150_snd_controls;
	card->num_controls = ARRAY_SIZE(sm8150_snd_controls);
	card->dev = dev;
	card->owner = THIS_MODULE;
	dev_set_drvdata(dev, card);
	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;
	snd_soc_card_set_drvdata(card, data);

	sm8150_add_ops(card);
	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id sm8150_snd_device_id[]  = {
	{ .compatible = "qcom,sm8150-sndcard" },
	{},
};
MODULE_DEVICE_TABLE(of, sm8150_snd_device_id);

static struct platform_driver sm8150_snd_driver = {
	.probe = sm8150_snd_platform_probe,
	.driver = {
		.name = "msm-snd-sm8150",
		.of_match_table = sm8150_snd_device_id,
	},
};
module_platform_driver(sm8150_snd_driver);

MODULE_DESCRIPTION("sm8150 ASoC Machine Driver");
MODULE_LICENSE("GPL");
MODULE_SOFTDEP("pre: snd-soc-es9218p");
