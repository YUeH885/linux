// SPDX-License-Identifier: GPL-2.0-only
/*
 * TFA container format and TFADSP commands described by NXP's
 * tfa98xx_parameters.h, tfa_container.c and tfa_dsp.c (2014-2019).
 */

#include <linux/crc32.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "tfa9872.h"

#define TFA_HEADER_SIZE		36
#define TFA_CONTAINER_HEADER_SIZE	46
#define TFA_DESC_PROFILE		1
#define TFA_DESC_REGISTER		2
#define TFA_DESC_STRING		3
#define TFA_DESC_FILE		4
#define TFA_DESC_PATCH		5
#define TFA_DESC_BITFIELD		16
#define TFA_DESC_DEFAULT		17
#define TFA_DESC_COMMAND		21
#define TFA_FILE_MESSAGE		0x474d
#define TFA_FILE_SPEAKER		0x5053
#define TFA_FILE_VSTEP		0x5056
#define TFA_SPEAKER_HEADER_SIZE	76

struct tfa9872_parser {
	const struct firmware *fw;
	struct tfa9872_config *config;
	unsigned int max_regs;
	unsigned int vstep;
	bool has_vstep;
};

static bool tfa9872_range(const struct firmware *fw, u32 offset, size_t size)
{
	return offset <= fw->size && size <= fw->size - offset;
}

static int tfa9872_append_message(struct tfa9872_config *config,
				  const u8 *raw, size_t length)
{
	u8 *dst = config->messages + config->message_size;
	size_t converted, i;

	if (!length || length % 3)
		return -EINVAL;
	converted = length / 3 * 4;
	if (converted > U16_MAX || converted + 2 > SZ_64K - config->message_size)
		return -E2BIG;

	put_unaligned_be16(converted, dst);
	for (i = 0; i < length; i += 3) {
		s32 value = (raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2];

		put_unaligned_le32(sign_extend32(value, 23), dst + 2 + i / 3 * 4);
	}
	config->message_size += converted + 2;
	return 0;
}

static int tfa9872_append_register(struct tfa9872_parser *parser, u8 reg,
				   u16 mask, u16 value)
{
	struct tfa9872_config *config = parser->config;
	struct tfa9872_reg_setting *setting;

	if (config->num_regs == parser->max_regs)
		return -E2BIG;
	setting = &config->regs[config->num_regs++];
	setting->reg = reg;
	setting->mask = mask;
	setting->value = value & mask;
	return 0;
}

static int tfa9872_append_bitfield(struct tfa9872_parser *parser,
				   u16 field, u16 value)
{
	unsigned int shift = (field >> 4) & 0xf;
	unsigned int width = (field & 0xf) + 1;
	u16 mask;

	if (shift + width > 16 || value >= BIT(width))
		return -EINVAL;
	mask = GENMASK(shift + width - 1, shift);
	return tfa9872_append_register(parser, field >> 8, mask, value << shift);
}

static int tfa9872_parse_vstep(struct tfa9872_parser *parser,
			       const u8 *file, size_t size)
{
	unsigned int steps, step, regs, messages, i, type;
	size_t pos = TFA_HEADER_SIZE + 4, length;
	int ret;

	if (size < pos || file[2] != '3' || file[4] != '0' || file[5] != '1')
		return -EOPNOTSUPP;
	steps = file[TFA_HEADER_SIZE + 3];
	if (parser->vstep >= steps || parser->has_vstep)
		return -EINVAL;

	for (step = 0; step < steps; step++) {
		if (pos >= size)
			return -EINVAL;
		regs = file[pos++];
		if (regs * 4 + 1 > size - pos)
			return -EINVAL;
		if (step == parser->vstep) {
			for (i = 0; i < regs; i++) {
				u16 field = get_unaligned_be16(file + pos + i * 4);
				u16 value = get_unaligned_le16(file + pos + i * 4 + 2) >> 8;

				ret = tfa9872_append_bitfield(parser, field, value);
				if (ret)
					return ret;
			}
		}
		pos += regs * 4;
		messages = file[pos++];
		for (i = 0; i < messages; i++) {
			if (size - pos < 4)
				return -EINVAL;
			type = file[pos];
			length = get_unaligned_be24(file + pos + 1);
			pos += 4;
			if (type != 3)
				length *= 3;
			if (length > size - pos || type > 3)
				return -EINVAL;
			if (step == parser->vstep && type != 3) {
				size_t start = parser->config->message_size;

				ret = tfa9872_append_message(parser->config, file + pos, length);
				if (ret)
					return ret;
				/* Each new AFE instance needs the cold-start command IDs. */
				if (type == 0 || type == 2)
					parser->config->messages[start + 2] = type == 0 ? 0 : 7;
			}
			pos += length;
		}
	}
	if (pos != size)
		return -EINVAL;
	parser->has_vstep = true;
	return 0;
}

