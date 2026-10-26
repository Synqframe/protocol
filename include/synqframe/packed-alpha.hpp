// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

// Packed alpha: a bundle's transparency carried through H.264, which has none (bundle
// background mode "alpha"). The plugin sends a frame twice
// the picture's height: the picture on top, drawn over black (so its colour is premultiplied by
// its alpha), and its alpha below as grey. The worker unpacks it to the picture with its alpha.
// Exact edges, unlike a chroma matte, which left a rim round soft masks.
//
// Header-only and libobs-only, for both the plugin (packs) and the worker (unpacks): each
// registers the filter it uses (registerPackFilter, registerUnpackFilter).
#include <obs-module.h>

#include <cstdint>

namespace packed_alpha {

// Draws its target's alpha as grey (plugin).
inline constexpr const char *PackFilterId = "synqframe_alpha_luma";
// Turns a packed frame back into its picture with alpha, at half the frame's height (worker).
inline constexpr const char *UnpackFilterId = "synqframe_alpha_unpack";

inline constexpr const char *PackEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v_in)
{
	VertData v_out;
	v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	v_out.uv = v_in.uv;
	return v_out;
}
float4 PSAlphaLuma(VertData v_in) : TARGET
{
	float a = image.Sample(textureSampler, v_in.uv).a;
	return float4(a, a, a, 1.0);
}
technique Draw { pass { vertex_shader = VSDefault(v_in); pixel_shader = PSAlphaLuma(v_in); } }
)";

// The colour half's rgb is premultiplied (drawn over black): divided back by alpha. Alpha near 0
// and 1 is clamped, since H.264 noise never quite reaches either.
inline constexpr const char *UnpackEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertData VSDefault(VertData v_in)
{
	VertData v_out;
	v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
	v_out.uv = v_in.uv;
	return v_out;
}
float4 PSUnpack(VertData v_in) : TARGET
{
	float3 rgb = image.Sample(textureSampler, float2(v_in.uv.x, v_in.uv.y * 0.5)).rgb;
	float a = image.Sample(textureSampler, float2(v_in.uv.x, 0.5 + v_in.uv.y * 0.5)).r;
	a = saturate((a - 0.02) / 0.96);
	return float4(saturate(rgb / max(a, 0.004)), a);
}
technique Draw { pass { vertex_shader = VSDefault(v_in); pixel_shader = PSUnpack(v_in); } }
)";

struct Filter {
  obs_source_t *self = nullptr;
  gs_effect_t *effect = nullptr;
  bool unpack = false;
};

inline void *createFilter(obs_source_t *source, bool unpack)
{
  auto *filter = new Filter{source, nullptr, unpack};
  obs_enter_graphics();
  char *errors = nullptr;
  filter->effect = gs_effect_create(unpack ? UnpackEffect : PackEffect, unpack ? "packed-alpha-unpack" : "packed-alpha-pack",
                                    &errors);
  obs_leave_graphics();
  if (!filter->effect)
    blog(LOG_WARNING, "[Synqframe] packed alpha: the %s shader did not compile: %s", unpack ? "unpack" : "pack",
         errors ? errors : "(no message)");
  bfree(errors);
  return filter;
}

inline void *createPack(obs_data_t *, obs_source_t *source) { return createFilter(source, false); }
inline void *createUnpack(obs_data_t *, obs_source_t *source) { return createFilter(source, true); }

inline void destroyFilter(void *data)
{
  auto *filter = static_cast<Filter *>(data);
  obs_enter_graphics();
  if (filter->effect) gs_effect_destroy(filter->effect);
  obs_leave_graphics();
  delete filter;
}

inline uint32_t targetWidth(Filter *filter)
{
  obs_source_t *target = obs_filter_get_target(filter->self);
  return target ? obs_source_get_base_width(target) : 0;
}

inline uint32_t targetHeight(Filter *filter)
{
  obs_source_t *target = obs_filter_get_target(filter->self);
  const uint32_t height = target ? obs_source_get_base_height(target) : 0;
  return filter->unpack ? height / 2 : height;
}

inline void renderFilter(void *data, gs_effect_t *)
{
  auto *filter = static_cast<Filter *>(data);
  if (!filter->effect) {
    obs_source_skip_video_filter(filter->self);
    return;
  }
  if (!obs_source_process_filter_begin(filter->self, GS_RGBA, OBS_NO_DIRECT_RENDERING)) return;
  obs_source_process_filter_end(filter->self, filter->effect, targetWidth(filter), targetHeight(filter));
}

inline void registerFilter(bool unpack)
{
  obs_source_info info{};
  info.id = unpack ? UnpackFilterId : PackFilterId;
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED; // made by code, never offered in OBS's list
  info.get_name = [](void *) { return "Synqframe packed alpha"; };
  info.create = unpack ? createUnpack : createPack;
  info.destroy = destroyFilter;
  info.video_render = renderFilter;
  info.get_width = [](void *data) { return targetWidth(static_cast<Filter *>(data)); };
  info.get_height = [](void *data) { return targetHeight(static_cast<Filter *>(data)); };
  obs_register_source(&info);
}

// Registers the filter, then compiles its shader once and logs whether it built: a shader is
// translated per graphics API (Direct3D in OBS on Windows, OpenGL on the worker), and a failure
// would otherwise show only when a bundle with packed alpha arrives.
inline void registerAndCheck(bool unpack)
{
  registerFilter(unpack);
  obs_source_t *probe = obs_source_create_private(unpack ? UnpackFilterId : PackFilterId, "Packed alpha check", nullptr);
  const auto *filter = probe ? static_cast<const Filter *>(obs_obj_get_data(probe)) : nullptr;
  blog(filter && filter->effect ? LOG_INFO : LOG_WARNING, "[Synqframe] packed alpha: the %s shader %s",
       unpack ? "unpack" : "pack", filter && filter->effect ? "is ready" : "did not build");
  if (probe) obs_source_release(probe);
}

inline void registerPackFilter() { registerAndCheck(false); }
inline void registerUnpackFilter() { registerAndCheck(true); }

} // namespace packed_alpha
