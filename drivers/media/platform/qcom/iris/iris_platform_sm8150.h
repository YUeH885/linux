/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __IRIS_PLATFORM_SM8150_H__
#define __IRIS_PLATFORM_SM8150_H__

static const struct bw_info sm8150_bw_table_dec[] = {
	{ ((4096 * 2160) / 256) * 60, 2403000 },
	{ ((4096 * 2160) / 256) * 30, 1224000 },
	{ ((1920 * 1088) / 256) * 60,  812000 },
	{ ((1920 * 1088) / 256) * 30,  416000 },
};

static const char * const sm8150_opp_pd_table[] = { "mx", "mmcx" };

static const struct platform_clk_data sm8150_clk_table[] = {
	{ IRIS_AXIC_CLK,   "iface"        },
	{ IRIS_AXI_CLK,    "vcodec0_axi"  },
	{ IRIS_AXI1_CLK,   "vcodec1_axi"  },
	{ IRIS_CTRL_CLK,   "core"         },
	{ IRIS_AHB_CLK,    "ahb"          },
	{ IRIS_HW_CLK,     "vcodec0_core" },
	{ IRIS_CVP_HW_CLK, "vcodec1_core" },
};

static const char * const sm8150_opp_clk_table[] = {
	"vcodec0_core",
	NULL,
};

#endif
