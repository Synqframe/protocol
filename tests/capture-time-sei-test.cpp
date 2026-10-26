// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

// Checks the capture-time SEI writer and reader (no libobs).
//
//   capture-time-sei-test                          unit checks
//   capture-time-sei-test h264|hevc <in> <out>     also stamps every access unit of a real
//                                                  Annex-B stream, reads each stamp back,
//                                                  and writes <out> for a decoder to check
//   capture-time-sei-test read h264|hevc <in>      reports the stamps in a captured stream
#include <synqframe/capture-time-sei.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace synqframe;

static int failures = 0;
#define CHECK(condition)                                                                              \
  do {                                                                                                \
    if (!(condition)) {                                                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);                                \
      ++failures;                                                                                     \
    }                                                                                                 \
  } while (0)

static void append(std::vector<uint8_t> &au, std::initializer_list<uint8_t> nal)
{
  au.insert(au.end(), {0, 0, 0, 1});
  au.insert(au.end(), nal);
}

static bool same(const CaptureTime &a, const CaptureTime &b)
{
  return a.synced == b.synced && a.captureTimeNs == b.captureTimeNs && a.uncertaintyUs == b.uncertaintyUs &&
         a.sequence == b.sequence;
}

static void roundTrip(VideoCodec codec)
{
  // Zero-heavy values force emulation-prevention bytes into the payload.
  const CaptureTime times[] = {
    {false, 0x0000000000000102, CaptureTimeUncertaintyUnknown, 0},
    {true, 1790000000123456789, 850, 0x00000003},
    {true, -1, 0, 0xffffffff},
  };
  for (const CaptureTime &time : times) {
    const auto nal = buildCaptureTimeSeiNal(codec, time);
    // 00 00 03 is the escape itself; 00 00 00/01/02 must never appear.
    bool escaped = true;
    for (size_t i = 4; i + 2 < nal.size(); ++i) escaped = escaped && !(nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] <= 2);
    CHECK(escaped);
    const auto parsed = findCaptureTime(codec, nal.data(), nal.size());
    CHECK(parsed && same(*parsed, time));
  }
}

static void placement()
{
  const CaptureTime time{true, 42, 7, 9};
  // H.264: AUD, SPS, PPS, foreign SEI, IDR slice.
  std::vector<uint8_t> au;
  append(au, {0x09, 0xf0});
  append(au, {0x67, 0x42, 0x00, 0x1e});
  append(au, {0x68, 0xce, 0x38, 0x80});
  append(au, {0x06, 0x05, 0x02, 0xaa, 0xbb, 0x80});
  const size_t pictureAt = au.size();
  append(au, {0x65, 0x88, 0x84, 0x00});
  const auto sei = buildCaptureTimeSeiNal(VideoCodec::H264, time);
  std::vector<uint8_t> out;
  CHECK(insertBeforeFirstPicture(VideoCodec::H264, au.data(), au.size(), sei, out));
  CHECK(out.size() == au.size() + sei.size());
  CHECK(std::memcmp(out.data(), au.data(), pictureAt) == 0);
  CHECK(std::memcmp(out.data() + pictureAt, sei.data(), sei.size()) == 0);
  CHECK(std::memcmp(out.data() + pictureAt + sei.size(), au.data() + pictureAt, au.size() - pictureAt) == 0);
  const auto parsed = findCaptureTime(VideoCodec::H264, out.data(), out.size());
  CHECK(parsed && same(*parsed, time));
  CHECK(!findCaptureTime(VideoCodec::H264, au.data(), au.size()));

  // HEVC: VPS, SPS, PPS, IDR_W_RADL; 3-byte start code on the picture.
  std::vector<uint8_t> hevc;
  append(hevc, {0x40, 0x01, 0x0c});
  append(hevc, {0x42, 0x01, 0x01});
  append(hevc, {0x44, 0x01, 0xc1});
  const size_t hevcPictureAt = hevc.size();
  hevc.insert(hevc.end(), {0, 0, 1, 0x26, 0x01, 0xaf});
  const auto hevcSei = buildCaptureTimeSeiNal(VideoCodec::Hevc, time);
  CHECK(insertBeforeFirstPicture(VideoCodec::Hevc, hevc.data(), hevc.size(), hevcSei, out));
  CHECK(std::memcmp(out.data() + hevcPictureAt, hevcSei.data(), hevcSei.size()) == 0);
  const auto hevcParsed = findCaptureTime(VideoCodec::Hevc, out.data(), out.size());
  CHECK(hevcParsed && same(*hevcParsed, time));

  // No picture, or no start code: nothing to stamp.
  std::vector<uint8_t> headersOnly;
  append(headersOnly, {0x67, 0x42});
  CHECK(!insertBeforeFirstPicture(VideoCodec::H264, headersOnly.data(), headersOnly.size(), sei, out) && out.empty());
  const uint8_t raw[] = {0x65, 0x88, 0x84};
  CHECK(!insertBeforeFirstPicture(VideoCodec::H264, raw, sizeof(raw), sei, out));
}

