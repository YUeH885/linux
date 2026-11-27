/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_POWER_H__
#define __IRIS_POWER_H__

struct iris_inst;

int iris_scale_power(struct iris_inst *inst);
void iris_reset_inst_power(struct iris_inst *inst);
void iris_power_mark_busy(struct iris_inst *inst);
void iris_power_mark_idle(struct iris_inst *inst);

#endif
