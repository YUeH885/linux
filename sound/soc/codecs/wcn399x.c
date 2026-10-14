// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 yuzu

#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/slimbus.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define WCN399X_REG_BASE		0x800
#define WCN399X_RX_CONFIG		(WCN399X_REG_BASE + 0x40)
#define WCN399X_TX_CONFIG		(WCN399X_REG_BASE + 0x50)
#define WCN399X_TX_MULTI_CHANNEL	(WCN399X_REG_BASE + 0x100)
#define WCN399X_RX_MULTI_CHANNEL	(WCN399X_REG_BASE + 0x180)
#define WCN399X_TX_RECOVERY	(WCN399X_REG_BASE + 0x1f0)
#define WCN399X_RX_CHANNEL	157
#define WCN399X_TX_CHANNEL	161
#define WCN399X_PORT_ENABLE	BIT(0)
#define WCN399X_TX_WATERMARK	BIT(1)
#define WCN399X_RX_WATERMARK	(0xb << 1)
#define WCN399X_TX_AUTO_RECOVERY	GENMASK(1, 0)

struct wcn399x_stream {
	struct slim_stream_runtime *runtime;
	struct slim_stream_config config;
	unsigned int channel;
	bool running;
};

struct wcn399x {
	struct slim_device *sdev;
	struct slim_device *ifd;
	struct wcn399x_stream streams[2];
};

static int wcn399x_port_enable(struct wcn399x *wcn, int direction, bool enable)
{
	u32 reg;
	u8 value;
	int ret;

	if (direction == SNDRV_PCM_STREAM_CAPTURE) {
		reg = WCN399X_TX_CONFIG;
		value = WCN399X_PORT_ENABLE | WCN399X_TX_WATERMARK;
		if (enable) {
			ret = slim_writeb(wcn->ifd, WCN399X_TX_MULTI_CHANNEL, 1);
			if (ret)
				return ret;
			ret = slim_writeb(wcn->ifd, WCN399X_TX_RECOVERY, WCN399X_TX_AUTO_RECOVERY);
			if (ret)
				return ret;
		}
	} else {
		reg = WCN399X_RX_CONFIG;
		value = WCN399X_PORT_ENABLE | WCN399X_RX_WATERMARK;
		if (enable) {
			ret = slim_writeb(wcn->ifd, WCN399X_RX_MULTI_CHANNEL, 1);
			if (ret)
				return ret;
		}
	}

	return slim_writeb(wcn->ifd, reg, enable ? value : 0);
}

static int wcn399x_stop(struct wcn399x *wcn, int direction)
{
	struct wcn399x_stream *stream = &wcn->streams[direction];
	int ret, err;

	if (!stream->running)
		return 0;

	ret = slim_stream_disable(stream->runtime);
	err = slim_stream_unprepare(stream->runtime);
	ret = ret ?: err;
	err = wcn399x_port_enable(wcn, direction, false);
	stream->running = false;
	return ret ?: err;
}

static int wcn399x_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct wcn399x *wcn = snd_soc_component_get_drvdata(dai->component);
	int ret;

	ret = pm_runtime_resume_and_get(wcn->sdev->dev.parent);
	if (ret < 0)
		return ret;
	wcn->streams[substream->stream].runtime =
		slim_stream_allocate(wcn->sdev, "WCN399x-SCO");
	if (IS_ERR(wcn->streams[substream->stream].runtime)) {
		ret = PTR_ERR(wcn->streams[substream->stream].runtime);
		wcn->streams[substream->stream].runtime = NULL;
		pm_runtime_put_autosuspend(wcn->sdev->dev.parent);
		return ret;
	}
	return 0;
}

static void wcn399x_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct wcn399x *wcn = snd_soc_component_get_drvdata(dai->component);
	struct wcn399x_stream *stream = &wcn->streams[substream->stream];

	slim_stream_free(stream->runtime);
	stream->runtime = NULL;
	pm_runtime_mark_last_busy(wcn->sdev->dev.parent);
	pm_runtime_put_autosuspend(wcn->sdev->dev.parent);
}

static int wcn399x_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct wcn399x *wcn = snd_soc_component_get_drvdata(dai->component);
	struct wcn399x_stream *stream = &wcn->streams[substream->stream];
	int ret;

	ret = slim_get_logical_addr(wcn->sdev);
	if (ret)
		return ret;
	ret = slim_get_logical_addr(wcn->ifd);
	if (ret)
		return ret;

	stream->channel = substream->stream == SNDRV_PCM_STREAM_CAPTURE ?
		WCN399X_TX_CHANNEL : WCN399X_RX_CHANNEL;
	stream->config = (struct slim_stream_config) {
		.rate = params_rate(params),
		.bps = 16,
		.ch_count = 1,
		.chs = &stream->channel,
		.port_mask = substream->stream == SNDRV_PCM_STREAM_CAPTURE ? BIT(0) : BIT(16),
		.direction = substream->stream,
	};
	return 0;
}

