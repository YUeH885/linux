// SPDX-License-Identifier: GPL-2.0-only
#include "iris_core.h"
#include "iris_hfi_gen1.h"

#define HFI_PROPERTY_CAPS_FORMATS	0x1004
#define HFI_PROPERTY_CAPS_PROFILES	0x1006
#define HFI_PROPERTY_CAPS_RANGES		0x1007
#define HFI_PROPERTY_CAPS_PROPERTIES	0x1008
#define HFI_PROPERTY_CAPS_CODECS		0x1009
#define HFI_PROPERTY_CAPS_NAL_FORMATS	0x100a
#define HFI_PROPERTY_CAPS_NAL_SELECT	0x100b
#define HFI_PROPERTY_CAPS_CODEC_MASK	0x100e
#define HFI_PROPERTY_CAPS_MAX_SESSIONS	0x1010
#define HFI_PROPERTY_CAPS_TME_VERSION	0x1016

#define HFI_CAPABILITY_FRAME_WIDTH	1
#define HFI_CAPABILITY_FRAME_HEIGHT	2
#define HFI_CAPABILITY_MBPF	3
#define HFI_CAPABILITY_MBPS	4
#define HFI_CAPABILITY_FRAMERATE	5
#define HFI_CAPABILITY_BFRAME		9

static bool iris_hfi_caps_match(struct iris_codec_caps *caps, u32 codecs, u32 domain)
{
	return caps->hfi_codec & codecs && caps->domain == domain;
}

static void iris_hfi_set_cap_range(struct platform_inst_caps *caps, const u32 *range)
{
	switch (range[0]) {
	case HFI_CAPABILITY_FRAME_WIDTH:
		caps->min_frame_width = range[1];
		caps->max_frame_width = range[2];
		caps->frame_width_step = range[3];
		break;
	case HFI_CAPABILITY_FRAME_HEIGHT:
		caps->min_frame_height = range[1];
		caps->max_frame_height = range[2];
		caps->frame_height_step = range[3];
		break;
	case HFI_CAPABILITY_MBPF:
		caps->max_mbpf = range[2];
		break;
	case HFI_CAPABILITY_MBPS:
		caps->max_mbps = range[2];
		break;
	case HFI_CAPABILITY_BFRAME:
		caps->max_b_frames = range[2];
		break;
	case HFI_CAPABILITY_FRAMERATE:
		caps->max_frame_rate = range[2];
		caps->max_operating_rate = range[2];
		break;
	}
}

