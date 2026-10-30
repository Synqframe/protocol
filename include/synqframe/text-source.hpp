// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

// The text source as an OBS source, synqframe_text: a layout's text slot (sourceKind "text"),
// drawn by text-raster.hpp so the plugin's OBS and the worker show the same pixels (PinkFrog
// msg 1142). OBS's own text sources can't: they pick fonts by installed face name, and their
// outline and shadow have no width, colour or offset.
//
// Settings: the slot's text object as the layout sends it (content, size, color, align,
// outline {color, width} or null, shadow {color, x, y} or null, font {id, url, sha256}), and
// three the host sets:
//   width, height  the slot's box in canvas px: the source is exactly that size
//   font_path      the host's local copy of font.url; empty while it is still fetching
// The source checks the file against font.sha256 and draws nothing from one that doesn't match.
//
// Drawn off the graphics thread (a change of words or box redraws in the background) and shown
// once drawn. Header-only and libobs-only, like packed-alpha.hpp; the host links
// synqframe-protocol and calls registerTextSource() once.
#include <synqframe/sha256.hpp>
#include <synqframe/text-raster.hpp>

#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace text_source {

inline constexpr const char *Id = "synqframe_text";

struct Request {
  text_raster::Text text;
  int width = 0, height = 0;
  std::string fontPath, fontSha256;
};

struct Source {
  obs_source_t *self = nullptr;
  std::atomic<uint32_t> width{0}, height{0};

  std::mutex mutex;
  std::condition_variable wake;
  bool stopping = false;
  std::optional<Request> pending;          // the latest settings, not yet drawn
  std::optional<text_raster::Image> drawn; // drawn, not yet shown
  std::thread drawer;

  gs_texture_t *texture = nullptr; // the graphics thread's

  // The drawer's.
  std::string facePath;
  std::shared_ptr<const text_raster::Face> face;
};

inline text_raster::Rgba colorSetting(obs_data_t *data, const char *name, text_raster::Rgba fallback)
{
  text_raster::Rgba color = fallback;
  text_raster::parseColor(obs_data_get_string(data, name), color);
  return color;
}

inline Request requestFrom(obs_data_t *settings)
{
  Request request;
  text_raster::Text &text = request.text;
  text.content = obs_data_get_string(settings, "content");
  text.size = static_cast<double>(obs_data_get_int(settings, "size"));
  text.color = colorSetting(settings, "color", {255, 255, 255, 255});
  text.align = obs_data_get_string(settings, "align");
  if (obs_data_t *outline = obs_data_get_obj(settings, "outline")) {
    const int width = static_cast<int>(obs_data_get_int(outline, "width"));
    if (width > 0) text.outline = text_raster::Outline{colorSetting(outline, "color", {0, 0, 0, 255}), width};
    obs_data_release(outline);
  }
  if (obs_data_t *shadow = obs_data_get_obj(settings, "shadow")) {
    text.shadow = text_raster::Shadow{colorSetting(shadow, "color", {0, 0, 0, 255}), static_cast<int>(obs_data_get_int(shadow, "x")),
                                      static_cast<int>(obs_data_get_int(shadow, "y"))};
    obs_data_release(shadow);
  }
  if (obs_data_t *font = obs_data_get_obj(settings, "font")) {
    request.fontSha256 = obs_data_get_string(font, "sha256");
    obs_data_release(font);
  }
  request.fontPath = obs_data_get_string(settings, "font_path");
  request.width = static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, "width"), 0, text_raster::MaxSide));
  request.height = static_cast<int>(std::clamp<long long>(obs_data_get_int(settings, "height"), 0, text_raster::MaxSide));
  return request;
}