static int wcn399x_prepare(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct wcn399x *wcn = snd_soc_component_get_drvdata(dai->component);
	struct wcn399x_stream *stream = &wcn->streams[substream->stream];
	int ret, err;

	ret = wcn399x_stop(wcn, substream->stream);
	if (ret)
		return ret;
	ret = wcn399x_port_enable(wcn, substream->stream, true);
	if (ret)
		goto disable_port;
	ret = slim_stream_prepare(stream->runtime, &stream->config);
	if (ret)
		goto disable_port;
	ret = slim_stream_enable(stream->runtime);
	if (ret) {
		err = slim_stream_disable(stream->runtime);
		if (err)
			dev_err(dai->dev, "failed to remove SCO channels: %d\n", err);
		err = slim_stream_unprepare(stream->runtime);
		if (err)
			dev_err(dai->dev, "failed to unprepare SCO stream: %d\n", err);
		goto disable_port;
	}
	stream->running = true;
	return 0;

disable_port:
	err = wcn399x_port_enable(wcn, substream->stream, false);
	if (err)
		dev_err(dai->dev, "failed to disable SCO port: %d\n", err);
	return ret;
}

static int wcn399x_hw_free(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct wcn399x *wcn = snd_soc_component_get_drvdata(dai->component);

	return wcn399x_stop(wcn, substream->stream);
}

static int wcn399x_get_channel_map(const struct snd_soc_dai *dai,
				   unsigned int *tx_num, unsigned int *tx_slot,
				   unsigned int *rx_num, unsigned int *rx_slot)
{
	*tx_num = 1;
	*rx_num = 1;
	tx_slot[0] = WCN399X_TX_CHANNEL;
	rx_slot[0] = WCN399X_RX_CHANNEL;
	return 0;
}

static const struct snd_soc_dai_ops wcn399x_dai_ops = {
	.startup = wcn399x_startup,
	.shutdown = wcn399x_shutdown,
	.hw_params = wcn399x_hw_params,
	.prepare = wcn399x_prepare,
	.hw_free = wcn399x_hw_free,
	.get_channel_map = wcn399x_get_channel_map,
};

static struct snd_soc_dai_driver wcn399x_dai = {
	.name = "wcn399x-sco",
	.playback = {
		.stream_name = "WCN399x SCO Playback",
		.channels_min = 1,
		.channels_max = 1,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "WCN399x SCO Capture",
		.channels_min = 1,
		.channels_max = 1,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.symmetric_rate = 1,
	.ops = &wcn399x_dai_ops,
};

static const struct snd_soc_dapm_widget wcn399x_widgets[] = {
	SND_SOC_DAPM_OUTPUT("Bluetooth Speaker"),
	SND_SOC_DAPM_INPUT("Bluetooth Microphone"),
};

static const struct snd_soc_dapm_route wcn399x_routes[] = {
	{ "Bluetooth Speaker", NULL, "WCN399x SCO Playback" },
	{ "WCN399x SCO Capture", NULL, "Bluetooth Microphone" },
};

static const struct snd_soc_component_driver wcn399x_component = {
	.dapm_widgets = wcn399x_widgets,
	.num_dapm_widgets = ARRAY_SIZE(wcn399x_widgets),
	.dapm_routes = wcn399x_routes,
	.num_dapm_routes = ARRAY_SIZE(wcn399x_routes),
};

static void wcn399x_put_ifd(void *data)
{
	struct slim_device *ifd = data;

	put_device(&ifd->dev);
}

static int wcn399x_probe(struct slim_device *sdev)
{
	struct device *dev = &sdev->dev;
	struct device_node *node;
	struct wcn399x *wcn;
	int ret;

	if (sdev->e_addr.dev_index != 1)
		return -ENODEV;

	wcn = devm_kzalloc(dev, sizeof(*wcn), GFP_KERNEL);
	if (!wcn)
		return -ENOMEM;
	wcn->sdev = sdev;
	node = of_parse_phandle(dev->of_node, "slim-ifc-dev", 0);
	if (!node)
		return -EINVAL;
	wcn->ifd = of_slim_get_device(sdev->ctrl, node);
	of_node_put(node);
	if (!wcn->ifd)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, wcn399x_put_ifd, wcn->ifd);
	if (ret)
		return ret;
	dev_set_drvdata(dev, wcn);
	return devm_snd_soc_register_component(dev, &wcn399x_component, &wcn399x_dai, 1);
}

static const struct of_device_id wcn399x_of_match[] = {
	{ .compatible = "slim217,220" },
	{ }
};
MODULE_DEVICE_TABLE(of, wcn399x_of_match);

static const struct slim_device_id wcn399x_slim_id[] = {
	{ .manf_id = 0x217, .prod_code = 0x220, .dev_index = 1, .instance = 0 },
	{ }
};
MODULE_DEVICE_TABLE(slim, wcn399x_slim_id);

static struct slim_driver wcn399x_driver = {
	.driver = {
		.name = "wcn399x-codec",
		.of_match_table = wcn399x_of_match,
	},
	.probe = wcn399x_probe,
	.id_table = wcn399x_slim_id,
};
module_slim_driver(wcn399x_driver);

MODULE_DESCRIPTION("Qualcomm WCN399x SLIMbus SCO audio");
MODULE_LICENSE("GPL");
