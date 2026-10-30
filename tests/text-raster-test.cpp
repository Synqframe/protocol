// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

// Checks the text source's layout and drawing against the studio editor's rules
// (text-raster.hpp), and SHA-256 against FIPS 180-4's examples. The font is Manrope 700, the
// platform's (tests/fonts, SIL OFL), its path passed as the first argument.
#include <synqframe/sha256.hpp>
#include <synqframe/text-raster.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace text_raster;

static int failures = 0;
#define CHECK(condition)                                                                              \
  do {                                                                                                \
    if (!(condition)) {                                                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                                \
      ++failures;                                                                                     \
    }                                                                                                 \
  } while (0)

static std::u32string u32(const char *ascii)
{
  std::u32string out;
  for (; *ascii; ++ascii) out.push_back(static_cast<unsigned char>(*ascii));
  return out;
}

static const uint8_t *at(const Image &image, int x, int y) { return &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4]; }

// The box round every pixel with any alpha: {left, top, right, bottom} (right and bottom
// exclusive), or all -1 when nothing is drawn.
struct Ink {
  int left = -1, top = -1, right = -1, bottom = -1;
};
static Ink ink(const Image &image, int minAlpha = 1)
{
  Ink box;
  for (int y = 0; y < image.height; ++y)
    for (int x = 0; x < image.width; ++x)
      if (at(image, x, y)[3] >= minAlpha) {
        if (box.left < 0 || x < box.left) box.left = x;
        if (box.top < 0) box.top = y;
        box.right = std::max(box.right, x + 1);
        box.bottom = y + 1;
      }
  return box;
}

