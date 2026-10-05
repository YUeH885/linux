// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <dt-bindings/sound/qcom,q6asm.h>
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
#include <sound/hdmi-codec.h>
#include <sound/soc.h>
#include <sound/soc-card.h>
#include <sound/qcom/q6afe-tfadsp.h>
#include <uapi/linux/input-event-codes.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define DRIVER_NAME		"sm8150"
#define DEFAULT_SAMPLE_RATE_48K	48000
#define SLIM_MAX_TX_PORTS 16
#define SLIM_MAX_RX_PORTS 13
#define WCD934X_DEFAULT_MCLK_RATE	9600000
struct sm8150_snd_data {
	struct snd_soc_jack jack;
	struct snd_soc_jack dp_jack;
	struct snd_soc_dai *dp_dai;
	struct snd_soc_component *wcd_component;
	struct snd_soc_component *es_component;
	struct notifier_block jack_notifier;
	struct gpio_desc *hph_en0;
	struct gpio_desc *hph_en1;
	struct pinctrl *pinctrl;
	struct pinctrl_state *tert_mi2s_active;
	struct pinctrl_state *tert_mi2s_sleep;
	struct pinctrl_state *sec_mi2s_active;
	struct pinctrl_state *sec_mi2s_sleep;
	unsigned int tert_mi2s_clk_count;
	unsigned int sec_mi2s_clk_count;
	struct q6afe_port *speaker_rx;
	struct q6afe_port *speaker_feedback;
	bool jack_setup;
	bool slim_port_setup;
	bool jack_notifier_registered;
};

static int sm8150_sec_mi2s_hw_params(struct snd_pcm_substream *substream,
				     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	unsigned int width = params_width(params);
	unsigned int rate = params_rate(params);
	unsigned int bclk_rate;
	int ret, i;

	if (params_channels(params) != 2 || (width != 16 && width != 24 &&
					     width != 32) || rate != 48000)
		return -EINVAL;

	ret = snd_soc_dai_set_fmt(cpu_dai,
				  SND_SOC_DAIFMT_BP_FP |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_I2S);
	if (ret)
		return ret;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_fmt(codec_dai,
					  SND_SOC_DAIFMT_BC_FC |
					  SND_SOC_DAIFMT_NB_NF |
					  SND_SOC_DAIFMT_I2S);
		if (ret)
			return ret;
	}

	bclk_rate = rate * params_channels(params) * 32;
	return snd_soc_dai_set_sysclk(cpu_dai,
				      Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT,
				      bclk_rate, SNDRV_PCM_STREAM_PLAYBACK);
}

static int sm8150_sec_mi2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	int ret;

	if (!data->sec_mi2s_clk_count && data->sec_mi2s_active) {
		ret = pinctrl_select_state(data->pinctrl, data->sec_mi2s_active);
		if (ret)
			return ret;
	}
	data->sec_mi2s_clk_count++;
	return 0;
}

static void sm8150_sec_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	int ret;

	if (data->sec_mi2s_clk_count && --data->sec_mi2s_clk_count == 0) {
		ret = snd_soc_dai_set_sysclk(cpu_dai,
					     Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT, 0,
					     SNDRV_PCM_STREAM_PLAYBACK);
		if (ret)
			dev_err(rtd->dev, "failed to disable secondary MI2S clock: %d\n", ret);
		if (data->sec_mi2s_sleep) {
			ret = pinctrl_select_state(data->pinctrl, data->sec_mi2s_sleep);
			if (ret)
				dev_err(rtd->dev, "failed to restore secondary MI2S pins: %d\n",
					ret);
		}
	}
}

static int sm8150_sec_mi2s_fixup(struct snd_soc_pcm_runtime *rtd,
				 struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = 48000;
	rate->max = 48000;
	channels->min = 2;
	channels->max = 2;
	snd_mask_none(format);
	/* Stock SEC RX/TX use 24-bit samples in two 32-bit wire slots. */
	snd_mask_set_format(format, SNDRV_PCM_FORMAT_S24_LE);
	return 0;
}

