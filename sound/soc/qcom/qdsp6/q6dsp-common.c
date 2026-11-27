// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2011-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2018, Linaro Limited

#include "q6dsp-common.h"
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <sound/pcm.h>

int q6dsp_map_chmap(u8 ch_map[PCM_MAX_NUM_CHANNEL], unsigned int channels,
		    const unsigned int *map)
{
	static const u8 positions[] = {
		[SNDRV_CHMAP_FL] = PCM_CHANNEL_FL,
		[SNDRV_CHMAP_FR] = PCM_CHANNEL_FR,
		[SNDRV_CHMAP_FC] = PCM_CHANNEL_FC,
		[SNDRV_CHMAP_LFE] = PCM_CHANNEL_LFE,
		[SNDRV_CHMAP_RL] = PCM_CHANNEL_LB,
		[SNDRV_CHMAP_RR] = PCM_CHANNEL_RB,
		[SNDRV_CHMAP_SL] = PCM_CHANNEL_LS,
		[SNDRV_CHMAP_SR] = PCM_CHANNEL_RS,
		[SNDRV_CHMAP_RC] = PCM_CHANNEL_CS,
		[SNDRV_CHMAP_FLC] = PCM_CHANNEL_FLC,
		[SNDRV_CHMAP_FRC] = PCM_CHANNEL_FRC,
		[SNDRV_CHMAP_RLC] = PCM_CHANNEL_RLC,
		[SNDRV_CHMAP_RRC] = PCM_CHANNEL_RRC,
	};
	unsigned int i;

	if (!channels || channels > PCM_MAX_NUM_CHANNEL)
		return -EINVAL;

	memset(ch_map, 0, PCM_MAX_NUM_CHANNEL);
	for (i = 0; i < channels; i++) {
		if (map[i] >= ARRAY_SIZE(positions) || !positions[map[i]])
			return -EINVAL;
		ch_map[i] = positions[map[i]];
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6dsp_map_chmap);

int q6dsp_map_channels(u8 ch_map[PCM_MAX_NUM_CHANNEL], int ch)
{
	memset(ch_map, 0, PCM_MAX_NUM_CHANNEL);

	switch (ch) {
	case 1:
		ch_map[0] = PCM_CHANNEL_FC;
		break;
	case 2:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		break;
	case 3:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		ch_map[2] = PCM_CHANNEL_FC;
		break;
	case 4:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		ch_map[2] = PCM_CHANNEL_LS;
		ch_map[3] = PCM_CHANNEL_RS;
		break;
	case 5:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		ch_map[2] = PCM_CHANNEL_FC;
		ch_map[3] = PCM_CHANNEL_LS;
		ch_map[4] = PCM_CHANNEL_RS;
		break;
	case 6:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		ch_map[2] = PCM_CHANNEL_LFE;
		ch_map[3] = PCM_CHANNEL_FC;
		ch_map[4] = PCM_CHANNEL_LS;
		ch_map[5] = PCM_CHANNEL_RS;
		break;
	case 8:
		ch_map[0] = PCM_CHANNEL_FL;
		ch_map[1] = PCM_CHANNEL_FR;
		ch_map[2] = PCM_CHANNEL_LFE;
		ch_map[3] = PCM_CHANNEL_FC;
		ch_map[4] = PCM_CHANNEL_LS;
		ch_map[5] = PCM_CHANNEL_RS;
		ch_map[6] = PCM_CHANNEL_LB;
		ch_map[7] = PCM_CHANNEL_RB;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(q6dsp_map_channels);

int q6dsp_get_channel_allocation(int channels)
{
	int channel_allocation;

	/* HDMI spec CEA-861-E: Table 28 Audio InfoFrame Data Byte 4 */
	switch (channels) {
	case 2:
		channel_allocation = 0;
		break;
	case 3:
		channel_allocation = 0x02;
		break;
	case 4:
		channel_allocation = 0x06;
		break;
	case 5:
		channel_allocation = 0x0A;
		break;
	case 6:
		channel_allocation = 0x0B;
		break;
	case 7:
		channel_allocation = 0x12;
		break;
	case 8:
		channel_allocation = 0x13;
		break;
	default:
		return -EINVAL;
	}

	return channel_allocation;
}
EXPORT_SYMBOL_GPL(q6dsp_get_channel_allocation);

MODULE_DESCRIPTION("ASoC MSM QDSP6 helper functions");
MODULE_LICENSE("GPL v2");
