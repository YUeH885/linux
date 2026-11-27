/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __IRIS_SECURE_BUFFER_H__
#define __IRIS_SECURE_BUFFER_H__

struct iris_core;
struct iris_buffer;
struct platform_driver;

extern struct platform_driver iris_cp_driver;

int iris_secure_buffer_init(struct iris_core *core);
int iris_secure_buffer_alloc(struct iris_core *core, struct iris_buffer *buffer);
void iris_secure_buffer_free(struct iris_core *core, struct iris_buffer *buffer);

#endif