static int sm8150_speaker_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct q6afe_i2s_cfg config = {
		.sample_rate = 48000,
		.bit_width = 24,
		.num_channels = 2,
		.sd_line_mask = BIT(0),
		.fmt = SND_SOC_DAIFMT_BP_FP | SND_SOC_DAIFMT_NB_NF |
		       SND_SOC_DAIFMT_I2S,
	};
	/* TX consumes the amplifier's current/voltage slots without a host PCM. */
	return q6afe_i2s_port_prepare(data->speaker_feedback, &config);
}

static const struct snd_soc_ops sm8150_sec_mi2s_ops = {
	.hw_params = sm8150_sec_mi2s_hw_params,
	.startup = sm8150_sec_mi2s_startup,
	.shutdown = sm8150_sec_mi2s_shutdown,
	.prepare = sm8150_speaker_prepare,
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

static int sm8150_enable_route(struct snd_soc_card *card, const char *name,
			       bool fixed)
{
	struct snd_kcontrol *control = snd_soc_card_get_kcontrol(card, name);
	struct snd_ctl_elem_value value = {};
	int ret;

	if (!control)
		return -ENOENT;
	value.value.integer.value[0] = 1;
	ret = control->put(control, &value);
	if (ret < 0)
		return ret;
	if (fixed) {
		/* This dedicated PCM must remain reachable across ALSA state restore. */
		control->vd[0].access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE;
		control->put = NULL;
	}
	return 0;
}

static int sm8150_dp_init(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_pcm_runtime *fe;
	int ret;

	for_each_card_rtds(rtd->card, fe) {
		if (!fe->dai_link->dynamic ||
		    fe->dai_link->id != MSM_FRONTEND_DAI_MULTIMEDIA3)
			continue;
		ret = hdmi_codec_set_pcm(codec_dai, fe);
		if (ret)
			return ret;
		ret = qcom_snd_dp_jack_setup(rtd, &data->dp_jack, 0);
		if (ret)
			return ret;
		data->dp_dai = codec_dai;
		return 0;
	}
	return -ENODEV;
}

static void sm8150_dp_exit(struct snd_soc_pcm_runtime *rtd)
{
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	snd_soc_component_set_jack(snd_soc_rtd_to_codec(rtd, 0)->component, NULL, NULL);
	data->dp_dai = NULL;
}

static int sm8150_dp_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);

	return hdmi_codec_hw_constraint_eld(data->dp_dai, substream->runtime);
}

static int sm8150_dp_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(rtd->card);
	unsigned int map[8];
	int ret;

	ret = hdmi_codec_get_chmap(data->dp_dai, params_channels(params), map);
	if (ret < 0)
		return ret;
	return snd_soc_dai_set_channel_map(snd_soc_rtd_to_cpu(rtd, 0),
					 params_channels(params), map, 0, NULL);
}

static const struct snd_soc_ops sm8150_dp_fe_ops = {
	.startup = sm8150_dp_startup,
	.hw_params = sm8150_dp_hw_params,
};

static int sm8150_dp_prepare(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);

	if (hdmi_codec_is_plugged(codec_dai) == 0)
		/* Stop AFE before the codec's offline prepare releases DP clocks. */
		return snd_soc_dai_prepare(snd_soc_rtd_to_cpu(rtd, 0), substream);
	/* Establish DP audio clocks before AFE start, including PCM re-prepare. */
	return snd_soc_dai_prepare(codec_dai, substream);
}

static const struct snd_soc_ops sm8150_dp_be_ops = {
	.prepare = sm8150_dp_prepare,
};

