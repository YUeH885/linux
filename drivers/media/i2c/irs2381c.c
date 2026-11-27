// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 The LM-G820N Linux contributors

#include <linux/clk.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define IRS2381C_LINK_FREQ_400MHZ	400000000ULL
#define IRS2381C_MCLK_FREQ_24MHZ	(24000 * HZ_PER_KHZ)
#define IRS2381C_DATA_LANES		2

#define IRS2381C_REG_CHIP_ID		CCI_REG16(0x0000)
#define IRS2381C_CHIP_ID		0x0300
#define IRS2381C_REG_DESIGN_STEP	CCI_REG16(0xa0a5)

/* M2453_B11 SPI Flash bridge and sequencer registers */
#define IRS2381C_REG_SPI_BUF_TX0	CCI_REG16(0x0000)
#define IRS2381C_REG_SPI_BUF_TX1	CCI_REG16(0x0001)
#define IRS2381C_REG_SPI_BUF_RX		0x0002
#define IRS2381C_REG_SPI_CFG		CCI_REG16(0xa087)
#define IRS2381C_REG_SPI_TX_ADDR	CCI_REG16(0xa088)
#define IRS2381C_REG_SPI_RX_ADDR	CCI_REG16(0xa089)
#define IRS2381C_REG_SPI_LEN		CCI_REG16(0xa08a)
#define IRS2381C_REG_SPI_TRIG		CCI_REG16(0xa08b)
#define IRS2381C_REG_SPI_STATUS		CCI_REG16(0xa08c)

#define IRS2381C_REG_EXPO_BASE		0x9000
#define IRS2381C_REG_EXPO_STRIDE	2
#define IRS2381C_REG_STREAM_CTRL	CCI_REG16(0x9400)
#define IRS2381C_REG_RECONFIG_TRIG	CCI_REG16(0x9402)

#define IRS2381C_FLASH_CHUNK_BYTES	256
#define IRS2381C_ZWETSCHGE_TOC_ADDR	0x2000
#define IRS2381C_ZWETSCHGE_TOC_SIZE	0x58
#define IRS2381C_ZWETSCHGE_MAX_NVM_SIZE	(256 * 1024)
#define IRS2381C_ZWETSCHGE_MAX_UC_SIZE	(32 * 1024)

#define IRS2381C_WIDTH			224
#define IRS2381C_ACTIVE_HEIGHT		172
#define IRS2381C_SUBFRAME_ROWS		(IRS2381C_ACTIVE_HEIGHT + 1)

#define IRS2381C_MAX_MODES		32
#define IRS2381C_MAX_EXPO_GROUPS	8
#define IRS2381C_MAX_SUBFRAMES		16
#define IRS2381C_MAX_MOD_FREQS		16

#define IRS2381C_NUM_SUPPLIES		3

#define to_irs2381c(_sd)		container_of(_sd, struct irs2381c, sd)

static const char * const irs2381c_supply_names[IRS2381C_NUM_SUPPLIES] = {
	"vddio",
	"vdda",
	"vddd",
};

static const s64 irs2381c_link_freq_menu[] = {
	IRS2381C_LINK_FREQ_400MHZ,
};

static const u32 irs2381c_mbus_formats[] = {
	MEDIA_BUS_FMT_SBGGR12_1X12,
};

struct irs2381c_expo_group {
	u16 default_us;
	u16 min_us;
	u16 max_us;
	u32 mod_freq_hz;
	bool is_modulated;
};

struct irs2381c_subframe_expo {
	u8 group_idx;
	u32 mod_freq_hz;
	bool is_modulated;
};

struct irs2381c_mode {
	char name[48];
	u32 width;
	u32 active_height;
	u32 csi_height;
	u32 subframes;
	u32 flash_offset;
	u32 fps;
	u8 num_expo_groups;
	struct irs2381c_expo_group expo_groups[IRS2381C_MAX_EXPO_GROUPS];
	u8 num_expo_subframes;
	struct irs2381c_subframe_expo sub_expo[IRS2381C_MAX_SUBFRAMES];
	u32 mod_expo_min_us;
	u32 mod_expo_max_us;
	u32 mod_expo_def_us;
};

static const struct irs2381c_mode irs2381c_fallback_mode = {
	.name = "MODE_9_5FPS_FALLBACK",
	.width = IRS2381C_WIDTH,
	.active_height = IRS2381C_ACTIVE_HEIGHT,
	.csi_height = 11 * IRS2381C_SUBFRAME_ROWS,
	.subframes = 9,
	.flash_offset = 0x2a00,
	.fps = 5,
	.num_expo_groups = 0,
	.num_expo_subframes = 0,
	.mod_expo_min_us = 100,
	.mod_expo_max_us = 2020,
	.mod_expo_def_us = 2020,
};

struct irs2381c {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct device *dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[IRS2381C_NUM_SUPPLIES];

	struct irs2381c_mode modes[IRS2381C_MAX_MODES];
	unsigned int num_modes;
	const struct irs2381c_mode *cur_mode;

	struct nvmem_device *nvmem;
	u8 *nvm_buf;
	size_t nvm_size;
	u32 zwetschge_base;
	u32 cal_addr;
	u32 cal_size;
	u32 cal_crc;
	char module_serial[20];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;

	bool streaming;
};

static const struct media_entity_operations irs2381c_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct cci_reg_sequence irs2381c_initial_config[] = {
	/* ZWETSCHGE EEPROM eLENA initialization map. */
	{ CCI_REG16(0xa001), 0x0007 },
	{ CCI_REG16(0xa008), 0x1513 },
	{ CCI_REG16(0xa00c), 0x0135 },
	{ CCI_REG16(0x9401), 0x0002 },
	{ CCI_REG16(0xa087), 0x8003 },
	{ CCI_REG16(0xa039), 0x16a1 },
	{ CCI_REG16(0xa03a), 0x5555 },
	{ CCI_REG16(0xa03b), 0x0005 },
	{ CCI_REG16(0xa03c), 0x0000 },
	{ CCI_REG16(0xa03d), 0x04d0 },
	{ CCI_REG16(0xa03e), 0x0000 },
	{ CCI_REG16(0xa03f), 0x0017 },
};

static u32 irs2381c_get_le24(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}

static u32 irs2381c_crc32(const u8 *data, size_t len)
{
	return crc32_le(~0U, data, len) ^ ~0U;
}