static int tfa9872_parse_file(struct tfa9872_parser *parser, u32 offset)
{
	const struct firmware *fw = parser->fw;
	const u8 *file;
	u32 size;
	u16 length;

	if (!tfa9872_range(fw, offset, 8 + TFA_HEADER_SIZE))
		return -EINVAL;
	size = get_unaligned_le32(fw->data + offset + 4);
	file = fw->data + offset + 8;
	length = get_unaligned_le16(file + 6);
	if (length < TFA_HEADER_SIZE || length != size ||
	    !tfa9872_range(fw, offset + 8, size))
		return -EINVAL;
	if (~crc32_le(~0U, file + 12, length - 12) != get_unaligned_le32(file + 8))
		return -EBADMSG;

	switch (get_unaligned_le16(file)) {
	case TFA_FILE_MESSAGE:
		return tfa9872_append_message(parser->config, file + TFA_HEADER_SIZE,
					      length - TFA_HEADER_SIZE);
	case TFA_FILE_SPEAKER:
		if (length <= TFA_SPEAKER_HEADER_SIZE)
			return -EINVAL;
		return tfa9872_append_message(parser->config, file + TFA_SPEAKER_HEADER_SIZE,
					      length - TFA_SPEAKER_HEADER_SIZE);
	case TFA_FILE_VSTEP:
		return tfa9872_parse_vstep(parser, file, length);
	default:
		return -EOPNOTSUPP;
	}
}

static int tfa9872_parse_list(struct tfa9872_parser *parser, u32 offset,
			      unsigned int count, bool device)
{
	const struct firmware *fw = parser->fw;
	u32 entry, type, pos;
	unsigned int i;
	u16 length;
	int ret;

	if (!tfa9872_range(fw, offset, count * 4))
		return -EINVAL;
	for (i = 0; i < count; i++) {
		entry = get_unaligned_le32(fw->data + offset + i * 4);
		type = entry >> 24;
		pos = entry & GENMASK(23, 0);
		switch (type) {
		case TFA_DESC_BITFIELD:
			if (!tfa9872_range(fw, pos, 4))
				return -EINVAL;
			ret = tfa9872_append_bitfield(parser,
						      get_unaligned_le16(fw->data + pos + 2),
						      get_unaligned_le16(fw->data + pos));
			break;
		case TFA_DESC_REGISTER:
			if (!tfa9872_range(fw, pos, 5))
				return -EINVAL;
			ret = tfa9872_append_register(parser, fw->data[pos],
						      get_unaligned_le16(fw->data + pos + 3),
						      get_unaligned_le16(fw->data + pos + 1));
			break;
		case TFA_DESC_COMMAND:
			if (!tfa9872_range(fw, pos, 2))
				return -EINVAL;
			length = get_unaligned_le16(fw->data + pos);
			if (!tfa9872_range(fw, pos + 2, length))
				return -EINVAL;
			ret = tfa9872_append_message(parser->config, fw->data + pos + 2, length);
			break;
		case TFA_DESC_FILE:
			ret = tfa9872_parse_file(parser, pos);
			break;
		case TFA_DESC_DEFAULT:
			return 0;
		case TFA_DESC_PROFILE:
		case TFA_DESC_PATCH:
		case 18: /* Device live-data descriptions. */
			if (!device)
				return -EOPNOTSUPP;
			continue;
		default:
			return -EOPNOTSUPP;
		}
		if (ret)
			return ret;
	}
	return 0;
}

