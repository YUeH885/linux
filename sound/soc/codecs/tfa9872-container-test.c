// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/crc32.h>
#include <linux/firmware.h>
#include <linux/unaligned.h>

#include "tfa9872.h"

struct tfa9872_fixture {
	struct firmware fw;
	u8 *data;
	u32 vstep;
};

static void tfa9872_test_crc(u8 *data, unsigned int offset, unsigned int size)
{
	put_unaligned_le32(~crc32_le(~0U, data + offset, size - offset), data + offset - 4);
}

static u32 tfa9872_test_file(struct tfa9872_fixture *fixture, u16 type,
			     const u8 *payload, unsigned int size)
{
	u32 offset = fixture->fw.size;
	u8 *file = fixture->data + offset + 8;
	unsigned int header = type == 0x5053 ? 76 : 36;

	put_unaligned_le32(header + size, fixture->data + offset + 4);
	put_unaligned_le16(type, file);
	memcpy(file + 2, type == 0x5056 ? "3_01" : "1_00", 4);
	put_unaligned_le16(header + size, file + 6);
	memcpy(file + header, payload, size);
	tfa9872_test_crc(file, 12, header + size);
	fixture->fw.size += 8 + header + size;
	return offset | (4 << 24);
}

static int tfa9872_test_init(struct kunit *test)
{
	static const u8 command[] = { 0, 0x80, 6, 0xff, 0, 1 };
	static const u8 message[] = { 0, 0x80, 0x13, 0xff, 0, 1 };
	static const u8 speaker[] = { 4, 0x81, 6, 0, 0, 1 };
	static const u8 vstep[] = {
		5, 21, 0, 2,
		1, 0x22, 0x24, 0, 23,
		4,
		0, 0, 0, 2, 4, 0x81, 0x10, 0x80, 0, 0,
		1, 0, 0, 1, 4, 0x82, 0,
		2, 0, 0, 1, 4, 0x81, 8,
		3, 0, 0, 5, 'i', 'n', 'f', 'o', 0,
		0, 1,
		0, 0, 0, 2, 4, 0x81, 0x10, 0, 0, 2,
	};
	struct tfa9872_fixture *fixture;
	u8 *data;
	u32 pos;

	fixture = kunit_kzalloc(test, sizeof(*fixture), GFP_KERNEL);
	if (!fixture)
		return -ENOMEM;
	data = kunit_kzalloc(test, 512, GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	fixture->data = data;
	fixture->fw.data = data;
	fixture->fw.size = 146;
	memcpy(data, "PM1_00", 6);
	put_unaligned_le16(1, data + 40);
	put_unaligned_le32(50, data + 46);
	data[50] = 5;
	data[52] = 0x34;
	put_unaligned_le32(114 | (16 << 24), data + 62);
	put_unaligned_le32(126 | (21 << 24), data + 66);
	put_unaligned_le32(82 | (1 << 24), data + 78);
	data[82] = 5;
	put_unaligned_le32(106 | (3 << 24), data + 86);
	put_unaligned_le32(118 | (16 << 24), data + 90);
	put_unaligned_le32(17 << 24, data + 98);
	put_unaligned_le32(122 | (16 << 24), data + 102);
	memcpy(data + 106, "stereo", 7);
	put_unaligned_le16(23, data + 114);
	put_unaligned_le16(0x2224, data + 116);
	put_unaligned_le16(8, data + 118);
	put_unaligned_le16(0x0203, data + 120);
	put_unaligned_le16(7, data + 122);
	put_unaligned_le16(0x0203, data + 124);
	put_unaligned_le16(6, data + 126);
	memcpy(data + 128, command, sizeof(command));
	pos = tfa9872_test_file(fixture, 0x474d, message, sizeof(message));
	put_unaligned_le32(pos, data + 70);
	pos = tfa9872_test_file(fixture, 0x5053, speaker, sizeof(speaker));
	put_unaligned_le32(pos, data + 74);
	pos = tfa9872_test_file(fixture, 0x5056, vstep, sizeof(vstep));
	fixture->vstep = (pos & 0xffffff) + 8;
	put_unaligned_le32(pos, data + 94);
	put_unaligned_le32(fixture->fw.size, data + 6);
	tfa9872_test_crc(data, 14, fixture->fw.size);
	test->priv = fixture;
	return 0;
}

static int tfa9872_test_parse(struct kunit *test, unsigned int step,
			      struct tfa9872_config **config)
{
	struct tfa9872_fixture *fixture = test->priv;

	return tfa9872_container_parse(&fixture->fw, 0x34, 48000, "stereo",
				      step, 7243, config);
}

static void tfa9872_test_messages(struct kunit *test)
{
	static const u32 ids[] = {
		0x008006, 0x008013, 0x048106, 0x048100,
		0x048200, 0x048107, 0x008105,
	};
	struct tfa9872_config *config;
	unsigned int i, pos = 4, size;

	KUNIT_ASSERT_EQ(test, tfa9872_test_parse(test, 0, &config), 0);
	KUNIT_EXPECT_EQ(test, config->num_regs, 3);
	KUNIT_EXPECT_EQ(test, config->regs[1].value, 8);
	KUNIT_EXPECT_EQ(test, config->message_size, 70);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(config->messages + 2), 66);
	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		size = get_unaligned_be16(config->messages + pos);
		pos += 2;
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(config->messages + pos), ids[i]);
		pos += size;
	}
	KUNIT_EXPECT_EQ(test, pos, config->message_size);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(config->messages + 10), 0xffff0001);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(config->messages + 40), 0xff800000);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(config->messages + pos - 8), 474677);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(config->messages + pos - 4), 474677);
	tfa9872_config_free(config);
}