static u16 irs2381c_calc_reg_exposure(u32 exposure_us, u32 mod_freq_hz)
{
	static const u32 prescale_divs[4] = { 1, 8, 32, 128 };
	unsigned int i;

	if (!mod_freq_hz)
		mod_freq_hz = IRS2381C_MCLK_FREQ_24MHZ;

	for (i = 0; i < ARRAY_SIZE(prescale_divs); i++) {
		u64 cycles = div_u64((u64)exposure_us * mod_freq_hz,
				     1000000ULL * prescale_divs[i]);

		if (cycles <= 0x3fff)
			return (i << 14) | (u16)cycles;
	}

	return (3 << 14) | 0x3fff;
}

static int irs2381c_i2c_read_regs_raw(struct irs2381c *irs2381c, u16 start_reg,
				      u8 *buf, size_t len)
{
	__be16 reg_be = cpu_to_be16(start_reg);
	struct i2c_msg msgs[2] = {
		{
			.addr = irs2381c->client->addr,
			.flags = 0,
			.len = sizeof(reg_be),
			.buf = (u8 *)&reg_be,
		},
		{
			.addr = irs2381c->client->addr,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret;

	ret = i2c_transfer(irs2381c->client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

/*
 * Read arbitrary bytes from the module's onboard SPI Flash through the
 * M2453_B11 hardware SPI master bridge (0xa087..0xa08c). The imager must
 * be powered on and idle (not streaming).
 */
static int irs2381c_flash_read(struct irs2381c *irs2381c, u32 flash_addr,
			       u8 *dst, size_t len)
{
	u8 *rx_buf;
	size_t offset = 0;
	int ret = 0;

	rx_buf = kmalloc(IRS2381C_FLASH_CHUNK_BYTES, GFP_KERNEL);
	if (!rx_buf)
		return -ENOMEM;

	cci_write(irs2381c->regmap, IRS2381C_REG_SPI_CFG, 0x4002, &ret);
	cci_write(irs2381c->regmap, IRS2381C_REG_SPI_TX_ADDR, 0x0000, &ret);
	cci_write(irs2381c->regmap, IRS2381C_REG_SPI_RX_ADDR,
		  IRS2381C_REG_SPI_BUF_RX, &ret);
	if (ret)
		goto out_free;

	while (offset < len) {
		size_t chunk = min_t(size_t, len - offset,
				     IRS2381C_FLASH_CHUNK_BYTES);
		size_t xfer_bytes = ALIGN(chunk, 2);
		u32 cur_addr = flash_addr + offset;
		u64 status = 0;
		int poll_ret;

		cci_write(irs2381c->regmap, IRS2381C_REG_SPI_LEN,
			  0xe000 | (xfer_bytes + 3), &ret);
		cci_write(irs2381c->regmap, IRS2381C_REG_SPI_BUF_TX0,
			  0x0300 | ((cur_addr >> 16) & 0xff), &ret);
		cci_write(irs2381c->regmap, IRS2381C_REG_SPI_BUF_TX1,
			  cur_addr & 0xffff, &ret);
		cci_write(irs2381c->regmap, IRS2381C_REG_SPI_TRIG, 0x0002,
			  &ret);
		if (ret)
			goto out_free;

		/*
		 * Wait before the first SPI_STATUS read so the bridge has time
		 * to de-assert idle after SPI_TRIG; otherwise an immediate poll
		 * can see stale idle=1 and race SRAM reads against SPI writes.
		 */
		poll_ret = read_poll_timeout(cci_read, ret,
					     ret || (status & 0x0001),
					     200, 10000, true,
					     irs2381c->regmap,
					     IRS2381C_REG_SPI_STATUS,
					     &status, NULL);
		if (ret)
			goto out_free;
		if (poll_ret) {
			ret = poll_ret;
			goto out_free;
		}

		ret = irs2381c_i2c_read_regs_raw(irs2381c,
						 IRS2381C_REG_SPI_BUF_RX,
						 rx_buf, xfer_bytes);
		if (ret)
			goto out_free;

		memcpy(dst + offset, rx_buf, chunk);
		offset += chunk;
	}

out_free:
	kfree(rx_buf);
	return ret;
}

static int irs2381c_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct irs2381c *irs2381c = to_irs2381c(sd);
	int ret;

	gpiod_set_value_cansleep(irs2381c->reset_gpio, 1);

	ret = regulator_bulk_enable(IRS2381C_NUM_SUPPLIES, irs2381c->supplies);
	if (ret) {
		dev_err(dev, "failed to enable supplies: %d\n", ret);
		return ret;
	}
	usleep_range(5000, 7000);

	ret = clk_prepare_enable(irs2381c->mclk);
	if (ret) {
		dev_err(dev, "failed to enable MCLK clock: %d\n", ret);
		goto disable_supplies;
	}
	usleep_range(5000, 7000);

	gpiod_set_value_cansleep(irs2381c->reset_gpio, 0);
	usleep_range(10000, 15000);

	return 0;

disable_supplies:
	regulator_bulk_disable(IRS2381C_NUM_SUPPLIES, irs2381c->supplies);
	return ret;
}

static int irs2381c_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct irs2381c *irs2381c = to_irs2381c(sd);

	gpiod_set_value_cansleep(irs2381c->reset_gpio, 1);
	usleep_range(1000, 2000);
	clk_disable_unprepare(irs2381c->mclk);
	regulator_bulk_disable(IRS2381C_NUM_SUPPLIES, irs2381c->supplies);

	return 0;
}

static int irs2381c_identify_sensor(struct irs2381c *irs2381c)
{
	u64 val, step = 0;
	int ret;

	ret = cci_read(irs2381c->regmap, IRS2381C_REG_CHIP_ID, &val, NULL);
	if (ret) {
		dev_err(irs2381c->dev, "failed to read chip id: %d\n", ret);
		return ret;
	}

	if (val != IRS2381C_CHIP_ID) {
		dev_err(irs2381c->dev, "chip id mismatch: %x!=%llx\n",
			IRS2381C_CHIP_ID, val);
		return -ENODEV;
	}

	if (!cci_read(irs2381c->regmap, IRS2381C_REG_DESIGN_STEP, &step, NULL))
		dev_info(irs2381c->dev, "identified IRS2381C design step 0x%04llx\n",
			 step);

	return 0;
}

static int irs2381c_parse_elena_usecases(struct irs2381c *irs2381c,
					 const u8 *uc, size_t uc_size)
{
	size_t off = 9;
	u32 expected_crc, calc_crc;

	if (uc_size < 16 || memcmp(uc, "Elena", 5) != 0) {
		dev_warn(irs2381c->dev, "invalid Elena use-case header\n");
		return -EINVAL;
	}

	expected_crc = get_unaligned_le32(uc + 5);
	calc_crc = irs2381c_crc32(uc + 9, uc_size - 9);
	if (calc_crc != expected_crc) {
		dev_warn(irs2381c->dev,
			 "Elena CRC32 mismatch: got 0x%08x expected 0x%08x\n",
			 calc_crc, expected_crc);
		return -EINVAL;
	}

	while (off + 0x41 <= uc_size && irs2381c->num_modes < IRS2381C_MAX_MODES) {
		const u8 *p = uc + off;
		u16 entry_len = get_unaligned_le16(p + 0x00);
		u32 flash_off = irs2381c_get_le24(p + 0x02);
		u32 reg_size = irs2381c_get_le24(p + 0x05);
		u16 width = get_unaligned_le16(p + 0x0a);
		u16 height = get_unaligned_le16(p + 0x0c);
		u8 target_fps = p[0x1e];
		u8 name_len = p[0x35];
		u16 num_meas_blocks = get_unaligned_le16(p + 0x36);
		u16 num_mod_freqs = get_unaligned_le16(p + 0x38);
		u16 num_reg_entries = get_unaligned_le16(p + 0x3a);
		u8 num_streams = p[0x3c];
		u8 num_expo_groups = p[0x3d];
		u16 num_rfs = get_unaligned_le16(p + 0x3e);
		u8 num_extra = p[0x40];
		size_t calc_len;
		const u8 *cur;
		struct irs2381c_mode *mode;
		u32 total_subframes = 0;
		u32 mod_freqs[IRS2381C_MAX_MOD_FREQS] = { 0 };
		unsigned int i;
		bool have_mod_expo = false;

		if (entry_len < 0x41 || off + entry_len > uc_size)
			break;

		calc_len = 0x41 + name_len + num_meas_blocks * 2 +
			   num_mod_freqs * 4 + num_reg_entries * 6 +
			   num_streams * 2 + num_expo_groups * 6 +
			   num_rfs * 6 + num_extra;
		if (calc_len != entry_len) {
			dev_warn(irs2381c->dev,
				 "Elena entry at 0x%zx length mismatch (%zu != %u)\n",
				 off, calc_len, entry_len);
			break;
		}

		off += entry_len;

		/* Skip non-flash or zero-dimension entries */
		if (!flash_off || !reg_size || !width || !height || !target_fps)
			continue;

		cur = p + 0x41;
		mode = &irs2381c->modes[irs2381c->num_modes];
		memset(mode, 0, sizeof(*mode));

		memcpy(mode->name, cur, min_t(size_t, name_len, sizeof(mode->name) - 1));
		cur += name_len;

		for (i = 0; i < num_meas_blocks; i++) {
			total_subframes += get_unaligned_le16(cur);
			cur += 2;
		}

		for (i = 0; i < num_mod_freqs; i++) {
			u32 f = get_unaligned_le32(cur);

			if (i < IRS2381C_MAX_MOD_FREQS)
				mod_freqs[i] = f;
			cur += 4;
		}

		/* Skip timed register overrides and stream IDs */
		cur += num_reg_entries * 6;
		cur += num_streams * 2;

		mode->num_expo_groups = min_t(u8, num_expo_groups,
					      IRS2381C_MAX_EXPO_GROUPS);
		for (i = 0; i < num_expo_groups; i++) {
			u16 def_us = get_unaligned_le16(cur + 0);
			u16 min_us = get_unaligned_le16(cur + 2);
			u16 max_us = get_unaligned_le16(cur + 4);

			if (i < IRS2381C_MAX_EXPO_GROUPS) {
				mode->expo_groups[i].default_us = def_us;
				mode->expo_groups[i].min_us = min_us;
				mode->expo_groups[i].max_us = max_us;
				if (i < num_mod_freqs && mod_freqs[i])
					mode->expo_groups[i].mod_freq_hz = mod_freqs[i];
			}
			cur += 6;
		}

		mode->num_expo_subframes = 0;
		for (i = 0; i < num_rfs; i++) {
			u8 rfs_count = cur[0];
			u32 rfs_freq = get_unaligned_le32(cur + 1);
			u8 grp = cur[5];
			bool is_mod = rfs_count >= 4;
			unsigned int j;

			if (grp < mode->num_expo_groups) {
				if (is_mod)
					mode->expo_groups[grp].is_modulated = true;
				if (rfs_freq)
					mode->expo_groups[grp].mod_freq_hz = rfs_freq;
			}
			for (j = 0; j < rfs_count &&
				    mode->num_expo_subframes < IRS2381C_MAX_SUBFRAMES; j++) {
				struct irs2381c_subframe_expo *se =
					&mode->sub_expo[mode->num_expo_subframes++];

				se->group_idx = grp;
				se->mod_freq_hz = rfs_freq;
				se->is_modulated = is_mod;
			}
			cur += 6;
		}

		/*
		 * Elena's width/height fields describe the Spectre processing
		 * crop (172x120), whereas the sensor always streams full
		 * 224x172 active pixels plus 1 PseudoData row per subframe.
		 */
		mode->width = IRS2381C_WIDTH;
		mode->active_height = IRS2381C_ACTIVE_HEIGHT;
		mode->flash_offset = flash_off;
		mode->fps = target_fps;
		mode->subframes = total_subframes;
		/*
		 * Keep unique (subframes, fps) primary modes (skip _OPT and
		 * duplicate frequency variants that share the same CSI geometry
		 * and frame rate, as well as 2-subframe diagnostic modes).
		 */
		if (total_subframes != 9 && total_subframes != 5)
			continue;
		if (strstr(mode->name, "_OPT"))
			continue;
		{
			bool dup = false;

			for (i = 0; i < irs2381c->num_modes; i++) {
				if (irs2381c->modes[i].subframes == total_subframes &&
				    irs2381c->modes[i].fps == target_fps) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;
		}
		/*
		 * M2453_B11 pads each MIPI superframe with 2 blanking
		 * subframes (9-phase -> 11*173 = 1903 rows, 5-phase ->
		 * 7*173 = 1211 rows).
		 */
		mode->csi_height = (total_subframes + 2) * IRS2381C_SUBFRAME_ROWS;

		mode->mod_expo_min_us = 1;
		mode->mod_expo_max_us = 1000;
		mode->mod_expo_def_us = 1000;
		for (i = 0; i < mode->num_expo_groups; i++) {
			const struct irs2381c_expo_group *g = &mode->expo_groups[i];

			if (!g->is_modulated || !g->max_us)
				continue;
			if (!have_mod_expo) {
				mode->mod_expo_min_us = g->min_us;
				mode->mod_expo_max_us = g->max_us;
				mode->mod_expo_def_us = g->default_us;
				have_mod_expo = true;
			} else {
				/*
				 * Use the tightest intersection across all
				 * modulated groups to guarantee laser eye
				 * safety for every modulation frequency.
				 */
				mode->mod_expo_min_us = max_t(u32, mode->mod_expo_min_us, g->min_us);
				mode->mod_expo_max_us = min_t(u32, mode->mod_expo_max_us, g->max_us);
				mode->mod_expo_def_us = clamp_t(u32, mode->mod_expo_def_us,
								mode->mod_expo_min_us,
								mode->mod_expo_max_us);
			}
		}

		dev_info(irs2381c->dev,
			 "usecase[%u]: %s off=0x%x %ux%u(sub=%u,csi_h=%u) %ufps expo=%u..%u(def=%u)us grps=%u\n",
			 irs2381c->num_modes, mode->name, mode->flash_offset,
			 mode->width, mode->active_height, mode->subframes,
			 mode->csi_height, mode->fps, mode->mod_expo_min_us,
			 mode->mod_expo_max_us, mode->mod_expo_def_us,
			 mode->num_expo_groups);
		irs2381c->num_modes++;
	}

	return irs2381c->num_modes ? 0 : -ENODATA;
}

static int irs2381c_nvmem_read(void *priv, unsigned int off, void *val,
			       size_t count)
{
	struct irs2381c *irs2381c = priv;
	struct v4l2_subdev_state *state;
	int ret = 0;

	if (off >= irs2381c->nvm_size || count > irs2381c->nvm_size - off)
		return -EINVAL;

	state = v4l2_subdev_lock_and_get_active_state(&irs2381c->sd);

	if (!irs2381c->nvm_buf) {
		u8 *buf;

		if (irs2381c->streaming) {
			ret = -EBUSY;
			goto unlock;
		}

		buf = devm_kzalloc(irs2381c->dev, irs2381c->nvm_size,
				   GFP_KERNEL);
		if (!buf) {
			ret = -ENOMEM;
			goto unlock;
		}

		ret = pm_runtime_resume_and_get(irs2381c->dev);
		if (ret < 0) {
			devm_kfree(irs2381c->dev, buf);
			goto unlock;
		}

		for (unsigned int attempt = 0; attempt < 2; attempt++) {
			ret = irs2381c_flash_read(irs2381c,
						  irs2381c->zwetschge_base,
						  buf, irs2381c->nvm_size);
			if (ret)
				break;

			if (irs2381c->cal_size &&
			    irs2381c->cal_addr >= irs2381c->zwetschge_base) {
				size_t cal_off = irs2381c->cal_addr -
						 irs2381c->zwetschge_base;
				u32 crc;

				if (cal_off + irs2381c->cal_size <= irs2381c->nvm_size) {
					crc = irs2381c_crc32(buf + cal_off,
							     irs2381c->cal_size);
					if (crc != irs2381c->cal_crc) {
						dev_warn(irs2381c->dev,
							 "Zwetschge calibration CRC mismatch (attempt %u): got 0x%08x expected 0x%08x\n",
							 attempt + 1, crc,
							 irs2381c->cal_crc);
						ret = -EIO;
						continue;
					}
				}
			}
			break;
		}

		pm_runtime_mark_last_busy(irs2381c->dev);
		pm_runtime_put_autosuspend(irs2381c->dev);
		if (ret) {
			dev_err(irs2381c->dev,
				"failed to cache Zwetschge flash (%zu bytes): %d\n",
				irs2381c->nvm_size, ret);
			devm_kfree(irs2381c->dev, buf);
			goto unlock;
		}

		irs2381c->nvm_buf = buf;
	}

	memcpy(val, irs2381c->nvm_buf + off, count);

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static void irs2381c_load_zwetschge(struct irs2381c *irs2381c)
{
	u8 toc[IRS2381C_ZWETSCHGE_TOC_SIZE];
	u32 toc_crc, calc_crc, version;
	u32 regmap_addr, regmap_size;
	u32 uc_addr, uc_size;
	u32 cal_addr, cal_size, cal_crc;
	u32 sig_addr, sig_size;
	u32 max_end;
	u8 *uc_buf;
	int ret;

	ret = irs2381c_flash_read(irs2381c, IRS2381C_ZWETSCHGE_TOC_ADDR,
				  toc, sizeof(toc));
	if (ret) {
		dev_warn(irs2381c->dev, "failed to read Zwetschge ToC: %d\n", ret);
		return;
	}

	if (memcmp(toc, "ZWETSCHGE", 9) != 0) {
		dev_warn(irs2381c->dev,
			 "unexpected Zwetschge magic: %02x %02x %02x %02x\n",
			 toc[0], toc[1], toc[2], toc[3]);
		return;
	}

	toc_crc = get_unaligned_le32(toc + 0x09);
	calc_crc = irs2381c_crc32(toc + 0x0d, sizeof(toc) - 0x0d);
	version = irs2381c_get_le24(toc + 0x0d);
	if (calc_crc != toc_crc) {
		dev_warn(irs2381c->dev,
			 "Zwetschge ToC CRC32 mismatch: got 0x%08x expected 0x%08x\n",
			 calc_crc, toc_crc);
		return;
	}

	sig_addr = irs2381c_get_le24(toc + 0x10);
	sig_size = irs2381c_get_le24(toc + 0x13);
	regmap_addr = irs2381c_get_le24(toc + 0x2e);
	regmap_size = irs2381c_get_le24(toc + 0x31);
	cal_addr = irs2381c_get_le24(toc + 0x34);
	cal_size = irs2381c_get_le24(toc + 0x37);
	cal_crc = get_unaligned_le32(toc + 0x3a);
	uc_addr = irs2381c_get_le24(toc + 0x3f);
	uc_size = irs2381c_get_le24(toc + 0x42);

	memcpy(irs2381c->module_serial, toc + 0x45, 19);
	irs2381c->module_serial[19] = '\0';

	dev_info(irs2381c->dev,
		 "Zwetschge v0x%x serial=%s uc=0x%x+%u cal=0x%x+%u(crc=0x%08x)\n",
		 version, irs2381c->module_serial, uc_addr, uc_size,
		 cal_addr, cal_size, cal_crc);

	max_end = IRS2381C_ZWETSCHGE_TOC_ADDR + IRS2381C_ZWETSCHGE_TOC_SIZE;
	if (regmap_addr >= IRS2381C_ZWETSCHGE_TOC_ADDR && regmap_size)
		max_end = max(max_end, regmap_addr + regmap_size);
	if (uc_addr >= IRS2381C_ZWETSCHGE_TOC_ADDR && uc_size)
		max_end = max(max_end, uc_addr + uc_size);
	if (cal_addr >= IRS2381C_ZWETSCHGE_TOC_ADDR && cal_size)
		max_end = max(max_end, cal_addr + cal_size);
	if (sig_addr >= IRS2381C_ZWETSCHGE_TOC_ADDR && sig_size)
		max_end = max(max_end, sig_addr + sig_size);

	irs2381c->zwetschge_base = IRS2381C_ZWETSCHGE_TOC_ADDR;
	irs2381c->cal_addr = cal_addr;
	irs2381c->cal_size = cal_size;
	irs2381c->cal_crc = cal_crc;
	irs2381c->nvm_size = min_t(size_t,
				   ALIGN(max_end - irs2381c->zwetschge_base, 256),
				   IRS2381C_ZWETSCHGE_MAX_NVM_SIZE);

	if (uc_addr && uc_size >= 16 && uc_size <= IRS2381C_ZWETSCHGE_MAX_UC_SIZE) {
		uc_buf = kmalloc(uc_size, GFP_KERNEL);
		if (uc_buf) {
			ret = irs2381c_flash_read(irs2381c, uc_addr, uc_buf, uc_size);
			if (!ret)
				irs2381c_parse_elena_usecases(irs2381c, uc_buf, uc_size);
			else
				dev_warn(irs2381c->dev,
					 "failed to read Elena use-cases: %d\n", ret);
			kfree(uc_buf);
		}
	}

	if (irs2381c->nvm_size >= IRS2381C_ZWETSCHGE_TOC_SIZE) {
		struct nvmem_config cfg = {
			.name = dev_name(irs2381c->dev),
			.dev = irs2381c->dev,
			.base_dev = irs2381c->dev,
			.owner = THIS_MODULE,
			.read_only = true,
			.root_only = false,
			.compat = true,
			.stride = 1,
			.word_size = 1,
			.size = irs2381c->nvm_size,
			.reg_read = irs2381c_nvmem_read,
			.priv = irs2381c,
		};

		irs2381c->nvmem = devm_nvmem_register(irs2381c->dev, &cfg);
		if (IS_ERR(irs2381c->nvmem)) {
			dev_warn(irs2381c->dev,
				 "failed to register calibration nvmem: %ld\n",
				 PTR_ERR(irs2381c->nvmem));
			irs2381c->nvmem = NULL;
		}
	}
}

static int irs2381c_apply_exposure(struct irs2381c *irs2381c, u32 exposure_us,
				   bool live_latch)
{
	const struct irs2381c_mode *mode = irs2381c->cur_mode;
	u64 pending = 0;
	unsigned int i;
	int ret = 0;

	if (!mode || !mode->num_expo_groups || !mode->num_expo_subframes)
		return 0;

	exposure_us = clamp(exposure_us, mode->mod_expo_min_us,
			    mode->mod_expo_max_us);

	if (live_latch) {
		int poll_ret;

		poll_ret = read_poll_timeout(cci_read, ret,
					     ret || !(pending & 0x3),
					     200, 10000, false,
					     irs2381c->regmap,
					     IRS2381C_REG_RECONFIG_TRIG,
					     &pending, NULL);
		if (ret)
			return ret;
		if (poll_ret)
			return -EBUSY;
	}

	/*
	 * M2453_B11 stores one 16-bit exposure register per subframe starting
	 * at 0x9000 with stride 2 (e.g. 0x9000=gray, 0x9002..0x9008=f1,
	 * 0x900a..0x9010=f2 for 9-phase modes).
	 */
	for (i = 0; i < mode->num_expo_subframes; i++) {
		const struct irs2381c_subframe_expo *se = &mode->sub_expo[i];
		const struct irs2381c_expo_group *g;
		u32 grp_us;
		u16 reg_val;

		if (se->group_idx >= mode->num_expo_groups)
			continue;
		g = &mode->expo_groups[se->group_idx];

		if (se->is_modulated)
			grp_us = clamp_t(u32, exposure_us, g->min_us, g->max_us);
		else
			grp_us = clamp_t(u32, g->default_us, g->min_us, g->max_us);

		reg_val = irs2381c_calc_reg_exposure(grp_us, se->mod_freq_hz);
		cci_write(irs2381c->regmap,
			  CCI_REG16(IRS2381C_REG_EXPO_BASE +
				    i * IRS2381C_REG_EXPO_STRIDE),
			  reg_val, &ret);
	}

	if (!ret && live_latch)
		cci_write(irs2381c->regmap, IRS2381C_REG_RECONFIG_TRIG, 0x0001,
			  &ret);

	return ret;
}

static int irs2381c_start_streaming(struct irs2381c *irs2381c)
{
	u64 status;
	int ret;

	ret = cci_multi_reg_write(irs2381c->regmap, irs2381c_initial_config,
				  ARRAY_SIZE(irs2381c_initial_config), NULL);
	if (ret)
		return ret;

	cci_write(irs2381c->regmap, CCI_REG16(0xa02a), 0x0001, &ret);

	/* ImagerM2453_B11 copies the selected use case from module Flash. */
	cci_write(irs2381c->regmap, IRS2381C_REG_SPI_CFG, 0x4002, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x9380),
		  irs2381c->cur_mode->flash_offset & 0xffff, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x9381),
		  irs2381c->cur_mode->flash_offset >> 16, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x93e1), 0x0007, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x8452), 0x0004, &ret);
	cci_write(irs2381c->regmap, CCI_REG16(0x8401), 0x0008, &ret);
	if (ret)
		return ret;
	usleep_range(10000, 12000);

	cci_read(irs2381c->regmap, CCI_REG16(0x93e2), &status, &ret);
	if (ret)
		return ret;
	if (status != 0xaaaa) {
		dev_err(irs2381c->dev, "use case transfer failed: 0x%04llx\n", status);
		return -EIO;
	}

	if (irs2381c->exposure && irs2381c->cur_mode->num_expo_groups &&
	    (u32)irs2381c->exposure->val != irs2381c->cur_mode->mod_expo_def_us) {
		ret = irs2381c_apply_exposure(irs2381c,
					      irs2381c->exposure->val, false);
		if (ret)
			return ret;
	}

	return cci_write(irs2381c->regmap, IRS2381C_REG_STREAM_CTRL, 0x0001, NULL);
}

