// SPDX-License-Identifier: GPL-2.0-only
#include "iris_core.h"
#include "iris_ctrls.h"
#include "iris_platform_common.h"
#include "iris_resources.h"
#include "iris_hfi_gen1.h"
#include "iris_hfi_gen1_defines.h"
#include "iris_vpu_buffer.h"
#include "iris_vpu_common.h"
#include "iris_instance.h"

#include "iris_platform_sm8150.h"

static const struct iris_firmware_desc iris_vpu10_desc = {
	.firmware_data = &iris_hfi_sm8150_data,
	.get_vpu_buffer_size = iris_hfi_gen1_get_buffer_size,
	.get_vpu_buffer_count = iris_hfi_gen1_get_buffer_count,
	.fwname = "qcom/sm8150/venus.mdt",
};

static const u32 iris_fmts_vpu1_dec[] = {
	[IRIS_FMT_H264] = V4L2_PIX_FMT_H264,
	[IRIS_FMT_HEVC] = V4L2_PIX_FMT_HEVC,
	[IRIS_FMT_VP9] = V4L2_PIX_FMT_VP9,
};

static struct platform_inst_caps platform_inst_cap_vpu1 = {
	.min_frame_width = 128,
	.max_frame_width = 8192,
	.min_frame_height = 128,
	.max_frame_height = 8192,
	.max_mbpf = 138240,
	.mb_cycles_vsp = 10,
	.mb_cycles_vpp = 200,
	.mb_cycles_fw = 760000,
	.mb_cycles_fw_vpp = 166667,
	.max_frame_rate = MAXIMUM_FPS,
	.max_operating_rate = MAXIMUM_FPS,
};

static const struct icc_info iris_icc_info_vpu1[] = {
	{ "cpu-cfg",    1000, 1000     },
	{ "video-mem",  1000, 6533000 },
};

static const char * const iris_clk_reset_table_vpu1[] = {
	"bus", "core", "vcodec0", "vcodec1",
};

static const char * const iris_pmdomain_table_vpu1[] = {
	"venus", "vcodec0", "vcodec1",
};

static const struct tz_cp_config tz_cp_config_vpu1[] = {
	{
		.cp_start = 0,
		.cp_size = 0x25800000,
		.cp_nonpixel_start = 0x01000000,
		.cp_nonpixel_size = 0x24800000,
	},
};

const struct iris_platform_data sm8150_data = {
	.firmware_desc_gen1 = &iris_vpu10_desc,
	.vpu_ops = &iris_vpu1_ops,
	.icc_tbl = iris_icc_info_vpu1,
	.icc_tbl_size = ARRAY_SIZE(iris_icc_info_vpu1),
	.clk_rst_tbl = iris_clk_reset_table_vpu1,
	.clk_rst_tbl_size = ARRAY_SIZE(iris_clk_reset_table_vpu1),
	.bw_tbl_dec = sm8150_bw_table_dec,
	.bw_tbl_dec_size = ARRAY_SIZE(sm8150_bw_table_dec),
	.pmdomain_tbl = iris_pmdomain_table_vpu1,
	.pmdomain_tbl_size = ARRAY_SIZE(iris_pmdomain_table_vpu1),
	.opp_pd_tbl = sm8150_opp_pd_table,
	.opp_pd_tbl_size = ARRAY_SIZE(sm8150_opp_pd_table),
	.clk_tbl = sm8150_clk_table,
	.clk_tbl_size = ARRAY_SIZE(sm8150_clk_table),
	.opp_clk_tbl = sm8150_opp_clk_table,
	.dma_mask = 0xe0000000 - 1,
	.inst_iris_fmts = iris_fmts_vpu1_dec,
	.inst_iris_fmts_size = ARRAY_SIZE(iris_fmts_vpu1_dec),
	.inst_caps = &platform_inst_cap_vpu1,
	.tz_cp_config_data = tz_cp_config_vpu1,
	.tz_cp_config_data_size = ARRAY_SIZE(tz_cp_config_vpu1),
	.num_vpp_pipe = 2,
	.max_session_count = 16,
	.max_core_mbpf = ((4096 * 2176) / 256) * 2,
	.max_core_mbps = 3916800,
};
