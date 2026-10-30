// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

// A text source's pixels (layout sourceKind "text", schema $defs.text): one drawing for the OBS
// plugin, the compositor worker and the relay's drop screen, so all three show the same text
// (PinkFrog msg 1142). It follows the studio's editor (web components/canvas/text-slot.tsx),
// which draws the slot as CSS:
//   - the words in the font file at `size` px per em, in lines 1.2 x size apart
//     (line-height 1.2, the font's ascent and descent rounded to whole pixels, as Blink does);
//   - wrapped at the box width with whitespace kept (white-space: pre-wrap): a line breaks
//     after a run of spaces or tabs, after a hyphen, or between CJK characters; a word wider
//     than the box breaks between characters (overflow-wrap: break-word); spaces at a line's
//     end hang past the box rather than wrap; a final newline adds no line;
//   - the lines aligned left, center or right, the block centred vertically in the box
//     (overflowing it evenly when taller) and everything clipped to the box;
//   - an outline `width` px outside the glyphs (the editor's stroke of 2 x width, centred on the
//     glyph's edge, under the fill), with round joins;
//   - a hard shadow of the outlined glyphs, offset by (x, y) px, no blur.
// Pixels are canvas pixels: the box is the slot's own size on its canvas.
//
// Not drawn as a browser would: no ligatures or other substitutions (kerning, from GPOS or kern,
// is applied), no fallback font for characters the font lacks (they draw as its .notdef glyph),
// and no bidirectional reordering.
//
// The standard library only; the font is read with stb_truetype, built in (third_party/stb).
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace text_raster {

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  bool operator==(const Rgba &) const = default;
};

// "#rgb", "#rrggbb" or "#rrggbbaa", in either case. False, and `out` untouched, for anything else.
bool parseColor(const std::string &text, Rgba &out);

struct Outline {
  Rgba color;
  int width = 1; // px outside the glyphs, at most MaxOutline
};

struct Shadow {
  Rgba color;
  int x = 0, y = 0; // px, right and down
};

struct Text {
  std::string content; // UTF-8; "\n" (or "\r\n") ends a line
  double size = 48;    // px per em
  Rgba color{255, 255, 255, 255};
  std::string align = "left"; // "left", "center" or "right"
  std::optional<Outline> outline;
  std::optional<Shadow> shadow;
};

inline constexpr int MaxSide = 4096;     // a box past this on either side draws nothing (a 4K canvas fits)
inline constexpr int MaxOutline = 256;   // a wider outline is drawn at this width
inline constexpr int MaxShadowOffset = 1024;

// A font file, read once and drawn from by any number of threads.
class Face {
public:
  // Null, with `error` set when given, for bytes that aren't a TrueType or OpenType font.
  static std::shared_ptr<const Face> load(std::string bytes, std::string *error = nullptr);
  ~Face();
  struct Impl;
  const Impl &impl() const { return *impl_; }

private:
  explicit Face(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// One laid-out line: where its glyphs go, from the line's left edge.
struct Line {
  struct Glyph {
    int index = 0;  // the font's glyph
    double x = 0;   // pen position, px from the line's start
    bool ink = true; // false for spaces and tabs
  };
  std::u32string text;       // its characters, trailing spaces included
  std::vector<Glyph> glyphs; // one per character
  double width = 0;          // what the alignment measures: hanging spaces count only up to the box
};

// The lines `text` makes in a box `width` px wide.
std::vector<Line> layout(const Face &face, const Text &text, double width);

// Where line `index` of `count` has its baseline, px from the box's top.
double baseline(const Face &face, const Text &text, double height, int index, int count);

struct Image {
  int width = 0, height = 0;
  std::vector<uint8_t> rgba; // width x height, rows top to bottom, straight (not premultiplied) alpha
};

// `text` drawn in a box of `width` x `height` px. Empty (0 x 0) for a box outside 1-MaxSide.
Image draw(const Face &face, const Text &text, int width, int height);

} // namespace text_raster