static int irs2381c_stop_streaming(struct irs2381c *irs2381c)
{
	int ret;

	ret = cci_write(irs2381c->regmap, IRS2381C_REG_STREAM_CTRL, 0x0000, NULL);
	if (ret)
		return ret;

	if (irs2381c->reset_gpio) {
		/*
		 * M2453_B11 otherwise finishes the active superframe (up to
		 * 160 ms at 5 fps) before accepting a new Flash use-case load.
		 * Pulsing reset returns the imager to a clean idle state for
		 * immediate back-to-back streaming or nvmem reads.
		 */
		usleep_range(5000, 7000);
		gpiod_set_value_cansleep(irs2381c->reset_gpio, 1);
		usleep_range(2000, 3000);
		gpiod_set_value_cansleep(irs2381c->reset_gpio, 0);
		usleep_range(10000, 15000);
	} else {
		usleep_range(200000, 220000);
	}

	return 0;
}

static int irs2381c_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (irs2381c->streaming == enable)
		goto unlock;

	if (enable) {
		ret = pm_runtime_resume_and_get(irs2381c->dev);
		if (ret < 0)
			goto unlock;

		ret = irs2381c_start_streaming(irs2381c);
		if (ret) {
			pm_runtime_put_autosuspend(irs2381c->dev);
			goto unlock;
		}
	} else {
		ret = irs2381c_stop_streaming(irs2381c);
		if (ret) {
			dev_err(irs2381c->dev, "failed to stop streaming: %d\n", ret);
			pm_runtime_put_sync_suspend(irs2381c->dev);
		} else {
			pm_runtime_mark_last_busy(irs2381c->dev);
			pm_runtime_put_autosuspend(irs2381c->dev);
		}
	}

	irs2381c->streaming = enable;

