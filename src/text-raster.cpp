// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/text-raster.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>

// Built in privately (static), so a host that has its own copy doesn't clash with this one.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace text_raster {

struct Face::Impl {
  std::string bytes;
  stbtt_fontinfo info{};
  int ascent = 0, descent = 0; // font units, from hhea; descent positive
  // Character to glyph, filled as characters are first met.
  mutable std::mutex mutex;
  mutable std::unordered_map<char32_t, int> glyphs;

  int glyph(char32_t c) const
  {
    std::lock_guard lock(mutex);
    auto [it, added] = glyphs.try_emplace(c, 0);
    if (added) it->second = stbtt_FindGlyphIndex(&info, static_cast<int>(c));
    return it->second;
  }
};

Face::Face(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Face::~Face() = default;

std::shared_ptr<const Face> Face::load(std::string bytes, std::string *error)
{
  auto impl = std::make_unique<Impl>();
  impl->bytes = std::move(bytes);
  const auto *data = reinterpret_cast<const unsigned char *>(impl->bytes.data());
  const int offset = impl->bytes.size() >= 12 ? stbtt_GetFontOffsetForIndex(data, 0) : -1;
  if (offset < 0 || !stbtt_InitFont(&impl->info, data, offset)) {
    if (error) *error = "not a TrueType or OpenType font";
    return nullptr;
  }
  int lineGap = 0;
  stbtt_GetFontVMetrics(&impl->info, &impl->ascent, &impl->descent, &lineGap);
  impl->descent = -impl->descent;
  return std::shared_ptr<const Face>(new Face(std::move(impl)));
}

bool parseColor(const std::string &text, Rgba &out)
{
  if (text.empty() || text[0] != '#') return false;
  const std::string hex = text.substr(1);
  if (hex.size() != 3 && hex.size() != 6 && hex.size() != 8) return false;
  std::vector<int> digits;
  for (char c : hex) {
    if (c >= '0' && c <= '9') digits.push_back(c - '0');
    else if (c >= 'a' && c <= 'f') digits.push_back(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') digits.push_back(c - 'A' + 10);
    else return false;
  }
  const auto pair = [&](int at) { return static_cast<uint8_t>(digits[at] * 16 + digits[at + 1]); };
  if (hex.size() == 3) out = {static_cast<uint8_t>(digits[0] * 17), static_cast<uint8_t>(digits[1] * 17), static_cast<uint8_t>(digits[2] * 17), 255};
  else out = {pair(0), pair(2), pair(4), hex.size() == 8 ? pair(6) : uint8_t{255}};
  return true;
}

namespace {

// UTF-8 to characters, a bad byte as U+FFFD; "\r\n" and a lone "\r" become "\n".
std::u32string decode(const std::string &in)
{
  std::u32string out;
  for (size_t i = 0; i < in.size();) {
    const auto c = static_cast<unsigned char>(in[i]);
    int length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    char32_t value = length == 1 ? c : length == 2 ? c & 0x1F : length == 3 ? c & 0x0F : c & 0x07;
    bool good = length > 0 && i + length <= in.size();
    for (int k = 1; good && k < length; ++k) {
      const auto next = static_cast<unsigned char>(in[i + k]);
      good = (next & 0xC0) == 0x80;
      value = (value << 6) | (next & 0x3F);
    }
    if (!good) {
      out.push_back(char32_t{0xFFFD});
      ++i;
      continue;
    }
    i += length;
    if (value == U'\r') {
      if (i < in.size() && in[i] == '\n') ++i;
      value = U'\n';
    }
    out.push_back(value);
  }
  return out;
}

bool isSpace(char32_t c) { return c == U' ' || c == U'\t'; }

// Characters a line may break before or after (UAX #14 ID, roughly): CJK ideographs, kana,
// Hangul and fullwidth forms.
bool isCjk(char32_t c)
{
  return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
         (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

struct Metrics {
  const Face::Impl &face;
  float scale;
  double space, tabStop;

  Metrics(const Face::Impl &f, double size) : face(f), scale(stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(size)))
  {
    space = advance(face.glyph(U' '));
    tabStop = 8 * space; // CSS tab-size: 8
  }
  double advance(int glyph) const
  {
    int width = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&face.info, glyph, &width, &bearing);
    return width * scale;
  }
  double kern(int left, int right) const { return stbtt_GetGlyphKernAdvance(&face.info, left, right) * scale; }
  // A tab ends at the next stop at least half a space away.
  double tabAdvance(double x) const
  {
    if (tabStop <= 0) return 0;
    double next = (std::floor(x / tabStop) + 1) * tabStop;
    if (next - x < space / 2) next += tabStop;
    return next - x;
  }
};

// Whether a line may break after `at` in `text`.
bool breakAfter(const std::u32string &text, size_t at)
{
  const char32_t c = text[at];
  const char32_t next = at + 1 < text.size() ? text[at + 1] : 0;
  if (isSpace(c)) return !isSpace(next); // after a run of spaces, never inside it
  if (c == U'-') return at > 0 && !isSpace(text[at - 1]) && !(next >= U'0' && next <= U'9');
  return isCjk(c) || isCjk(next);
}

void layoutParagraph(const Metrics &m, const std::u32string &text, double width, std::vector<Line> &lines)
{
  size_t start = 0;
  do {
    std::vector<Line::Glyph> glyphs;
    std::vector<double> ends; // each character's right edge
    size_t end = text.size(), next = text.size(), lastBreak = std::u32string::npos;
    int previous = -1;
    double x = 0;
    for (size_t i = start; i < text.size(); ++i) {
      const char32_t c = text[i];
      const int glyph = m.face.glyph(c);
      double pen = x;
      if (previous >= 0 && c != U'\t') pen += m.kern(previous, glyph);
      const double advance = c == U'\t' ? m.tabAdvance(pen) : m.advance(glyph);
      // Spaces hang past the box; anything else that would cross it starts the next line, at
      // the last break before it, or here when this line has none (a word wider than the box).
      if (!isSpace(c) && i > start && pen + advance > width + 1e-6) {
        end = next = lastBreak != std::u32string::npos ? lastBreak : i;
        break;
      }
      glyphs.push_back({glyph, pen, !isSpace(c)});
      ends.push_back(pen + advance);
      x = pen + advance;
      previous = c == U'\t' ? -1 : glyph;
      if (breakAfter(text, i)) lastBreak = i + 1;
    }
    Line line;
    line.text = text.substr(start, end - start);
    glyphs.resize(end - start);
    ends.resize(end - start);
    double ink = 0;
    for (size_t k = 0; k < glyphs.size(); ++k)
      if (glyphs[k].ink) ink = ends[k];
    const double full = ends.empty() ? 0 : ends.back();
    line.width = std::max(ink, std::min(full, width));
    line.glyphs = std::move(glyphs);
    lines.push_back(std::move(line));
    start = next;
  } while (start < text.size());
}

// Squared distance to the nearest zero of f, in one dimension (Felzenszwalb and Huttenlocher).
// "Far" is a large finite value rather than infinity, which keeps the parabola sums finite.
constexpr double Far = 1e20;

void distance1d(const float *f, int n, int stride, float *out, std::vector<int> &v, std::vector<double> &z, std::vector<double> &g)
{
  for (int q = 0; q < n; ++q) g[q] = f[q * stride];
  const auto meet = [&](int q, int p) { return ((g[q] + double(q) * q) - (g[p] + double(p) * p)) / (2.0 * (q - p)); };
  int k = 0;
  v[0] = 0;
  z[0] = -std::numeric_limits<double>::infinity();
  z[1] = std::numeric_limits<double>::infinity();
  for (int q = 1; q < n; ++q) {
    double s = meet(q, v[k]);
    while (s <= z[k]) s = meet(q, v[--k]);
    ++k;
    v[k] = q;
    z[k] = s;
    z[k + 1] = std::numeric_limits<double>::infinity();
  }
  k = 0;
  for (int q = 0; q < n; ++q) {
    while (z[k + 1] < q) ++k;
    const int p = v[k];
    out[q * stride] = static_cast<float>(std::min(Far, double(q - p) * (q - p) + g[p]));
  }
}

// Each pixel's distance to the nearest pixel at least half covered, 0 for those.
std::vector<float> distances(const std::vector<float> &coverage, int width, int height)
{
  std::vector<float> d(coverage.size()), pass(coverage.size());
  for (size_t i = 0; i < coverage.size(); ++i) d[i] = coverage[i] >= 0.5f ? 0.0f : static_cast<float>(Far);
  const int n = std::max(width, height);
  std::vector<int> v(n);
  std::vector<double> z(n + 1), g(n);
  for (int x = 0; x < width; ++x) distance1d(d.data() + x, height, width, pass.data() + x, v, z, g);
  for (int y = 0; y < height; ++y) distance1d(pass.data() + static_cast<size_t>(y) * width, width, 1, d.data() + static_cast<size_t>(y) * width, v, z, g);
  for (float &value : d) value = std::sqrt(value);
  return d;
}

} // namespace

std::vector<Line> layout(const Face &face, const Text &text, double width)
{
  std::vector<Line> lines;
  if (text.size <= 0) return lines;
  const Metrics m(face.impl(), text.size);
  std::u32string all = decode(text.content);
  if (!all.empty() && all.back() == U'\n') all.pop_back(); // a final newline adds no line
  if (all.empty() && text.content.empty()) return lines;
  size_t from = 0;
  while (true) {
    const size_t newline = all.find(U'\n', from);
    layoutParagraph(m, all.substr(from, newline == std::u32string::npos ? std::u32string::npos : newline - from), width, lines);
    if (newline == std::u32string::npos) break;
    from = newline + 1;
  }
  return lines;
}

double baseline(const Face &face, const Text &text, double height, int index, int count)
{
  const Face::Impl &f = face.impl();
  const double scale = stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(text.size));
  const double ascent = std::round(f.ascent * scale), descent = std::round(f.descent * scale);
  const double lineHeight = 1.2 * text.size;
  const double top = (height - count * lineHeight) / 2;
  return top + index * lineHeight + (lineHeight - (ascent + descent)) / 2 + ascent;
}

Image draw(const Face &face, const Text &text, int width, int height)
{
  Image image;
  if (width < 1 || height < 1 || width > MaxSide || height > MaxSide) return image;
  image.width = width;
  image.height = height;
  image.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
  const std::vector<Line> lines = layout(face, text, width);
  if (lines.empty()) return image;

  const int outline = text.outline ? std::clamp(text.outline->width, 0, MaxOutline) : 0;
  const int shadowX = text.shadow ? std::clamp(text.shadow->x, -MaxShadowOffset, MaxShadowOffset) : 0;
  const int shadowY = text.shadow ? std::clamp(text.shadow->y, -MaxShadowOffset, MaxShadowOffset) : 0;
  // Drawn into a margin round the box, so glyphs just outside it still cast their outline and
  // shadow into it.
  const int pad = outline + std::max(std::abs(shadowX), std::abs(shadowY)) + 2;
  const int pw = width + 2 * pad, ph = height + 2 * pad;
  std::vector<float> fill(static_cast<size_t>(pw) * ph, 0.0f);

  const Face::Impl &f = face.impl();
  const float scale = stbtt_ScaleForMappingEmToPixels(&f.info, static_cast<float>(text.size));
  std::vector<unsigned char> bitmap;
  const int count = static_cast<int>(lines.size());
  for (int li = 0; li < count; ++li) {
    const Line &line = lines[li];
    const double y = baseline(face, text, height, li, count) + pad;
    if (y + 2 * text.size < 0 || y - 2 * text.size > ph) continue;
    const double left = pad + (text.align == "right" ? width - line.width : text.align == "center" ? (width - line.width) / 2 : 0.0);
    for (const Line::Glyph &glyph : line.glyphs) {
      if (!glyph.ink) continue;
      const double penX = left + glyph.x;
      const double ix = std::floor(penX), iy = std::floor(y);
      const float fx = static_cast<float>(penX - ix), fy = static_cast<float>(y - iy);
      int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
      stbtt_GetGlyphBitmapBoxSubpixel(&f.info, glyph.index, scale, scale, fx, fy, &x0, &y0, &x1, &y1);
      const int gw = x1 - x0, gh = y1 - y0;
      const int ox = static_cast<int>(ix) + x0, oy = static_cast<int>(iy) + y0;
      if (gw <= 0 || gh <= 0 || ox >= pw || oy >= ph || ox + gw <= 0 || oy + gh <= 0) continue;
      bitmap.assign(static_cast<size_t>(gw) * gh, 0);
      stbtt_MakeGlyphBitmapSubpixel(&f.info, bitmap.data(), gw, gh, gw, scale, scale, fx, fy, glyph.index);
      for (int by = std::max(0, -oy); by < gh && oy + by < ph; ++by)
        for (int bx = std::max(0, -ox); bx < gw && ox + bx < pw; ++bx) {
          float &cell = fill[static_cast<size_t>(oy + by) * pw + ox + bx];
          cell = std::min(1.0f, cell + bitmap[static_cast<size_t>(by) * gw + bx] / 255.0f);
        }
    }
  }

  // The outline: everything within `outline` px of the glyphs, its edge a pixel soft, the
  // glyphs' own soft edges included.
  std::vector<float> stroke;
  if (outline > 0) {
    stroke = distances(fill, pw, ph);
    for (size_t i = 0; i < stroke.size(); ++i) stroke[i] = std::max(fill[i], std::clamp(outline + 1.0f - stroke[i], 0.0f, 1.0f));
  }
  const std::vector<float> &shape = outline > 0 ? stroke : fill;

  const auto layer = [](const Rgba &color, float coverage, float out[4]) {
    // `color` at `coverage` over what's in `out` (premultiplied).
    const float a = coverage * color.a / 255.0f;
    out[0] = color.r * a + out[0] * (1 - a);
    out[1] = color.g * a + out[1] * (1 - a);
    out[2] = color.b * a + out[2] * (1 - a);
    out[3] = a + out[3] * (1 - a);
  };
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      const size_t at = static_cast<size_t>(y + pad) * pw + x + pad;
      float out[4] = {0, 0, 0, 0};
      if (text.shadow) {
        const int sx = x + pad - shadowX, sy = y + pad - shadowY;
        if (sx >= 0 && sy >= 0 && sx < pw && sy < ph) layer(text.shadow->color, shape[static_cast<size_t>(sy) * pw + sx], out);
      }
      if (outline > 0) layer(text.outline->color, stroke[at], out);
      layer(text.color, fill[at], out);
      if (out[3] <= 0) continue;
      uint8_t *pixel = &image.rgba[(static_cast<size_t>(y) * width + x) * 4];
      for (int c = 0; c < 3; ++c) pixel[c] = static_cast<uint8_t>(std::clamp(std::lround(out[c] / out[3]), 0L, 255L));
      pixel[3] = static_cast<uint8_t>(std::clamp(std::lround(out[3] * 255.0f), 0L, 255L));
    }
  return image;
}

} // namespace text_raster