// Splits a raw elementary stream at each picture's first slice, the way an
// encoder hands OBS one access unit per packet.
static std::vector<std::vector<uint8_t>> accessUnits(VideoCodec codec, const std::vector<uint8_t> &stream)
{
  std::vector<std::vector<uint8_t>> units;
  size_t unitStart = 0;
  bool sawPicture = false;
  for (size_t i = 0; i + 4 < stream.size(); ++i) {
    if (stream[i] || stream[i + 1] || stream[i + 2] != 1) continue;
    const size_t start = i > 0 && stream[i - 1] == 0 ? i - 1 : i;
    const uint8_t *h = &stream[i + 3];
    const bool hevc = codec == VideoCodec::Hevc;
    const uint8_t type = hevc ? (h[0] >> 1) & 0x3f : h[0] & 0x1f;
    const bool picture = hevc ? type <= 31 : type >= 1 && type <= 5;
    const bool firstSlice = picture && (hevc ? (h[2] & 0x80) : (h[1] & 0x80));
    // A new access unit starts at the first non-picture NAL after a picture, or at a first slice.
    if (sawPicture && (!picture || firstSlice)) {
      units.emplace_back(stream.begin() + unitStart, stream.begin() + start);
      unitStart = start;
      sawPicture = false;
    }
    sawPicture = sawPicture || picture;
    i += 2;
  }
  units.emplace_back(stream.begin() + unitStart, stream.end());
  return units;
}

static int stampFile(VideoCodec codec, const char *in, const char *out)
{
  std::ifstream input(in, std::ios::binary);
  const std::vector<uint8_t> stream((std::istreambuf_iterator<char>(input)), {});
  std::vector<uint8_t> stamped, unit;
  uint32_t sequence = 0, readBack = 0;
  const auto units = accessUnits(codec, stream);
  for (const auto &au : units) {
    const CaptureTime time{true, 1790000000000000000 + int64_t(sequence) * 33333333, 500, sequence};
    if (!insertBeforeFirstPicture(codec, au.data(), au.size(), buildCaptureTimeSeiNal(codec, time), unit)) {
      stamped.insert(stamped.end(), au.begin(), au.end());
      continue;
    }
    const auto parsed = findCaptureTime(codec, unit.data(), unit.size());
    if (parsed && same(*parsed, time)) ++readBack;
    stamped.insert(stamped.end(), unit.begin(), unit.end());
    ++sequence;
  }
  std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char *>(stamped.data()), stamped.size());
  std::printf("%zu access units, %u stamped, %u read back\n", units.size(), sequence, readBack);
  CHECK(sequence > 0 && readBack == sequence);
  return 0;
}

// Reports the stamps in a captured stream, e.g. one pulled from ingest with
// `ffmpeg -i rtsp://... -c copy -f h264 feed.h264`.
static int readFile(VideoCodec codec, const char *in)
{
  std::ifstream input(in, std::ios::binary);
  const std::vector<uint8_t> stream((std::istreambuf_iterator<char>(input)), {});
  const auto units = accessUnits(codec, stream);
  size_t stamped = 0, synced = 0, gaps = 0;
  std::optional<CaptureTime> first, last;
  for (const auto &au : units) {
    const auto time = findCaptureTime(codec, au.data(), au.size());
    if (!time) continue;
    if (last && time->sequence != last->sequence + 1) ++gaps;
    if (!first) first = time;
    last = time;
    ++stamped;
    synced += time->synced;
  }
  std::printf("%zu access units, %zu stamped (%zu synced), %zu sequence gaps\n", units.size(), stamped, synced, gaps);
  if (first && last) {
    const double spanMs = (last->captureTimeNs - first->captureTimeNs) / 1e6;
    std::printf("sequence %u..%u, capture span %.1f ms (%.2f fps)\n", first->sequence, last->sequence, spanMs,
                stamped > 1 && spanMs > 0 ? (stamped - 1) * 1000.0 / spanMs : 0.0);
  }
  return stamped ? 0 : 1;
}

int main(int argc, char **argv)
{
  if (argc == 4 && std::strcmp(argv[1], "read") == 0)
    return readFile(std::strcmp(argv[2], "hevc") == 0 ? VideoCodec::Hevc : VideoCodec::H264, argv[3]);
  roundTrip(VideoCodec::H264);
  roundTrip(VideoCodec::Hevc);
  placement();
  if (argc == 4) stampFile(std::strcmp(argv[1], "hevc") == 0 ? VideoCodec::Hevc : VideoCodec::H264, argv[2], argv[3]);
  std::printf(failures ? "%d failure(s)\n" : "All checks passed.\n", failures);
  return failures ? 1 : 0;
}