unlock:
	v4l2_subdev_unlock_state(state);
	return ret;
}

static int irs2381c_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct irs2381c *irs2381c =
		container_of(ctrl->handler, struct irs2381c, ctrl_handler);

	if (!irs2381c->streaming)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		return irs2381c_apply_exposure(irs2381c, ctrl->val, true);
	default:
		return -EINVAL;
	}
}

static const struct v4l2_ctrl_ops irs2381c_ctrl_ops = {
	.s_ctrl = irs2381c_set_ctrl,
};

static void irs2381c_update_mode_controls(struct irs2381c *irs2381c,
					  const struct irs2381c_mode *mode)
{
	if (irs2381c->exposure)
		__v4l2_ctrl_modify_range(irs2381c->exposure,
					 mode->mod_expo_min_us,
					 mode->mod_expo_max_us, 1,
					 mode->mod_expo_def_us);
}

static int irs2381c_init_controls(struct irs2381c *irs2381c)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &irs2381c->ctrl_handler;
	struct v4l2_fwnode_device_properties props;
	const struct irs2381c_mode *mode = irs2381c->cur_mode;
	s64 pixel_rate_val;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 8);

	irs2381c->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL,
						     V4L2_CID_LINK_FREQ,
						     ARRAY_SIZE(irs2381c_link_freq_menu) - 1,
						     0, irs2381c_link_freq_menu);
	if (irs2381c->link_freq)
		irs2381c->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate_val = (IRS2381C_LINK_FREQ_400MHZ * 2 * IRS2381C_DATA_LANES) / 12;
	irs2381c->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
						 V4L2_CID_PIXEL_RATE, pixel_rate_val,
						 pixel_rate_val, 1, pixel_rate_val);

	irs2381c->hblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_HBLANK,
					     16, 16, 1, 16);
	if (irs2381c->hblank)
		irs2381c->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	irs2381c->vblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL, V4L2_CID_VBLANK,
					     32, 32, 1, 32);
	if (irs2381c->vblank)
		irs2381c->vblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	irs2381c->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &irs2381c_ctrl_ops,
					       V4L2_CID_EXPOSURE,
					       mode->mod_expo_min_us,
					       mode->mod_expo_max_us, 1,
					       mode->mod_expo_def_us);

	ret = v4l2_fwnode_device_parse(irs2381c->dev, &props);
	if (ret)
		goto free_handler;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, NULL, &props);
	if (ret)
		goto free_handler;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto free_handler;
	}

	irs2381c->sd.ctrl_handler = ctrl_hdlr;
	return 0;

