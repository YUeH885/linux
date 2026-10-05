/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __TFA9872_H
#define __TFA9872_H

#include <linux/types.h>

struct firmware;

struct tfa9872_reg_setting {
	u8 reg;
	u16 mask;
	u16 value;
};

struct tfa9872_config {
	struct tfa9872_reg_setting *regs;
	unsigned int num_regs;
	u8 *messages;
	size_t message_size;
};

int tfa9872_container_parse(const struct firmware *fw, u8 address,
			    unsigned int rate, const char *profile,
			    unsigned int vstep, unsigned int resistance,
			    struct tfa9872_config **config);
void tfa9872_config_free(struct tfa9872_config *config);

#endif
