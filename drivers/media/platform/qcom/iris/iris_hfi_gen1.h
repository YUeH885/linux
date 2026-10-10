/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __IRIS_HFI_GEN1_H__
#define __IRIS_HFI_GEN1_H__

#include "iris_hfi_gen1_defines.h"
#include "iris_instance.h"

struct iris_core;

#define to_iris_inst_hfi_gen1(ptr) \
	container_of(ptr, struct iris_inst_hfi_gen1, inst)

struct iris_inst_hfi_gen1 {
	struct iris_inst inst;
	struct hfi_buffer_requirements buffer_requirements[HFI_BUFFER_INTERNAL_RECON + 1];
};

u32 iris_hfi_gen1_get_buffer_size(struct iris_inst *inst, enum iris_buffer_type type);
u32 iris_hfi_gen1_get_buffer_count(struct iris_inst *inst, enum iris_buffer_type type);
void iris_hfi_gen1_sys_ops_init(struct iris_core *core);
void iris_hfi_gen1_response_handler(struct iris_core *core);

#endif