free_handler:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	return ret;
}

static int irs2381c_enum_mbus_code(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(irs2381c_mbus_formats))
		return -EINVAL;

	code->code = irs2381c_mbus_formats[code->index];
	return 0;
}

static int irs2381c_enum_frame_size(struct v4l2_subdev *sd,
				    struct v4l2_subdev_state *sd_state,
				    struct v4l2_subdev_frame_size_enum *fse)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	unsigned int i, j, unique_idx = 0;

	for (i = 0; i < irs2381c->num_modes; i++) {
		bool dup = false;

		for (j = 0; j < i; j++) {
			if (irs2381c->modes[j].width == irs2381c->modes[i].width &&
			    irs2381c->modes[j].csi_height == irs2381c->modes[i].csi_height) {
				dup = true;
				break;
			}
		}
		if (dup)
			continue;

		if (unique_idx == fse->index) {
			fse->min_width = irs2381c->modes[i].width;
			fse->max_width = irs2381c->modes[i].width;
			fse->min_height = irs2381c->modes[i].csi_height;
			fse->max_height = irs2381c->modes[i].csi_height;
			return 0;
		}
		unique_idx++;
	}

	return -EINVAL;
}

static int irs2381c_enum_frame_interval(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_frame_interval_enum *fie)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	unsigned int i, match_idx = 0;

	for (i = 0; i < irs2381c->num_modes; i++) {
		if (irs2381c->modes[i].width != fie->width ||
		    irs2381c->modes[i].csi_height != fie->height)
			continue;
		if (match_idx == fie->index) {
			fie->interval.numerator = 1;
			fie->interval.denominator = irs2381c->modes[i].fps;
			return 0;
		}
		match_idx++;
	}

	return -EINVAL;
}