static void sha256()
{
  CHECK(synqframe::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(synqframe::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the length no longer fits the first block.
  CHECK(synqframe::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(synqframe::sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

static void colors()
{
  Rgba c;
  CHECK(parseColor("#ff8000", c) && c == (Rgba{255, 128, 0, 255}));
  CHECK(parseColor("#FFF", c) && c == (Rgba{255, 255, 255, 255}));
  CHECK(parseColor("#11223344", c) && c == (Rgba{0x11, 0x22, 0x33, 0x44}));
  c = {1, 2, 3, 4};
  CHECK(!parseColor("ff8000", c) && !parseColor("#ff80", c) && !parseColor("#gg0000", c) && !parseColor("", c));
  CHECK(c == (Rgba{1, 2, 3, 4}));
}

static void wrapping(const Face &face)
{
  Text text;
  text.size = 40;

  text.content = "Hello world";
  std::vector<Line> lines = layout(face, text, 1000);
  CHECK(lines.size() == 1 && lines[0].text == u32("Hello world"));
  const double full = lines[0].width;
  CHECK(full > 150 && full < 300);

  // Too narrow for both words: breaks after the space, which hangs on the first line.
  lines = layout(face, text, full - 1);
  CHECK(lines.size() == 2);
  if (lines.size() == 2) {
    CHECK(lines[0].text == u32("Hello ") && lines[1].text == u32("world"));
    CHECK(lines[0].width <= full - 1); // the hanging space counts only up to the box
  }

  // Whitespace is kept: a run of spaces stays together and leads no line.
  text.content = "ab   cd";
  lines = layout(face, text, 1000);
  CHECK(lines.size() == 1 && lines[0].text == u32("ab   cd"));
  const double gap = layout(face, Text{"ab cd", 40}, 1000)[0].width;
  CHECK(lines[0].width > gap);

  // A word wider than the box breaks between characters, never leaving a line empty.
  text.content = "WWWWWWWWWW";
  lines = layout(face, text, 100);
  CHECK(lines.size() > 1);
  size_t characters = 0;
  for (const Line &line : lines) {
    CHECK(!line.text.empty() && line.width <= 100.0001);
    characters += line.text.size();
  }
  CHECK(characters == 10);
  // Even a box narrower than one character.
  lines = layout(face, text, 1);
  CHECK(lines.size() == 10);

  // Newlines: each starts a line, an empty one included; a final one adds none; \r\n is one.
  text.content = "a\n\nb\n";
  lines = layout(face, text, 1000);
  CHECK(lines.size() == 3 && lines[1].text.empty() && lines[1].width == 0);
  text.content = "a\r\nb";
  CHECK(layout(face, text, 1000).size() == 2);
  text.content = "";
  CHECK(layout(face, text, 1000).empty());

  // After a hyphen.
  text.content = "well-known";
  const double hyphenated = layout(face, text, 1000)[0].width;
  lines = layout(face, text, hyphenated - 1);
  CHECK(lines.size() == 2 && lines[0].text == u32("well-"));

  // Kerning applies: "AV" is narrower than its two letters' advances.
  text.content = "AV";
  const double pair = layout(face, text, 1000)[0].width;
  text.content = "A";
  const double a = layout(face, text, 1000)[0].width;
  text.content = "V";
  const double v = layout(face, text, 1000)[0].width;
  CHECK(pair < a + v - 0.5);
}

static void drawing(const Face &face)
{
  Text text;
  text.size = 100;
  text.content = "H";
  text.color = {255, 0, 0, 255};

  // Outside the box's limits: nothing.
  CHECK(draw(face, text, 0, 100).rgba.empty() && draw(face, text, MaxSide + 1, 10).rgba.empty());

  // Centred vertically: H's foot sits on the baseline, which for one line is
  // (h - 1.2 size) / 2 + (1.2 size - (ascent + descent)) / 2 + ascent.
  Image image = draw(face, text, 400, 300);
  CHECK(image.width == 400 && image.height == 300 && image.rgba.size() == 400u * 300 * 4);
  const double base = baseline(face, text, 300, 0, 1);
  Ink box = ink(image, 128);
  CHECK(std::abs(box.bottom - base) <= 1.0);
  // Manrope: ascent 2132, descent 600 of 2000 units; 107 + 30 px at 100 px.
  CHECK(std::abs(base - ((300 - 120) / 2.0 + (120 - 137) / 2.0 + 107)) < 0.01);
  // Fill colour, opaque inside the stem.
  const uint8_t *inside = at(image, box.left + 3, (box.top + box.bottom) / 2);
  CHECK(inside[0] == 255 && inside[1] == 0 && inside[2] == 0 && inside[3] == 255);

  // Alignment: left from 0, right to the box's edge, center in between.
  text.align = "left";
  const Ink left = ink(draw(face, text, 400, 300));
  text.align = "right";
  const Ink right = ink(draw(face, text, 400, 300));
  text.align = "center";
  const Ink center = ink(draw(face, text, 400, 300));
  CHECK(left.left >= 0 && left.left < 20);
  CHECK(right.right <= 400 && right.right > 380);
  CHECK(std::abs((center.left + center.right) / 2.0 - 200) <= 2);
  CHECK(std::abs((right.left - left.left) - (400 - layout(face, text, 400)[0].width)) <= 1);

  // Two lines sit 1.2 x size apart and stay centred.
  text.align = "left";
  text.content = "H\nH";
  const Ink two = ink(draw(face, text, 400, 400), 128);
  CHECK(std::abs(baseline(face, text, 400, 1, 2) - baseline(face, text, 400, 0, 2) - 120) < 1e-9);
  CHECK(std::abs(two.bottom - baseline(face, text, 400, 1, 2)) <= 1.0);
  CHECK(std::abs((400 - two.bottom) - two.top) <= 30); // roughly even above and below

  // Taller than the box: overflows evenly, clipped, and the box still holds its middle.
  text.content = "H\nH\nH\nH\nH";
  image = draw(face, text, 200, 150);
  CHECK(image.width == 200 && image.height == 150);
  box = ink(image, 128);
  CHECK(baseline(face, text, 150, 0, 5) < 0 && baseline(face, text, 150, 4, 5) > 150);
  CHECK(std::abs(box.bottom - baseline(face, text, 150, 2, 5)) <= 1.0); // only the middle line shows

  // Outline: `width` px of its colour outside the glyph, the glyph's own colour inside.
  text.content = "H";
  const Image plain = draw(face, text, 400, 300);
  const Ink bare = ink(plain, 128);
  text.outline = Outline{{0, 0, 255, 255}, 6};
  image = draw(face, text, 400, 300);
  const Ink outlined = ink(image, 128);
  CHECK(std::abs((bare.left - outlined.left) - 6) <= 1 && std::abs((outlined.right - bare.right) - 6) <= 1);
  CHECK(std::abs((outlined.bottom - bare.bottom) - 6) <= 1);
  const uint8_t *edge = at(image, bare.left - 3, (bare.top + bare.bottom) / 2);
  CHECK(edge[0] == 0 && edge[2] == 255 && edge[3] == 255);
  const uint8_t *stem = at(image, bare.left + 3, (bare.top + bare.bottom) / 2);
  CHECK(stem[0] == 255 && stem[2] == 0);

  // Shadow: the outlined glyph again, offset, under it, with no blur.
  text.shadow = Shadow{{0, 255, 0, 255}, 20, 10};
  image = draw(face, text, 400, 300);
  const Ink shadowed = ink(image, 128);
  CHECK(std::abs((shadowed.right - outlined.right) - 20) <= 1 && std::abs((shadowed.bottom - outlined.bottom) - 10) <= 1);
  const uint8_t *cast = at(image, outlined.right + 10, (outlined.top + outlined.bottom) / 2 + 10);
  CHECK(cast[1] == 255 && cast[0] == 0 && cast[3] == 255);
  // No blur: every shadow pixel well inside it is fully opaque, and one past its edge is clear.
  CHECK(at(image, std::min(399, shadowed.right + 1), (outlined.top + outlined.bottom) / 2 + 10)[3] == 0);
  // The glyph is still on top.
  CHECK(at(image, bare.left + 3, (bare.top + bare.bottom) / 2)[0] == 255);

  // A translucent colour stays translucent, in straight alpha.
  text.outline.reset();
  text.shadow.reset();
  text.color = {255, 255, 255, 128};
  image = draw(face, text, 400, 300);
  const uint8_t *half = at(image, bare.left + 3, (bare.top + bare.bottom) / 2);
  CHECK(half[0] == 255 && half[1] == 255 && half[2] == 255 && std::abs(half[3] - 128) <= 1);
}

int main(int argc, char **argv)
{
  sha256();
  colors();
  if (argc < 2) {
    std::printf("usage: text-raster-test <font.ttf>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::stringstream bytes;
  bytes << in.rdbuf();
  std::string error;
  CHECK(!Face::load("not a font", &error) && !error.empty());
  const auto face = Face::load(bytes.str(), &error);
  CHECK(face != nullptr);
  if (!face) {
    std::printf("could not read %s: %s\n", argv[1], error.c_str());
    return 1;
  }
  wrapping(*face);
  drawing(*face);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("text-raster-test: all checks passed\n");
  return 0;
}
