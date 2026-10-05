// SPDX-License-Identifier: GPL-2.0-only
/*
 * Silicon Mitus SMA6101 ALSA SoC audio amplifier driver
 *
 * Copyright (c) 2018 Silicon Mitus Corporation / Iron Device Corporation
 * Copyright (c) 2026 Yuzu <yuzu23234@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#include "sma6101.h"

struct sma6101_pll_match {
	unsigned int input_clk;
	u8 post_n;
	u8 n;
	u8 f1;
	u8 f2;
	u8 f3_p_cp;
};

static const struct sma6101_pll_match sma6101_pll_matches[] = {
	{ 1536000,  0x07, 0xe0, 0x00, 0x00, 0x03 },
	{ 3072000,  0x07, 0x70, 0x00, 0x00, 0x03 },
	{ 6144000,  0x07, 0x70, 0x00, 0x00, 0x07 },
	{ 12288000, 0x07, 0x70, 0x00, 0x00, 0x0b },
	{ 19200000, 0x07, 0x47, 0x8b, 0x00, 0x0a },
	{ 24576000, 0x07, 0x70, 0x00, 0x00, 0x0f },
};

struct sma6101_priv {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_desc *reset_gpio;
	unsigned int sys_clk_id;
	unsigned int init_vol;
	unsigned int rev_num;
	unsigned int format;
	bool stereo_two_chip;
	bool force_power_down;
	bool lr_swap;
	bool mono_mix;
	bool amp_power_status;
	u32 *eq_regs;
	int num_eq_regs;
	u32 *bo_regs;
	int num_bo_regs;
};

static const struct regmap_config sma6101_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = SMA6101_FF_VERSION,
	.cache_type = REGCACHE_NONE,
};

static const DECLARE_TLV_DB_SCALE(sma6101_spk_tlv, -6000, 50, 0);

static int sma6101_startup(struct snd_soc_component *component);
static int sma6101_shutdown(struct snd_soc_component *component);

static const char * const sma6101_spkmode_text[] = {
	"Mono",
	"Stereo",
};

static const struct soc_enum sma6101_spkmode_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sma6101_spkmode_text), sma6101_spkmode_text);

static int sma6101_spkmode_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	ucontrol->value.enumerated.item[0] = sma6101->stereo_two_chip ? 1 : 0;
	return 0;
}

static int sma6101_spkmode_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);
	bool stereo = (ucontrol->value.enumerated.item[0] == 1);

	if (sma6101->stereo_two_chip == stereo)
		return 0;

	sma6101->stereo_two_chip = stereo;
	if (sma6101->amp_power_status)
		regmap_update_bits(sma6101->regmap, SMA6101_10_SYSTEM_CTRL1,
				   SMA6101_SPK_MODE_MASK,
				   stereo ? SMA6101_SPK_STEREO : SMA6101_SPK_MONO);
	return 1;
}

static int sma6101_enable_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = !sma6101->force_power_down;
	return 0;
}

static int sma6101_enable_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);
	bool enable = !!ucontrol->value.integer.value[0];

	if (!enable == sma6101->force_power_down)
		return 0;

	sma6101->force_power_down = !enable;
	if (sma6101->force_power_down)
		sma6101_shutdown(component);
	else
		sma6101_startup(component);
	return 1;
}

static int sma6101_lr_swap_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = sma6101->lr_swap;
	return 0;
}

static int sma6101_lr_swap_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);
	bool val = !!ucontrol->value.integer.value[0];

	if (sma6101->lr_swap == val)
		return 0;

	sma6101->lr_swap = val;
	regmap_update_bits(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2,
			   BIT(4), val ? BIT(4) : 0);
	return 1;
}

static int sma6101_mono_mix_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = sma6101->mono_mix;
	return 0;
}

static int sma6101_mono_mix_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);
	bool val = !!ucontrol->value.integer.value[0];

	if (sma6101->mono_mix == val)
		return 0;

	sma6101->mono_mix = val;
	regmap_update_bits(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2,
			   BIT(0), val ? BIT(0) : 0);
	return 1;
}

static const struct snd_kcontrol_new sma6101_controls[] = {
	SOC_SINGLE_TLV("SMA6101 Speaker Volume", SMA6101_0A_SPK_VOL,
		       0, 0xa8, 0, sma6101_spk_tlv),
	SOC_SINGLE_BOOL_EXT("SMA6101 Switch", 0,
			    sma6101_enable_get, sma6101_enable_put),
	SOC_SINGLE_BOOL_EXT("SMA6101 LR Swap Switch", 0,
			    sma6101_lr_swap_get, sma6101_lr_swap_put),
	SOC_SINGLE_BOOL_EXT("SMA6101 Mono Mix Switch", 0,
			    sma6101_mono_mix_get, sma6101_mono_mix_put),
	SOC_ENUM_EXT("SMA6101 Speaker Mode", sma6101_spkmode_enum,
		     sma6101_spkmode_get, sma6101_spkmode_put),
};

static int sma6101_setup_pll(struct snd_soc_component *component,
			     unsigned int bclk_rate)
{
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);
	int i;

	regmap_update_bits(sma6101->regmap, SMA6101_AC_PLL_MODE_CTRL,
			   SMA6101_PLL_LDO_BYP_MASK,
			   SMA6101_PLL_LDO_BYP_ENABLE);

	if (sma6101->sys_clk_id == SMA6101_PLL_CLKIN_BCLK) {
		regmap_update_bits(sma6101->regmap, SMA6101_A7_TOP_MAN3,
				   SMA6101_CLOCK_MON_SEL_MASK,
				   SMA6101_CLOCK_MON_SCK);

		regmap_update_bits(sma6101->regmap, SMA6101_A2_TOP_MAN1,
				   SMA6101_PLL_PD_MASK |
				   SMA6101_MCLK_SEL_MASK |
				   SMA6101_PLL_REF_CLK1_MASK |
				   SMA6101_PLL_REF_CLK2_MASK,
				   SMA6101_PLL_OPERATION |
				   SMA6101_PLL_CLK |
				   SMA6101_REF_EXTERNAL_CLK |
				   SMA6101_PLL_SCK);

		for (i = 0; i < ARRAY_SIZE(sma6101_pll_matches); i++) {
			if (sma6101_pll_matches[i].input_clk == bclk_rate)
				break;
		}

		if (i >= ARRAY_SIZE(sma6101_pll_matches)) {
			dev_err(component->dev, "unsupported BCLK rate: %u\n",
				bclk_rate);
			return -EINVAL;
		}

		regmap_write(sma6101->regmap, SMA6101_8B_PLL_POST_N,
			     sma6101_pll_matches[i].post_n);
		regmap_write(sma6101->regmap, SMA6101_8C_PLL_N,
			     sma6101_pll_matches[i].n);
		regmap_write(sma6101->regmap, SMA6101_8D_PLL_F1,
			     sma6101_pll_matches[i].f1);
		regmap_write(sma6101->regmap, SMA6101_8E_PLL_F2,
			     sma6101_pll_matches[i].f2);
		regmap_write(sma6101->regmap, SMA6101_8F_PLL_F3,
			     sma6101_pll_matches[i].f3_p_cp);
	}

	return 0;
}

static int sma6101_startup(struct snd_soc_component *component)
{
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	if (sma6101->amp_power_status || sma6101->force_power_down)
		return 0;

	if (sma6101->rev_num == SMA6101_REV_NUM_REV3)
		regmap_write(sma6101->regmap, SMA6101_93_BOOST_CTRL0, 0xf0);

	regmap_update_bits(sma6101->regmap, SMA6101_0E_MUTE_VOL_CTRL,
			   SMA6101_SPK_MUTE_MASK, SMA6101_SPK_MUTE);
	regmap_update_bits(sma6101->regmap, SMA6101_10_SYSTEM_CTRL1,
			   SMA6101_SPK_MODE_MASK, SMA6101_SPK_OFF);

	regmap_update_bits(sma6101->regmap, SMA6101_00_SYSTEM_CTRL,
			   SMA6101_POWER_MASK, SMA6101_POWER_ON);

	msleep(20);

	if (sma6101->rev_num == SMA6101_REV_NUM_REV3) {
		regmap_update_bits(sma6101->regmap, SMA6101_13_FDPEC_CTRL1,
				   0x03, 0x02);
		regmap_update_bits(sma6101->regmap, SMA6101_93_BOOST_CTRL0,
				   0x1f, 0x18);
		usleep_range(1000, 1050);
		regmap_update_bits(sma6101->regmap, SMA6101_93_BOOST_CTRL0,
				   0xe0, 0x00);
	} else if (sma6101->rev_num == SMA6101_REV_NUM_REV4) {
		regmap_update_bits(sma6101->regmap, SMA6101_90_CLASS_H_CTRL_LVL1,
				   0x03, 0x02);
		regmap_update_bits(sma6101->regmap, SMA6101_91_CLASS_H_CTRL_LVL2,
				   0x03, 0x01);
		regmap_update_bits(sma6101->regmap, SMA6101_0D_CLASS_H_CTRL_LVL3,
				   0x03, 0x01);
		regmap_update_bits(sma6101->regmap, SMA6101_0F_CLASS_H_CTRL_LVL4,
				   0x03, 0x00);
		regmap_update_bits(sma6101->regmap, SMA6101_93_BOOST_CTRL0,
				   0x1f, 0x1a);
		usleep_range(1000, 1050);
	}

	if (sma6101->stereo_two_chip)
		regmap_update_bits(sma6101->regmap, SMA6101_10_SYSTEM_CTRL1,
				   SMA6101_SPK_MODE_MASK, SMA6101_SPK_STEREO);
	else
		regmap_update_bits(sma6101->regmap, SMA6101_10_SYSTEM_CTRL1,
				   SMA6101_SPK_MODE_MASK, SMA6101_SPK_MONO);

	regmap_update_bits(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2,
			   BIT(4) | BIT(0),
			   (sma6101->lr_swap ? BIT(4) : 0) |
			   (sma6101->mono_mix ? BIT(0) : 0));

	regmap_update_bits(sma6101->regmap, SMA6101_A8_TONE_GENERATOR,
			   SMA6101_TONE_ON_MASK, SMA6101_TONE_ON);
	regmap_update_bits(sma6101->regmap, SMA6101_13_FDPEC_CTRL1,
			   BIT(4), 0);

	sma6101->amp_power_status = true;

	regmap_update_bits(sma6101->regmap, SMA6101_0E_MUTE_VOL_CTRL,
			   SMA6101_SPK_MUTE_MASK, SMA6101_SPK_UNMUTE);

	return 0;
}

static int sma6101_shutdown(struct snd_soc_component *component)
{
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	if (!sma6101->amp_power_status)
		return 0;

	regmap_update_bits(sma6101->regmap, SMA6101_0E_MUTE_VOL_CTRL,
			   SMA6101_SPK_MUTE_MASK, SMA6101_SPK_MUTE);

	usleep_range(15000, 15500);

	if (sma6101->rev_num == SMA6101_REV_NUM_REV4) {
		regmap_update_bits(sma6101->regmap, SMA6101_92_FDPEC_CTRL2,
				   0x03, 0x00);
		regmap_update_bits(sma6101->regmap, SMA6101_90_CLASS_H_CTRL_LVL1,
				   0x03, 0x00);
		regmap_update_bits(sma6101->regmap, SMA6101_91_CLASS_H_CTRL_LVL2,
				   0x03, 0x00);
		regmap_update_bits(sma6101->regmap, SMA6101_0D_CLASS_H_CTRL_LVL3,
				   0x03, 0x00);
		regmap_update_bits(sma6101->regmap, SMA6101_0F_CLASS_H_CTRL_LVL4,
				   0x03, 0x00);
	}

	regmap_update_bits(sma6101->regmap, SMA6101_10_SYSTEM_CTRL1,
			   SMA6101_SPK_MODE_MASK, SMA6101_SPK_OFF);

	regmap_update_bits(sma6101->regmap, SMA6101_00_SYSTEM_CTRL,
			   SMA6101_POWER_MASK, SMA6101_POWER_OFF);

	regmap_update_bits(sma6101->regmap, SMA6101_A8_TONE_GENERATOR,
			   SMA6101_TONE_ON_MASK, SMA6101_TONE_OFF);

	sma6101->amp_power_status = false;

	return 0;
}

static int sma6101_dac_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return sma6101_startup(component);
	case SND_SOC_DAPM_POST_PMU:
		regmap_update_bits(sma6101->regmap, SMA6101_AE_TOP_MAN4,
				   SMA6101_DIS_IRQ_MASK,
				   SMA6101_NORMAL_OPERATION_IRQ);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		sma6101_shutdown(component);
		regmap_update_bits(sma6101->regmap, SMA6101_AE_TOP_MAN4,
				   SMA6101_DIS_IRQ_MASK,
				   SMA6101_HIGH_Z_IRQ);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(sma6101->regmap, SMA6101_AC_PLL_MODE_CTRL,
				   SMA6101_PLL_LDO_BYP_MASK,
				   SMA6101_PLL_LDO_BYP_DISABLE);
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget sma6101_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN_E("SMA6101 AIF", "Playback", 0, SND_SOC_NOPM, 0, 0,
			      sma6101_dac_event,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU |
			      SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("SMA6101 SPK"),
};

static const struct snd_soc_dapm_route sma6101_dapm_routes[] = {
	{ "SMA6101 SPK", NULL, "SMA6101 AIF" },
};

static int sma6101_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_component *component = dai->component;
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBC_CFC:
		regmap_update_bits(sma6101->regmap, SMA6101_01_INPUT1_CTRL1,
				   SMA6101_MASTER_SLAVE_MASK,
				   SMA6101_SLAVE_MODE);
		regmap_update_bits(sma6101->regmap, SMA6101_A7_TOP_MAN3,
				   SMA6101_MAS_EN_MASK,
				   SMA6101_MAS_EN_SLAVE);
		break;
	case SND_SOC_DAIFMT_CBP_CFP:
		regmap_update_bits(sma6101->regmap, SMA6101_01_INPUT1_CTRL1,
				   SMA6101_MASTER_SLAVE_MASK,
				   SMA6101_MASTER_MODE);
		regmap_update_bits(sma6101->regmap, SMA6101_A7_TOP_MAN3,
				   SMA6101_MAS_EN_MASK,
				   SMA6101_MAS_EN_MASTER);
		break;
	default:
		dev_err(component->dev, "unsupported provider mode: 0x%x\n", fmt);
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		sma6101->format = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
		break;
	default:
		dev_err(component->dev, "unsupported format: 0x%x\n", fmt);
		return -EINVAL;
	}

	return 0;
}

static int sma6101_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);
	unsigned int bclk_rate;
	u8 input_format = 0;
	int ret;

	bclk_rate = params_rate(params) * params_physical_width(params) *
		    params_channels(params);

	if (sma6101->sys_clk_id == SMA6101_PLL_CLKIN_BCLK) {
		regmap_update_bits(sma6101->regmap, SMA6101_03_INPUT1_CTRL3,
				   SMA6101_BP_SRC_MASK,
				   SMA6101_BP_SRC_NORMAL);

		ret = sma6101_setup_pll(component, bclk_rate);
		if (ret)
			return ret;
	}

	switch (params_rate(params)) {
	case 48000:
	case 96000:
		regmap_update_bits(sma6101->regmap, SMA6101_A2_TOP_MAN1,
				   SMA6101_PLL_LOCK_SKIP_MASK |
				   SMA6101_DAC_DN_CONV_MASK,
				   SMA6101_PLL_LOCK_ENABLE |
				   SMA6101_DAC_DN_CONV_DISABLE);
		regmap_update_bits(sma6101->regmap, SMA6101_01_INPUT1_CTRL1,
				   SMA6101_LEFTPOL_MASK,
				   SMA6101_LOW_FIRST_CH);
		break;
	case 192000:
		regmap_update_bits(sma6101->regmap, SMA6101_A2_TOP_MAN1,
				   SMA6101_PLL_LOCK_SKIP_MASK |
				   SMA6101_DAC_DN_CONV_MASK,
				   SMA6101_PLL_LOCK_DISABLE |
				   SMA6101_DAC_DN_CONV_ENABLE);
		regmap_update_bits(sma6101->regmap, SMA6101_01_INPUT1_CTRL1,
				   SMA6101_LEFTPOL_MASK,
				   SMA6101_HIGH_FIRST_CH);
		break;
	default:
		dev_err(component->dev, "unsupported sample rate: %u\n",
			params_rate(params));
		return -EINVAL;
	}

	switch (params_width(params)) {
	case 16:
		switch (sma6101->format) {
		case SND_SOC_DAIFMT_I2S:
			input_format = SMA6101_STANDARD_I2S;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			input_format = SMA6101_LJ;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			input_format = SMA6101_RJ_16BIT;
			break;
		}
		break;
	case 24:
	case 32:
		switch (sma6101->format) {
		case SND_SOC_DAIFMT_I2S:
			input_format = SMA6101_STANDARD_I2S;
			break;
		case SND_SOC_DAIFMT_LEFT_J:
			input_format = SMA6101_LJ;
			break;
		case SND_SOC_DAIFMT_RIGHT_J:
			input_format = SMA6101_RJ_24BIT;
			break;
		}
		break;
	default:
		dev_err(component->dev, "unsupported data width: %u\n",
			params_width(params));
		return -EINVAL;
	}

	regmap_update_bits(sma6101->regmap, SMA6101_01_INPUT1_CTRL1,
			   SMA6101_I2S_MODE_MASK, input_format);

	return 0;
}

static int sma6101_prepare(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	if (sma6101->force_power_down)
		return 0;

	return sma6101_startup(component);
}

static int sma6101_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct snd_soc_component *component = dai->component;
	struct sma6101_priv *sma6101 = snd_soc_component_get_drvdata(component);

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;

	if (mute || sma6101->force_power_down) {
		regmap_update_bits(sma6101->regmap, SMA6101_0E_MUTE_VOL_CTRL,
				   SMA6101_SPK_MUTE_MASK, SMA6101_SPK_MUTE);
		return sma6101_shutdown(component);
	}

	sma6101_startup(component);
	regmap_update_bits(sma6101->regmap, SMA6101_0E_MUTE_VOL_CTRL,
			   SMA6101_SPK_MUTE_MASK, SMA6101_SPK_UNMUTE);

	return 0;
}

static const struct snd_soc_dai_ops sma6101_dai_ops = {
	.set_fmt = sma6101_set_fmt,
	.hw_params = sma6101_hw_params,
	.prepare = sma6101_prepare,
	.mute_stream = sma6101_mute_stream,
};

static struct snd_soc_dai_driver sma6101_dai = {
	.name = "sma6101-piezo",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000 |
			 SNDRV_PCM_RATE_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &sma6101_dai_ops,
};

static const struct snd_soc_component_driver sma6101_component = {
	.controls = sma6101_controls,
	.num_controls = ARRAY_SIZE(sma6101_controls),
	.dapm_widgets = sma6101_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sma6101_dapm_widgets),
	.dapm_routes = sma6101_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(sma6101_dapm_routes),
	.use_pmdown_time = 1,
};

static int sma6101_reset(struct sma6101_priv *sma6101)
{
	unsigned int status;
	int ret, i;

	ret = regmap_read(sma6101->regmap, SMA6101_FA_STATUS1, &status);
	if (ret) {
		dev_err(sma6101->dev, "failed to read revision status: %d\n", ret);
		return ret;
	}

	sma6101->rev_num = status & SMA6101_REV_NUM_STATUS;

	regmap_write(sma6101->regmap, SMA6101_93_BOOST_CTRL0, 0xf0);
	regmap_write(sma6101->regmap, SMA6101_0F_CLASS_H_CTRL_LVL4, 0xf3);
	regmap_write(sma6101->regmap, SMA6101_00_SYSTEM_CTRL, 0x80);
	regmap_write(sma6101->regmap, SMA6101_0A_SPK_VOL, sma6101->init_vol);
	regmap_write(sma6101->regmap, SMA6101_0D_CLASS_H_CTRL_LVL3, 0xf3);
	regmap_write(sma6101->regmap, SMA6101_0E_MUTE_VOL_CTRL, 0xff);

	if (sma6101->stereo_two_chip)
		regmap_update_bits(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2,
				   SMA6101_MONOMIX_MASK, SMA6101_MONOMIX_OFF);
	else
		regmap_update_bits(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2,
				   SMA6101_MONOMIX_MASK, SMA6101_MONOMIX_ON);

	regmap_write(sma6101->regmap, SMA6101_14_MODULATOR, 0x11);
	regmap_write(sma6101->regmap, SMA6101_37_SLOPE_CTRL, 0x10);

	if (sma6101->rev_num == SMA6101_REV_NUM_REV0)
		regmap_write(sma6101->regmap, SMA6101_90_CLASS_H_CTRL_LVL1, 0x02);
	else
		regmap_write(sma6101->regmap, SMA6101_90_CLASS_H_CTRL_LVL1, 0xf2);

	regmap_write(sma6101->regmap, SMA6101_91_CLASS_H_CTRL_LVL2, 0xf3);
	regmap_write(sma6101->regmap, SMA6101_92_FDPEC_CTRL2, 0x03);
	regmap_write(sma6101->regmap, SMA6101_94_BOOST_CTRL1, 0x9b);

	if (sma6101->rev_num == SMA6101_REV_NUM_REV3) {
		regmap_write(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2, 0xa0);
		regmap_write(sma6101->regmap, SMA6101_13_FDPEC_CTRL1, 0x3c);
		regmap_write(sma6101->regmap, SMA6101_14_MODULATOR, 0x59);
		regmap_write(sma6101->regmap, SMA6101_23_COMP_LIM1, 0x1f);
		regmap_write(sma6101->regmap, SMA6101_26_COMP_LIM4, 0xff);
		regmap_write(sma6101->regmap, SMA6101_37_SLOPE_CTRL, 0x05);
		regmap_write(sma6101->regmap, SMA6101_3B_TEST1, 0x5a);
		regmap_update_bits(sma6101->regmap, SMA6101_3F_ATEST2, 0x03, 0x03);
		regmap_write(sma6101->regmap, SMA6101_0D_CLASS_H_CTRL_LVL3, 0xc5);
		regmap_write(sma6101->regmap, SMA6101_0F_CLASS_H_CTRL_LVL4, 0x94);
		regmap_write(sma6101->regmap, SMA6101_90_CLASS_H_CTRL_LVL1, 0xe7);
		regmap_write(sma6101->regmap, SMA6101_91_CLASS_H_CTRL_LVL2, 0x76);
		regmap_write(sma6101->regmap, SMA6101_92_FDPEC_CTRL2, 0x89);
		regmap_write(sma6101->regmap, SMA6101_95_BOOST_CTRL2, 0x44);
		regmap_write(sma6101->regmap, SMA6101_A9_TONE_FINE_VOL, 0xaf);
		regmap_write(sma6101->regmap, SMA6101_AD_SPK_OCP_LVL, 0x0a);
		regmap_write(sma6101->regmap, SMA6101_AF_VIN_SENSING, 0x01);
	} else if (sma6101->rev_num == SMA6101_REV_NUM_REV4) {
		regmap_write(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2, 0xa0);
		regmap_write(sma6101->regmap, SMA6101_13_FDPEC_CTRL1, 0x3c);
		regmap_write(sma6101->regmap, SMA6101_14_MODULATOR, 0x61);
		regmap_write(sma6101->regmap, SMA6101_23_COMP_LIM1, 0x1f);
		regmap_write(sma6101->regmap, SMA6101_24_COMP_LIM2, 0x02);
		regmap_write(sma6101->regmap, SMA6101_25_COMP_LIM3, 0x09);
		regmap_write(sma6101->regmap, SMA6101_26_COMP_LIM4, 0xff);
		regmap_write(sma6101->regmap, SMA6101_37_SLOPE_CTRL, 0x05);
		regmap_write(sma6101->regmap, SMA6101_0D_CLASS_H_CTRL_LVL3, 0xb4);
		regmap_write(sma6101->regmap, SMA6101_0F_CLASS_H_CTRL_LVL4, 0x83);
		regmap_write(sma6101->regmap, SMA6101_90_CLASS_H_CTRL_LVL1, 0xe6);
		regmap_write(sma6101->regmap, SMA6101_91_CLASS_H_CTRL_LVL2, 0x75);
		regmap_write(sma6101->regmap, SMA6101_92_FDPEC_CTRL2, 0x09);
		regmap_update_bits(sma6101->regmap, SMA6101_93_BOOST_CTRL0, 0x0f, 0x08);
		regmap_write(sma6101->regmap, SMA6101_95_BOOST_CTRL2, 0x44);
		regmap_write(sma6101->regmap, SMA6101_97_BOOST_CTRL4, 0xe4);
		regmap_update_bits(sma6101->regmap, SMA6101_A9_TONE_FINE_VOL, 0x3f, 0x00);
		regmap_update_bits(sma6101->regmap, SMA6101_A8_TONE_GENERATOR, 0x01, 0x00);
		regmap_write(sma6101->regmap, SMA6101_A9_TONE_FINE_VOL, 0x8f);
		regmap_write(sma6101->regmap, SMA6101_AD_SPK_OCP_LVL, 0x06);
		regmap_write(sma6101->regmap, SMA6101_AF_VIN_SENSING, 0x01);
	}

	/* Keep SDO High-Z so SEC MI2S SD0 is free for TFA9872 feedback. */
	regmap_update_bits(sma6101->regmap, SMA6101_A3_TOP_MAN2,
			   SMA6101_SDO_OUTPUT_MASK, SMA6101_HIGH_Z_OUT);
	regmap_update_bits(sma6101->regmap, SMA6101_09_OUTPUT_CTRL,
			   SMA6101_PORT_OUT_SEL_MASK, SMA6101_PORT_OUT_DISABLE);

	if (sma6101->eq_regs) {
		for (i = 0; i < sma6101->num_eq_regs; i += 2)
			regmap_write(sma6101->regmap, sma6101->eq_regs[i],
				     sma6101->eq_regs[i + 1]);
	}

	if (sma6101->bo_regs) {
		for (i = 0; i < sma6101->num_bo_regs; i += 2)
			regmap_write(sma6101->regmap, sma6101->bo_regs[i],
				     sma6101->bo_regs[i + 1]);
	}

	regmap_update_bits(sma6101->regmap, SMA6101_11_SYSTEM_CTRL2,
			   BIT(4) | BIT(0),
			   (sma6101->lr_swap ? BIT(4) : 0) |
			   (sma6101->mono_mix ? BIT(0) : 0));

	return 0;
}