int iris_hfi_gen1_parse_caps(struct iris_core *core, const struct hfi_msg_sys_init_done_pkt *pkt)
{
	static const u32 codecs[] = {HFI_VIDEO_CODEC_H264, HFI_VIDEO_CODEC_HEVC,
		HFI_VIDEO_CODEC_VP9, HFI_VIDEO_CODEC_H264, HFI_VIDEO_CODEC_HEVC};
	static const u32 formats[] = {V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC,
		V4L2_PIX_FMT_VP9, V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC};
	struct iris_codec_caps caps[IRIS_CODEC_CAP_COUNT] = {};
	const u32 *data = pkt->data;
	u32 words;
	u32 codec_mask = 0, domain = 0, decode_mask = 0, encode_mask = 0;
	u32 pos = 0, property, count, size, i, j, k;

	if (pkt->hdr.size < sizeof(*pkt) ||
	    (pkt->hdr.size - sizeof(*pkt)) % sizeof(u32))
		return -EBADMSG;
	words = (pkt->hdr.size - sizeof(*pkt)) / sizeof(u32);
	for (i = 0; i < ARRAY_SIZE(caps); i++) {
		caps[i].hfi_codec = codecs[i];
		caps[i].codec = formats[i];
		caps[i].domain = i < 3 ? HFI_SESSION_TYPE_DEC : HFI_SESSION_TYPE_ENC;
		caps[i].limits = *core->iris_platform_data->inst_caps;
	}

	for (i = 0; i < pkt->num_properties; i++) {
		if (pos >= words)
			return -EBADMSG;
		property = data[pos++];
		switch (property) {
		case HFI_PROPERTY_CAPS_CODECS:
		case HFI_PROPERTY_CAPS_CODEC_MASK:
			if (words - pos < 2)
				return -EBADMSG;
			if (property == HFI_PROPERTY_CAPS_CODECS) {
				decode_mask = data[pos];
				encode_mask = data[pos + 1];
			} else {
				codec_mask = data[pos];
				domain = data[pos + 1];
			}
			pos += 2;
			break;
		case HFI_PROPERTY_CAPS_RANGES:
		case HFI_PROPERTY_CAPS_PROFILES:
		case HFI_PROPERTY_CAPS_PROPERTIES:
			if (pos >= words)
				return -EBADMSG;
			count = data[pos++];
			size = property == HFI_PROPERTY_CAPS_RANGES ? 4 :
				property == HFI_PROPERTY_CAPS_PROFILES ? 2 : 1;
			if (count > (words - pos) / size)
				return -EBADMSG;
			for (j = 0; j < count; j++) {
				const u32 *entry = &data[pos + j * size];

				if (property == HFI_PROPERTY_CAPS_RANGES &&
				    (entry[1] > entry[2] || !entry[3]))
					return -EBADMSG;
				for (k = 0; k < ARRAY_SIZE(caps); k++) {
					if (!iris_hfi_caps_match(&caps[k], codec_mask, domain))
						continue;
					if (property == HFI_PROPERTY_CAPS_RANGES)
						iris_hfi_set_cap_range(&caps[k].limits, entry);
					else if (property == HFI_PROPERTY_CAPS_PROFILES)
						caps[k].profiles |= entry[0];
				}
			}
			pos += count * size;
			break;
		case HFI_PROPERTY_CAPS_FORMATS:
			if (words - pos < 2)
				return -EBADMSG;
			pos++;
			count = data[pos++];
			for (j = 0; j < count; j++) {
				if (words - pos < 2)
					return -EBADMSG;
				pos++;
				size = data[pos++];
				if (size > (words - pos) / 4)
					return -EBADMSG;
				pos += size * 4;
			}
			break;
		case HFI_PROPERTY_CAPS_MAX_SESSIONS:
		case HFI_PROPERTY_CAPS_NAL_FORMATS:
		case HFI_PROPERTY_CAPS_NAL_SELECT:
		case HFI_PROPERTY_CAPS_TME_VERSION:
			if (pos >= words)
				return -EBADMSG;
			pos++;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	if (pos != words)
		return -EBADMSG;
	for (i = 0; i < ARRAY_SIZE(caps); i++) {
		u32 supported = caps[i].domain == HFI_SESSION_TYPE_DEC ? decode_mask : encode_mask;

		if (!(supported & caps[i].hfi_codec) || !caps[i].limits.max_mbps ||
		    !caps[i].profiles)
			return -EOPNOTSUPP;
	}
	memcpy(core->codec_caps, caps, sizeof(caps));
	return 0;
}

void iris_hfi_gen1_init_inst_caps(struct iris_inst *inst)
{
	static const u32 h264_profiles[] = {
		[V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE] = HFI_H264_PROFILE_BASELINE,
		[V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE] =
			HFI_H264_PROFILE_CONSTRAINED_BASE,
		[V4L2_MPEG_VIDEO_H264_PROFILE_MAIN] = HFI_H264_PROFILE_MAIN,
		[V4L2_MPEG_VIDEO_H264_PROFILE_HIGH] = HFI_H264_PROFILE_HIGH,
		[V4L2_MPEG_VIDEO_H264_PROFILE_STEREO_HIGH] = HFI_H264_PROFILE_STEREO_HIGH,
		[V4L2_MPEG_VIDEO_H264_PROFILE_MULTIVIEW_HIGH] = HFI_H264_PROFILE_MULTIVIEW_HIGH,
		[V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH] = HFI_H264_PROFILE_CONSTRAINED_HIGH,
	};
	static const u32 hevc_profiles[] = {
		[V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN] = HFI_HEVC_PROFILE_MAIN,
		[V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE] = HFI_HEVC_PROFILE_MAIN_STILL_PIC,
		[V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10] = HFI_HEVC_PROFILE_MAIN10,
	};
	unsigned int i, j;

	if (!inst->core->iris_firmware_data->hfi_4xx)
		return;

	for (i = 0; i < ARRAY_SIZE(inst->core->codec_caps); i++) {
		struct iris_codec_caps *caps = &inst->core->codec_caps[i];
		struct platform_inst_fw_cap *cap;
		const u32 *profiles;
		u32 count;

		if (caps->domain != inst->domain)
			continue;
		if (inst->domain == ENCODER)
			inst->fw_caps[B_FRAME].max = caps->limits.max_b_frames;
		if (caps->codec == V4L2_PIX_FMT_VP9) {
			inst->fw_caps[PROFILE_VP9].step_or_mask = caps->profiles;
			continue;
		}
		if (caps->codec == V4L2_PIX_FMT_H264) {
			cap = &inst->fw_caps[PROFILE_H264];
			profiles = h264_profiles;
			count = ARRAY_SIZE(h264_profiles);
		} else {
			cap = &inst->fw_caps[PROFILE_HEVC];
			profiles = hevc_profiles;
			count = ARRAY_SIZE(hevc_profiles);
		}
		cap->step_or_mask = 0;
		cap->max = count - 1;
		for (j = 0; j < count; j++)
			if (caps->profiles & profiles[j])
				cap->step_or_mask |= BIT(j);
	}
}
