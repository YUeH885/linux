/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SND_SOC_WCD934X_H__
#define __SND_SOC_WCD934X_H__

#include <linux/types.h>

struct snd_soc_component;

int wcd934x_get_impedance(struct snd_soc_component *component, u32 *left,
			  u32 *right);

#endif