static int sma6101_parse_dt(struct sma6101_priv *sma6101)
{
	struct device *dev = sma6101->dev;
	struct device_node *np = dev->of_node;
	int count;

	if (!np)
		return -ENODEV;

	sma6101->init_vol = 0x30;
	of_property_read_u32(np, "init-vol", &sma6101->init_vol);

	sma6101->sys_clk_id = SMA6101_PLL_CLKIN_BCLK;
	of_property_read_u32(np, "sys-clk-id", &sma6101->sys_clk_id);

	sma6101->stereo_two_chip = of_property_read_bool(np, "stereo-two-chip");
	sma6101->lr_swap = false;
	sma6101->mono_mix = false;
	sma6101->force_power_down = false;

	count = of_property_count_u32_elems(np, "registers-of-eq");
	if (count > 0 && !(count % 2)) {
		sma6101->eq_regs = devm_kcalloc(dev, count, sizeof(u32), GFP_KERNEL);
		if (!sma6101->eq_regs)
			return -ENOMEM;
		of_property_read_u32_array(np, "registers-of-eq",
					   sma6101->eq_regs, count);
		sma6101->num_eq_regs = count;
	}

	count = of_property_count_u32_elems(np, "registers-of-bo");
	if (count > 0 && !(count % 2)) {
		sma6101->bo_regs = devm_kcalloc(dev, count, sizeof(u32), GFP_KERNEL);
		if (!sma6101->bo_regs)
			return -ENOMEM;
		of_property_read_u32_array(np, "registers-of-bo",
					   sma6101->bo_regs, count);
		sma6101->num_bo_regs = count;
	}

	return 0;
}