static int tfa9872_find_lists(const struct firmware *fw, u8 address,
			      const char *profile, u32 *dev, u32 *prof)
{
	u32 entry, offset, name;
	unsigned int devices, i, j;

	devices = get_unaligned_le16(fw->data + 40);
	if (!devices || !tfa9872_range(fw, TFA_CONTAINER_HEADER_SIZE, devices * 4))
		return -EINVAL;
	for (i = 0; i < devices; i++) {
		entry = get_unaligned_le32(fw->data + TFA_CONTAINER_HEADER_SIZE + i * 4);
		offset = entry & GENMASK(23, 0);
		if (entry >> 24 || !tfa9872_range(fw, offset, 12))
			return -EINVAL;
		if (fw->data[offset + 2] != address)
			continue;
		*dev = offset;
		if (!tfa9872_range(fw, offset + 12, fw->data[offset] * 4))
			return -EINVAL;
		for (j = 0; j < fw->data[offset]; j++) {
			entry = get_unaligned_le32(fw->data + offset + 12 + j * 4);
			if (entry >> 24 != TFA_DESC_PROFILE)
				continue;
			*prof = entry & GENMASK(23, 0);
			if (!tfa9872_range(fw, *prof, 8) || !fw->data[*prof] ||
			    !tfa9872_range(fw, *prof + 8, (fw->data[*prof] - 1) * 4))
				return -EINVAL;
			entry = get_unaligned_le32(fw->data + *prof + 4);
			name = entry & GENMASK(23, 0);
			if (entry >> 24 != TFA_DESC_STRING || !tfa9872_range(fw, name, 1) ||
			    !memchr(fw->data + name, 0, fw->size - name))
				return -EINVAL;
			if (!strcmp((const char *)fw->data + name, profile))
				return 0;
		}
		return -ENOENT;
	}
	return -ENODEV;
}

void tfa9872_config_free(struct tfa9872_config *config)
{
	if (!config)
		return;
	kfree(config->regs);
	kfree(config->messages);
	kfree(config);
}

int tfa9872_container_parse(const struct firmware *fw, u8 address,
			    unsigned int rate, const char *profile,
			    unsigned int vstep, unsigned int resistance,
			    struct tfa9872_config **result)
{
	struct tfa9872_parser parser = { .fw = fw, .vstep = vstep };
	struct tfa9872_config *config;
	u8 calibration[9] = { 0, 0x81, 5 };
	u32 dev, prof, re25;
	unsigned int i, rate_mask = 0, rate_code = 0;
	int ret;

	*result = NULL;
	if (fw->size < 50 || fw->size > SZ_256K || memcmp(fw->data, "PM", 2) ||
	    get_unaligned_le32(fw->data + 6) != fw->size)
		return -EINVAL;
	if (~crc32_le(~0U, fw->data + 14, fw->size - 14) !=
	    get_unaligned_le32(fw->data + 10))
		return -EBADMSG;
	if (rate != 48000)
		return -EOPNOTSUPP;
	if (!resistance || resistance > U16_MAX)
		return -ENODATA;
	ret = tfa9872_find_lists(fw, address, profile, &dev, &prof);
	if (ret)
		return ret;

	config = kzalloc_obj(*config);
	if (!config)
		return -ENOMEM;
	parser.config = config;
	parser.max_regs = fw->data[dev] + fw->data[prof] - 1 + U8_MAX;
	config->regs = kcalloc(parser.max_regs, sizeof(*config->regs), GFP_KERNEL);
	config->messages = kmalloc(SZ_64K, GFP_KERNEL);
	if (!config->regs || !config->messages) {
		ret = -ENOMEM;
		goto fail;
	}
	config->messages[0] = 'm';
	config->messages[1] = 'm';
	config->message_size = 4;
	ret = tfa9872_parse_list(&parser, dev + 12, fw->data[dev], true);
	if (!ret)
		ret = tfa9872_parse_list(&parser, prof + 8, fw->data[prof] - 1, false);
	if (ret)
		goto fail;
	if (!parser.has_vstep) {
		ret = -ENOENT;
		goto fail;
	}
	for (i = 0; i < config->num_regs; i++) {
		const struct tfa9872_reg_setting *setting = &config->regs[i];

		if (setting->reg != 0x02)
			continue;
		rate_code = (rate_code & ~setting->mask) | setting->value;
		rate_mask |= setting->mask & GENMASK(3, 0);
	}
	if (rate_mask != GENMASK(3, 0) || (rate_code & GENMASK(3, 0)) != 8) {
		ret = -EINVAL;
		goto fail;
	}

	/* SetRe25C terminates the configuration, with mono impedance in both channels. */
	re25 = (u64)resistance * 65536 / 1000;
	for (i = 0; i < 2; i++)
		put_unaligned_be24(re25, calibration + 3 + i * 3);
	ret = tfa9872_append_message(config, calibration, sizeof(calibration));
	if (ret)
		goto fail;
	put_unaligned_be16(config->message_size - 4, config->messages + 2);
	*result = config;
	return 0;
fail:
	tfa9872_config_free(config);
	return ret;
}