static const struct irs2381c_mode *
irs2381c_find_mode(struct irs2381c *irs2381c, u32 width, u32 height, u32 fps)
{
	const struct irs2381c_mode *best = &irs2381c->modes[0];
	unsigned int best_score = UINT_MAX;
	unsigned int i;

	for (i = 0; i < irs2381c->num_modes; i++) {
		const struct irs2381c_mode *m = &irs2381c->modes[i];
		unsigned int dim_diff = abs((int)m->width - (int)width) +
					abs((int)m->csi_height - (int)height);
		unsigned int fps_diff = fps ? abs((int)m->fps - (int)fps) : 0;
		unsigned int score = dim_diff * 1000 + fps_diff;

		if (score < best_score) {
			best_score = score;
			best = m;
		}
	}

	return best;
}

static int irs2381c_set_fmt(struct v4l2_subdev *sd,
			    struct v4l2_subdev_state *sd_state,
			    struct v4l2_subdev_format *fmt)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	const struct irs2381c_mode *mode;
	u32 preferred_fps = irs2381c->cur_mode ? irs2381c->cur_mode->fps : 5;
	unsigned int i;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE && irs2381c->streaming)
		return -EBUSY;

	mode = irs2381c_find_mode(irs2381c, fmt->format.width,
				  fmt->format.height, preferred_fps);

	for (i = 0; i < ARRAY_SIZE(irs2381c_mbus_formats); i++) {
		if (fmt->format.code == irs2381c_mbus_formats[i])
			break;
	}
	if (i == ARRAY_SIZE(irs2381c_mbus_formats))
		i = 0;

	fmt->format.width = mode->width;
	fmt->format.height = mode->csi_height;
	fmt->format.code = irs2381c_mbus_formats[i];
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;

	*v4l2_subdev_state_get_format(sd_state, fmt->pad) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		irs2381c->cur_mode = mode;
		irs2381c_update_mode_controls(irs2381c, mode);
	}

	return 0;
}