// The face for `request`, read again only when its file changes. Null (and logged once per
// file) when the file is unreadable, isn't a font, or isn't the one named.
inline std::shared_ptr<const text_raster::Face> faceFor(Source &source, const Request &request)
{
  if (request.fontPath == source.facePath) return source.face;
  source.facePath = request.fontPath;
  source.face = nullptr;
  if (request.fontPath.empty()) return nullptr;
  std::ifstream in(request.fontPath, std::ios::binary);
  std::stringstream bytes;
  bytes << in.rdbuf();
  const std::string file = bytes.str();
  const char *name = obs_source_get_name(source.self);
  if (file.empty()) {
    blog(LOG_WARNING, "[Synqframe] text '%s': could not read its font %s", name, request.fontPath.c_str());
    return nullptr;
  }
  if (!request.fontSha256.empty() && synqframe::sha256Hex(file) != request.fontSha256) {
    blog(LOG_WARNING, "[Synqframe] text '%s': its font %s is not the one the layout names (sha256 differs)", name,
         request.fontPath.c_str());
    return nullptr;
  }
  std::string error;
  source.face = text_raster::Face::load(file, &error);
  if (!source.face) blog(LOG_WARNING, "[Synqframe] text '%s': its font %s: %s", name, request.fontPath.c_str(), error.c_str());
  return source.face;
}

inline void drawLoop(Source *source)
{
  std::unique_lock lock(source->mutex);
  while (true) {
    source->wake.wait(lock, [&] { return source->stopping || source->pending; });
    if (source->stopping) return;
    const Request request = std::move(*source->pending);
    source->pending.reset();
    lock.unlock();
    text_raster::Image image;
    if (const auto face = faceFor(*source, request)) image = text_raster::draw(*face, request.text, request.width, request.height);
    lock.lock();
    // Superseded while drawing: the next pass draws the newer settings instead.
    if (!source->pending) source->drawn = std::move(image);
  }
}

inline void updateSource(void *data, obs_data_t *settings)
{
  auto *source = static_cast<Source *>(data);
  Request request = requestFrom(settings);
  source->width = static_cast<uint32_t>(request.width);
  source->height = static_cast<uint32_t>(request.height);
  std::lock_guard lock(source->mutex);
  source->pending = std::move(request);
  source->wake.notify_one();
}

inline void *createSource(obs_data_t *settings, obs_source_t *self)
{
  auto *source = new Source;
  source->self = self;
  source->drawer = std::thread(drawLoop, source);
  updateSource(source, settings);
  return source;
}

inline void destroySource(void *data)
{
  auto *source = static_cast<Source *>(data);
  {
    std::lock_guard lock(source->mutex);
    source->stopping = true;
    source->wake.notify_one();
  }
  source->drawer.join();
  if (source->texture) {
    obs_enter_graphics();
    gs_texture_destroy(source->texture);
    obs_leave_graphics();
  }
  delete source;
}

inline void renderSource(void *data, gs_effect_t *)
{
  auto *source = static_cast<Source *>(data);
  std::optional<text_raster::Image> drawn;
  {
    std::lock_guard lock(source->mutex);
    drawn.swap(source->drawn);
  }
  if (drawn) {
    if (source->texture) gs_texture_destroy(source->texture);
    source->texture = nullptr;
    if (drawn->width > 0 && drawn->height > 0) {
      const uint8_t *pixels = drawn->rgba.data();
      source->texture = gs_texture_create(static_cast<uint32_t>(drawn->width), static_cast<uint32_t>(drawn->height), GS_RGBA, 1, &pixels, 0);
    }
  }
  if (source->texture) obs_source_draw(source->texture, 0, 0, 0, 0, false);
}

inline void sourceDefaults(obs_data_t *settings)
{
  obs_data_set_default_int(settings, "size", 48);
  obs_data_set_default_string(settings, "color", "#ffffff");
  obs_data_set_default_string(settings, "align", "left");
}

inline void registerTextSource()
{
  obs_source_info info{};
  info.id = Id;
  info.type = OBS_SOURCE_TYPE_INPUT;
  info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED; // made by the layout, never offered in OBS's list
  info.get_name = [](void *) { return "Synqframe text"; };
  info.create = createSource;
  info.destroy = destroySource;
  info.update = updateSource;
  info.get_defaults = sourceDefaults;
  info.video_render = renderSource;
  info.get_width = [](void *data) { return static_cast<Source *>(data)->width.load(); };
  info.get_height = [](void *data) { return static_cast<Source *>(data)->height.load(); };
  obs_register_source(&info);
}

} // namespace text_source