static int sm8150_late_probe(struct snd_soc_card *card)
{
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *es_dai;
	int ret;

	if (!data->wcd_component)
		return -ENODEV;

	es_dai = snd_soc_card_get_codec_dai(card, "es9218-hifi");
	if (data->speaker_feedback || data->dp_dai) {
		/* Make the shared playback PCM usable before desktop port probing. */
		ret = snd_soc_dapm_new_widgets(card);
		if (ret)
			return ret;
	}
	if (data->dp_dai) {
		ret = sm8150_enable_route(card, "DISPLAY_PORT_RX Audio Mixer MultiMedia3", true);
		if (ret)
			return ret;
	}
	if (data->speaker_feedback) {
		/* The external DAC allows probing independently of speaker firmware. */
		ret = sm8150_enable_route(card, es_dai ?
					 "TERT_MI2S_RX Audio Mixer MultiMedia1" :
					 "SEC_MI2S_RX Audio Mixer MultiMedia1", false);
		if (ret < 0)
			return ret;
	}

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

	if (rtd->dai_link->id == SECONDARY_MI2S_RX) {
		int ret = q6afe_tfadsp_set_feedback(data->speaker_rx, NULL);

		if (ret)
			dev_err(rtd->dev, "failed to detach speaker feedback: %d\n", ret);
		q6afe_tfadsp_put_port(data->speaker_feedback);
		q6afe_tfadsp_put_port(data->speaker_rx);
		data->speaker_feedback = NULL;
		data->speaker_rx = NULL;
	}
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
	unsigned int width = params_width(params);
	unsigned int rate = params_rate(params);
	unsigned int bclk_rate;
	int ret;

	if (params_channels(params) != 2 || (width != 16 && width != 32))
		return -EINVAL;

	ret = snd_soc_dai_set_fmt(cpu_dai,
				 SND_SOC_DAIFMT_BP_FP |
				 SND_SOC_DAIFMT_NB_NF |
				 SND_SOC_DAIFMT_I2S);
	if (ret)
		return ret;

	ret = snd_soc_dai_set_fmt(codec_dai,
				  SND_SOC_DAIFMT_BC_FC |
				  SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_I2S);
	if (ret)
		return ret;

	bclk_rate = rate * params_channels(params) * width;
	return snd_soc_dai_set_sysclk(cpu_dai,
				      Q6AFE_LPASS_CLK_ID_TER_MI2S_IBIT,
				      bclk_rate, SNDRV_PCM_STREAM_PLAYBACK);
}

static int sm8150_tert_mi2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_card *card = rtd->card;
	struct sm8150_snd_data *data = snd_soc_card_get_drvdata(card);
	int ret = 0;

	if (data->tert_mi2s_active) {
		ret = pinctrl_select_state(data->pinctrl, data->tert_mi2s_active);
		if (ret)
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
	case SECONDARY_MI2S_RX: {
		struct snd_soc_dai *tfa_dai = codec_dai;
		struct snd_soc_dai *dai;

		for_each_rtd_codec_dais(rtd, i, dai) {
			if (of_property_present(dai->component->dev->of_node,
						"qcom,q6afe")) {
				tfa_dai = dai;
				break;
			}
		}

		pdata->speaker_rx = q6afe_tfadsp_get_port(tfa_dai->component->dev,
						       SECONDARY_MI2S_RX);
		if (IS_ERR(pdata->speaker_rx)) {
			rval = PTR_ERR(pdata->speaker_rx);
			pdata->speaker_rx = NULL;
			return rval;
		}
		pdata->speaker_feedback = q6afe_tfadsp_get_port(tfa_dai->component->dev,
							     SECONDARY_MI2S_TX);
		if (IS_ERR(pdata->speaker_feedback)) {
			rval = PTR_ERR(pdata->speaker_feedback);
			pdata->speaker_feedback = NULL;
			q6afe_tfadsp_put_port(pdata->speaker_rx);
			pdata->speaker_rx = NULL;
			return rval;
		}
		rval = q6afe_tfadsp_set_feedback(pdata->speaker_rx,
						 pdata->speaker_feedback);
		if (rval) {
			q6afe_tfadsp_put_port(pdata->speaker_feedback);
			q6afe_tfadsp_put_port(pdata->speaker_rx);
			pdata->speaker_feedback = NULL;
			pdata->speaker_rx = NULL;
			return rval;
		}
		break;
	}
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
	channels->min = channels->max = clamp_t(unsigned int, channels->min, 1, 4);
	snd_mask_none(fmt);
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static int sm8150_mi2s_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
					 struct snd_pcm_hw_params *params)
{
	static const unsigned int tert_mi2s_rates[] = {
		8000, 11025, 16000, 22050, 32000, 48000,
		96000, 176400, 192000,
	};
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
							 SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *format = hw_param_mask(params,
						SNDRV_PCM_HW_PARAM_FORMAT);