static int irs2381c_get_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_interval *fi)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	const struct irs2381c_mode *mode = irs2381c->cur_mode;

	fi->interval.numerator = 1;
	fi->interval.denominator = mode ? mode->fps : 5;
	return 0;
}

static int irs2381c_set_frame_interval(struct v4l2_subdev *sd,
				       struct v4l2_subdev_state *sd_state,
				       struct v4l2_subdev_frame_interval *fi)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	const struct v4l2_mbus_framefmt *fmt;
	const struct irs2381c_mode *mode;
	u32 req_fps = 5;

	if (fi->which == V4L2_SUBDEV_FORMAT_ACTIVE && irs2381c->streaming)
		return -EBUSY;

	if (fi->interval.numerator > 0 && fi->interval.denominator > 0)
		req_fps = DIV_ROUND_CLOSEST(fi->interval.denominator,
					    fi->interval.numerator);

	fmt = v4l2_subdev_state_get_format(sd_state, fi->pad);
	mode = irs2381c_find_mode(irs2381c, fmt->width, fmt->height, req_fps);

	fi->interval.numerator = 1;
	fi->interval.denominator = mode->fps;

	if (fi->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		irs2381c->cur_mode = mode;
		irs2381c_update_mode_controls(irs2381c, mode);
	}

	return 0;
}

static int irs2381c_get_selection(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_selection *sel)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	const struct v4l2_mbus_framefmt *fmt;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		fmt = v4l2_subdev_state_get_format(sd_state, sel->pad);
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = fmt->width;
		sel->r.height = fmt->height;
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = irs2381c->modes[0].width;
		sel->r.height = irs2381c->modes[0].csi_height;
		return 0;
	default:
		return -EINVAL;
	}
}

static int irs2381c_init_state(struct v4l2_subdev *sd,
			       struct v4l2_subdev_state *sd_state)
{
	struct irs2381c *irs2381c = to_irs2381c(sd);
	const struct irs2381c_mode *mode = irs2381c->cur_mode ?
					   irs2381c->cur_mode :
					   &irs2381c->modes[0];
	struct v4l2_subdev_format fmt = {
		.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE,
		.format = {
			.width = mode->width,
			.height = mode->csi_height,
			.code = irs2381c_mbus_formats[0],
		},
	};

	return irs2381c_set_fmt(sd, sd_state, &fmt);
}

static const struct v4l2_subdev_video_ops irs2381c_video_ops = {
	.s_stream = irs2381c_set_stream,
};

static const struct v4l2_subdev_pad_ops irs2381c_pad_ops = {
	.enum_mbus_code = irs2381c_enum_mbus_code,
	.enum_frame_size = irs2381c_enum_frame_size,
	.enum_frame_interval = irs2381c_enum_frame_interval,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = irs2381c_set_fmt,
	.get_frame_interval = irs2381c_get_frame_interval,
	.set_frame_interval = irs2381c_set_frame_interval,
	.get_selection = irs2381c_get_selection,
};

static const struct v4l2_subdev_ops irs2381c_subdev_ops = {
	.video = &irs2381c_video_ops,
	.pad = &irs2381c_pad_ops,
};

static const struct v4l2_subdev_internal_ops irs2381c_internal_ops = {
	.init_state = irs2381c_init_state,
};

