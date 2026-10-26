// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

// A slot's built-in mask (layout filters[].builtinMask): a shape drawn
// at the slot container's own size, never a fixed image, since OBS's image mask stretches its
// image to the target and a circle drawn once would turn oval in a 16:9 slot. OBS (plugin) and
// the worker both write it as a PNG and hand its path to mask_filter_v2.
//
// Header-only, the standard library only.
//   circle      the largest centred circle (diameter: the shorter side)
//   ellipse     fills the box
//   rounded-N   corner radius N% of the shorter side
// Inside is white and opaque, outside transparent black, with a one-pixel soft edge: the same
// mask for the image mask's alpha types and its colour ones.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace slot_mask {

// The corner radius as a fraction of the shorter side, or -1 for circle/ellipse, -2 unknown.
inline double roundedFraction(const std::string &shape)
{
  if (shape == "rounded-4") return 0.04;
  if (shape == "rounded-8") return 0.08;
  if (shape == "rounded-16") return 0.16;
  if (shape == "circle" || shape == "ellipse") return -1;
  return -2;
}

inline bool known(const std::string &shape) { return roundedFraction(shape) > -2; }

// How much of pixel (x, y) is inside the shape, 0 to 1.
inline double coverage(const std::string &shape, int width, int height, int x, int y)
{
  const double px = x + 0.5, py = y + 0.5, cx = width / 2.0, cy = height / 2.0;
  double distance = 0; // signed: negative inside, in pixels
  if (shape == "circle") {
    distance = std::hypot(px - cx, py - cy) - std::min(width, height) / 2.0;
  } else if (shape == "ellipse") {
    // The ellipse's level set scaled by its smaller radius: within a fraction of a pixel of the
    // true distance at the edge, where only the soft edge depends on it.
    const double nx = (px - cx) / cx, ny = (py - cy) / cy;
    distance = (std::sqrt(nx * nx + ny * ny) - 1.0) * std::min(cx, cy);
  } else {
    const double radius = roundedFraction(shape) * std::min(width, height);
    const double qx = std::abs(px - cx) - (cx - radius), qy = std::abs(py - cy) - (cy - radius);
    distance = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - radius;
  }
  return std::clamp(0.5 - distance, 0.0, 1.0);
}

inline uint32_t crc32(const std::string &data, size_t from)
{
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = from; i < data.size(); ++i) c = table[(c ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline void put32(std::string &out, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

inline void chunk(std::string &png, const char *type, const std::string &data)
{
  put32(png, static_cast<uint32_t>(data.size()));
  const size_t start = png.size();
  png.append(type, 4);
  png += data;
  put32(png, crc32(png, start));
}

// The shape as a PNG file's bytes; empty for an unknown shape or a size outside 1-8192. The
// image data is stored, not compressed (zlib's stored blocks): no zlib needed, and a mask is
// written once per shape and size.
inline std::string png(const std::string &shape, int width, int height)
{
  if (!known(shape) || width < 1 || height < 1 || width > 8192 || height > 8192) return {};
  std::string raw;
  raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 4 + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0); // filter: none
    for (int x = 0; x < width; ++x) {
      const char value = static_cast<char>(std::lround(coverage(shape, width, height, x, y) * 255.0));
      raw.append(4, value); // premultiplied white: r = g = b = a
    }
  }
  std::string zlib{"\x78\x01", 2};
  for (size_t at = 0; at < raw.size() || at == 0;) {
    const size_t length = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + length == raw.size();
    zlib.push_back(last ? 1 : 0);
    zlib.push_back(static_cast<char>(length & 0xFF));
    zlib.push_back(static_cast<char>(length >> 8));
    zlib.push_back(static_cast<char>(~length & 0xFF));
    zlib.push_back(static_cast<char>((~length >> 8) & 0xFF));
    zlib.append(raw, at, length);
    at += length;
    if (last) break;
  }
  uint32_t a = 1, b = 0; // Adler-32
  for (unsigned char c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put32(zlib, (b << 16) | a);

  std::string header;
  put32(header, static_cast<uint32_t>(width));
  put32(header, static_cast<uint32_t>(height));
  header += std::string{"\x08\x06\x00\x00\x00", 5}; // 8-bit RGBA, no interlace
  std::string out{"\x89PNG\r\n\x1a\n", 8};
  chunk(out, "IHDR", header);
  chunk(out, "IDAT", zlib);
  chunk(out, "IEND", {});
  return out;
}

} // namespace slot_mask