	int ret;

	ret = snd_interval_list(rate, ARRAY_SIZE(tert_mi2s_rates),
				tert_mi2s_rates, 0);
	if (ret < 0)
		return ret;
	channels->min = channels->max = 2;
	snd_mask_none(format);
	snd_mask_set_format(format, SNDRV_PCM_FORMAT_S16_LE);
	snd_mask_set_format(format, SNDRV_PCM_FORMAT_S32_LE);

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

/* Jack-owned pins must not be overwritten by ALSA state restoration. */
#define SM8150_JACK_PIN_CONTROL(xname, xpin) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, \
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE, \
	.info = snd_soc_dapm_info_pin_switch, \
	.get = snd_soc_dapm_get_pin_switch, \
	.private_value = (unsigned long)xpin }

static const struct snd_kcontrol_new sm8150_snd_controls[] = {
	SM8150_JACK_PIN_CONTROL("Headphone Jack Switch", "Headphone Jack"),
	SM8150_JACK_PIN_CONTROL("WCD Headphone Jack Switch", "WCD Headphone Jack"),
	SM8150_JACK_PIN_CONTROL("ES9218P Headphone Jack Switch", "ES9218P Headphone Jack"),
	SM8150_JACK_PIN_CONTROL("Headset Mic Switch", "Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Handset Mic"),
};

static void sm8150_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;
	bool has_dp = false;

	for_each_card_prelinks(card, i, link)
		if (link->no_pcm && link->id == DISPLAY_PORT_RX)
			has_dp = true;
	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm && link->id == DISPLAY_PORT_RX) {
			link->init = sm8150_dp_init;
			link->exit = sm8150_dp_exit;
			link->ops = &sm8150_dp_be_ops;
			continue;
		}
		if (has_dp && link->dynamic && link->id == MSM_FRONTEND_DAI_MULTIMEDIA3) {
			link->ops = &sm8150_dp_fe_ops;
			link->dpcm_merged_format = 1;
			link->dpcm_merged_chan = 1;
			link->dpcm_merged_rate = 1;
		} else if (link->id == TERTIARY_MI2S_RX) {
			link->ops = &sm8150_tert_mi2s_ops;
			link->be_hw_params_fixup = sm8150_mi2s_be_hw_params_fixup;
		} else if (link->id == SECONDARY_MI2S_RX) {
			link->ops = &sm8150_sec_mi2s_ops;
			link->be_hw_params_fixup = sm8150_sec_mi2s_fixup;
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
		data->sec_mi2s_active = pinctrl_lookup_state(data->pinctrl,
							     "sec-mi2s-active");
		data->sec_mi2s_sleep = pinctrl_lookup_state(data->pinctrl,
							    "sec-mi2s-sleep");
		if (IS_ERR(data->sec_mi2s_active) && IS_ERR(data->sec_mi2s_sleep)) {
			data->sec_mi2s_active = NULL;
			data->sec_mi2s_sleep = NULL;
		} else if (IS_ERR(data->sec_mi2s_active) || IS_ERR(data->sec_mi2s_sleep)) {
			return dev_err_probe(dev, -EINVAL,
					     "missing secondary MI2S pin states\n");
		}
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