static int irs2381c_check_hwcfg(struct irs2381c *irs2381c)
{
	struct fwnode_handle *fwnode = dev_fwnode(irs2381c->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus = {
			.mipi_csi2 = {
				.num_data_lanes = IRS2381C_DATA_LANES,
			},
		},
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENXIO;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IRS2381C_DATA_LANES) {
		dev_err(irs2381c->dev, "unsupported number of data-lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(irs2381c->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       irs2381c_link_freq_menu,
				       ARRAY_SIZE(irs2381c_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);
	return ret;
}

static int irs2381c_probe(struct i2c_client *client)
{
	struct irs2381c *irs2381c;
	unsigned long freq;
	unsigned int i;
	int ret;

	irs2381c = devm_kzalloc(&client->dev, sizeof(*irs2381c), GFP_KERNEL);
	if (!irs2381c)
		return -ENOMEM;

	irs2381c->dev = &client->dev;
	irs2381c->client = client;
	v4l2_i2c_subdev_init(&irs2381c->sd, client, &irs2381c_subdev_ops);

	irs2381c->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(irs2381c->regmap))
		return dev_err_probe(irs2381c->dev, PTR_ERR(irs2381c->regmap),
				     "failed to init CCI regmap\n");

	irs2381c->mclk = devm_v4l2_sensor_clk_get(irs2381c->dev, NULL);
	if (IS_ERR(irs2381c->mclk))
		return dev_err_probe(irs2381c->dev, PTR_ERR(irs2381c->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(irs2381c->mclk);
	if (freq != IRS2381C_MCLK_FREQ_24MHZ)
		return dev_err_probe(irs2381c->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = irs2381c_check_hwcfg(irs2381c);
	if (ret)
		return dev_err_probe(irs2381c->dev, ret,
				     "failed to check HW configuration\n");

	irs2381c->reset_gpio = devm_gpiod_get_optional(irs2381c->dev, "reset",
						       GPIOD_OUT_HIGH);
	if (IS_ERR(irs2381c->reset_gpio))
		return dev_err_probe(irs2381c->dev, PTR_ERR(irs2381c->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < IRS2381C_NUM_SUPPLIES; i++)
		irs2381c->supplies[i].supply = irs2381c_supply_names[i];

	ret = devm_regulator_bulk_get(irs2381c->dev, IRS2381C_NUM_SUPPLIES,
				      irs2381c->supplies);
	if (ret)
		return dev_err_probe(irs2381c->dev, ret,
				     "failed to get supply regulators\n");

	ret = irs2381c_power_on(irs2381c->dev);
	if (ret)
		return ret;

	ret = irs2381c_identify_sensor(irs2381c);
	if (ret) {
		dev_err_probe(irs2381c->dev, ret, "failed to identify IRS2381C\n");
		goto power_off;
	}

	irs2381c_load_zwetschge(irs2381c);
	if (!irs2381c->num_modes) {
		irs2381c->modes[0] = irs2381c_fallback_mode;
		irs2381c->num_modes = 1;
	}

	/* Default to MODE_9_5FPS (0x2a00) when available */
	irs2381c->cur_mode = &irs2381c->modes[0];
	for (i = 0; i < irs2381c->num_modes; i++) {
		if (irs2381c->modes[i].flash_offset == 0x2a00) {
			irs2381c->cur_mode = &irs2381c->modes[i];
			break;
		}
	}

	ret = irs2381c_init_controls(irs2381c);
	if (ret) {
		dev_err_probe(irs2381c->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	irs2381c->sd.state_lock = irs2381c->ctrl_handler.lock;
	irs2381c->sd.internal_ops = &irs2381c_internal_ops;
	irs2381c->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	irs2381c->sd.entity.ops = &irs2381c_subdev_entity_ops;
	irs2381c->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	irs2381c->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&irs2381c->sd.entity, 1, &irs2381c->pad);
	if (ret) {
		dev_err_probe(irs2381c->dev, ret,
			      "failed to init media entity pads\n");
		goto free_ctrls;
	}

	ret = v4l2_subdev_init_finalize(&irs2381c->sd);
	if (ret < 0) {
		dev_err_probe(irs2381c->dev, ret,
			      "failed to finalize V4L2 subdev\n");
		goto cleanup_entity;
	}

	pm_runtime_set_active(irs2381c->dev);
	pm_runtime_enable(irs2381c->dev);

	ret = v4l2_async_register_subdev_sensor(&irs2381c->sd);
	if (ret < 0) {
		dev_err_probe(irs2381c->dev, ret,
			      "failed to register V4L2 sensor subdev\n");
		goto disable_pm;
	}

	pm_runtime_set_autosuspend_delay(irs2381c->dev, 1000);
	pm_runtime_use_autosuspend(irs2381c->dev);
	pm_runtime_idle(irs2381c->dev);

	return 0;

disable_pm:
	v4l2_subdev_cleanup(&irs2381c->sd);
	pm_runtime_disable(irs2381c->dev);
	pm_runtime_set_suspended(irs2381c->dev);
cleanup_entity:
	media_entity_cleanup(&irs2381c->sd.entity);
free_ctrls:
	v4l2_ctrl_handler_free(irs2381c->sd.ctrl_handler);
power_off:
	irs2381c_power_off(irs2381c->dev);
	return ret;
}

static void irs2381c_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct irs2381c *irs2381c = to_irs2381c(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(irs2381c->dev);
	if (!pm_runtime_status_suspended(irs2381c->dev))
		irs2381c_power_off(irs2381c->dev);
	pm_runtime_set_suspended(irs2381c->dev);
}

static int __maybe_unused irs2381c_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct irs2381c *irs2381c = to_irs2381c(sd);

	bool streaming;

	mutex_lock(sd->state_lock);
	streaming = irs2381c->streaming;
	mutex_unlock(sd->state_lock);
	if (streaming)
		return -EBUSY;

	return pm_runtime_force_suspend(dev);
}

static const struct dev_pm_ops irs2381c_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(irs2381c_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(irs2381c_power_off, irs2381c_power_on, NULL)
};

static const struct of_device_id irs2381c_of_match[] = {
	{ .compatible = "infineon,irs2381c" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, irs2381c_of_match);

static struct i2c_driver irs2381c_i2c_driver = {
	.driver = {
		.name = "irs2381c",
		.pm = &irs2381c_pm_ops,
		.of_match_table = irs2381c_of_match,
	},
	.probe = irs2381c_probe,
	.remove = irs2381c_remove,
};

module_i2c_driver(irs2381c_i2c_driver);

MODULE_DESCRIPTION("Infineon REAL3 IRS2381C ToF sensor driver");
MODULE_AUTHOR("The LM-G820N Linux contributors");
MODULE_LICENSE("GPL");