static void tfa9872_test_second_step(struct kunit *test)
{
	struct tfa9872_config *config;

	KUNIT_ASSERT_EQ(test, tfa9872_test_parse(test, 1, &config), 0);
	KUNIT_EXPECT_EQ(test, config->num_regs, 2);
	KUNIT_EXPECT_EQ(test, config->message_size, 58);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(config->messages + 40), 2);
	tfa9872_config_free(config);
}

static void tfa9872_test_selection(struct kunit *test)
{
	struct tfa9872_fixture *fixture = test->priv;
	struct tfa9872_config *config;

	KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 2, &config), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, config, NULL);
	KUNIT_EXPECT_EQ(test, tfa9872_container_parse(&fixture->fw, 0x35, 48000,
						      "stereo", 0, 7243, &config), -ENODEV);
	KUNIT_EXPECT_EQ(test, tfa9872_container_parse(&fixture->fw, 0x34, 48000,
						      "stereo-other", 0, 7243, &config), -ENOENT);
	KUNIT_EXPECT_EQ(test, tfa9872_container_parse(&fixture->fw, 0x34, 96000,
						      "stereo", 0, 7243, &config), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, tfa9872_container_parse(&fixture->fw, 0x34, 48000,
						      "stereo", 0, 0, &config), -ENODATA);
}

static void tfa9872_test_bounds(struct kunit *test)
{
	struct tfa9872_fixture *fixture = test->priv;
	struct tfa9872_config *config;
	unsigned int size = fixture->fw.size, i;

	for (i = 0; i < 50; i++) {
		fixture->fw.size = i;
		KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 0, &config), -EINVAL);
		KUNIT_EXPECT_PTR_EQ(test, config, NULL);
	}
	fixture->fw.size = size;
	put_unaligned_le32(0xfffffe | (4 << 24), fixture->data + 94);
	tfa9872_test_crc(fixture->data, 14, size);
	KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 0, &config), -EINVAL);
}

static void tfa9872_test_crc_failure(struct kunit *test)
{
	struct tfa9872_fixture *fixture = test->priv;
	struct tfa9872_config *config;

	fixture->data[fixture->vstep + 40] ^= 1;
	KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 0, &config), -EBADMSG);
	tfa9872_test_crc(fixture->data, 14, fixture->fw.size);
	KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 0, &config), -EBADMSG);
}

static void tfa9872_test_vstep_bounds(struct kunit *test)
{
	struct tfa9872_fixture *fixture = test->priv;
	struct tfa9872_config *config;
	u8 *file = fixture->data + fixture->vstep;

	file[46] = 0xff;
	tfa9872_test_crc(file, 12, get_unaligned_le16(file + 6));
	tfa9872_test_crc(fixture->data, 14, fixture->fw.size);
	KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 0, &config), -EINVAL);
}

static void tfa9872_test_rate_mismatch(struct kunit *test)
{
	struct tfa9872_fixture *fixture = test->priv;
	struct tfa9872_config *config;

	put_unaligned_le16(7, fixture->data + 118);
	tfa9872_test_crc(fixture->data, 14, fixture->fw.size);
	KUNIT_EXPECT_EQ(test, tfa9872_test_parse(test, 0, &config), -EINVAL);
}

static struct kunit_case tfa9872_container_cases[] = {
	KUNIT_CASE(tfa9872_test_messages),
	KUNIT_CASE(tfa9872_test_second_step),
	KUNIT_CASE(tfa9872_test_selection),
	KUNIT_CASE(tfa9872_test_bounds),
	KUNIT_CASE(tfa9872_test_crc_failure),
	KUNIT_CASE(tfa9872_test_vstep_bounds),
	KUNIT_CASE(tfa9872_test_rate_mismatch),
	{ }
};

static struct kunit_suite tfa9872_container_suite = {
	.name = "tfa9872-container",
	.init = tfa9872_test_init,
	.test_cases = tfa9872_container_cases,
};

kunit_test_suite(tfa9872_container_suite);