static int sma6101_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sma6101_priv *sma6101;
	unsigned int val;
	int ret;

	sma6101 = devm_kzalloc(dev, sizeof(*sma6101), GFP_KERNEL);
	if (!sma6101)
		return -ENOMEM;

	sma6101->dev = dev;
	i2c_set_clientdata(client, sma6101);

	ret = sma6101_parse_dt(sma6101);
	if (ret)
		return ret;

	sma6101->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(sma6101->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sma6101->reset_gpio),
				      "failed to get reset GPIO\n");

	if (!sma6101->reset_gpio) {
		sma6101->reset_gpio = devm_fwnode_gpiod_get_optional(dev,
								     dev_fwnode(dev),
								     "sm,reset",
								     GPIOD_OUT_HIGH,
								     "sma6101");
		if (IS_ERR(sma6101->reset_gpio))
			return dev_err_probe(dev, PTR_ERR(sma6101->reset_gpio),
					      "failed to get sm,reset GPIO\n");
	}

	if (sma6101->reset_gpio) {
		gpiod_set_value_cansleep(sma6101->reset_gpio, 0);
		fsleep(5000);
		gpiod_set_value_cansleep(sma6101->reset_gpio, 1);
		fsleep(5000);
	}

	sma6101->regmap = devm_regmap_init_i2c(client, &sma6101_regmap_config);
	if (IS_ERR(sma6101->regmap))
		return dev_err_probe(dev, PTR_ERR(sma6101->regmap),
				      "failed to initialize register map\n");

	ret = regmap_read(sma6101->regmap, SMA6101_FF_VERSION, &val);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read version register\n");

	if (val != SMA6101_CHIP_VERSION) {
		dev_err(dev, "unsupported chip version: 0x%02x\n", val);
		return -ENODEV;
	}

	ret = sma6101_reset(sma6101);
	if (ret)
		return ret;

	return devm_snd_soc_register_component(dev, &sma6101_component,
					       &sma6101_dai, 1);
}

static const struct i2c_device_id sma6101_i2c_id[] = {
	{ "sma6101", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sma6101_i2c_id);

static const struct of_device_id sma6101_of_match[] = {
	{ .compatible = "siliconmitus,sma6101" },
	{ }
};
MODULE_DEVICE_TABLE(of, sma6101_of_match);

static struct i2c_driver sma6101_i2c_driver = {
	.driver = {
		.name = "sma6101",
		.of_match_table = sma6101_of_match,
	},
	.probe = sma6101_i2c_probe,
	.id_table = sma6101_i2c_id,
};
module_i2c_driver(sma6101_i2c_driver);

MODULE_DESCRIPTION("Silicon Mitus SMA6101 audio amplifier driver");
MODULE_AUTHOR("Yuzu <yuzu23234@gmail.com>");
MODULE_LICENSE("GPL");
