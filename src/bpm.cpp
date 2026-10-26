// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/bpm.hpp>

#include <algorithm>

namespace synqframe {
namespace {
using Bytes = std::vector<uint8_t>;
constexpr std::array<uint8_t, 16> tsUuid{0x0a,0xec,0xff,0xe7,0x52,0x72,0x4e,0x2f,0xa6,0x2f,0xd1,0x9c,0xd6,0x1a,0x93,0xb5};
constexpr std::array<uint8_t, 16> smUuid{0xca,0x60,0xe7,0x1c,0x6a,0x8b,0x43,0x88,0xa3,0x77,0x15,0x1d,0xf7,0xbf,0x8a,0xc2};
constexpr std::array<uint8_t, 16> ermUuid{0xf1,0xfb,0xc1,0xd5,0x10,0x1e,0x4f,0xb5,0xa6,0x1e,0xb8,0xce,0x3c,0x07,0xb8,0xc0};

Bytes payload(const std::array<uint8_t, 16> &uuid, const std::string &time)
{
  Bytes result(uuid.begin(), uuid.end());
  result.insert(result.end(), {0, 1, 4}); // one timestamp, RFC3339, PIR
  result.insert(result.end(), time.begin(), time.end());
  result.push_back(0); // H.264 st(v) terminator
  return result;
}

template<size_t N>
void counters(Bytes &result, const std::array<uint32_t, N> &current,
              const std::array<uint32_t, N> &previous, bool first)
{
  result.push_back(static_cast<uint8_t>(N - 1));
  for (size_t i = 0; i < N; ++i) {
    result.push_back(static_cast<uint8_t>(i + 1));
    const uint32_t delta = first ? 0 : current[i] - previous[i];
    for (int shift = 24; shift >= 0; shift -= 8)
      result.push_back(static_cast<uint8_t>(delta >> shift));
  }
}

void appendSei(Bytes &result, const Bytes &data, bool hevc, uint8_t h0, uint8_t h1)
{
  result.insert(result.end(), {0, 0, 0, 1});
  if (hevc) {
    result.push_back(static_cast<uint8_t>((39 << 1) | (h0 & 1))); // prefix SEI
    result.push_back(h1); // preserve layer and temporal ID from the IDR
  } else {
    result.push_back(6);
  }
  Bytes rbsp{5}; // user_data_unregistered
  size_t remaining = data.size();
  while (remaining >= 255) { rbsp.push_back(255); remaining -= 255; }
  rbsp.push_back(static_cast<uint8_t>(remaining));
  rbsp.insert(rbsp.end(), data.begin(), data.end());
  rbsp.push_back(0x80); // rbsp_trailing_bits
  unsigned zeros = 0;
  for (uint8_t byte : rbsp) {
    if (zeros == 2 && byte <= 3) { result.push_back(3); zeros = 0; }
    result.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
}
} // namespace

std::vector<uint8_t> insertBpm(std::span<const uint8_t> au, bool hevc,
                               int64_t pts, const BpmSample &sample, BpmTrack &track)
{
  size_t firstVcl = au.size();
  bool idr = false;
  uint8_t h0 = 0, h1 = 1;
  for (size_t i = 0; i + 3 < au.size();) {
    size_t prefix = 0;
    if (au[i] == 0 && au[i + 1] == 0) {
      if (au[i + 2] == 1) prefix = 3;
      else if (au[i + 2] == 0 && au[i + 3] == 1) prefix = 4;
    }
    if (!prefix || i + prefix + (hevc ? 1 : 0) >= au.size()) { ++i; continue; }
    const uint8_t header = au[i + prefix];
    const unsigned type = hevc ? (header >> 1) & 63 : header & 31;
    const bool vcl = hevc ? type <= 31 : type >= 1 && type <= 5;
    if (vcl && firstVcl == au.size()) {
      firstVcl = i;
      h0 = header;
      if (hevc) h1 = au[i + prefix + 1];
    }
    if (hevc ? type == 19 || type == 20 : type == 5) idr = true;
    i += prefix + (hevc ? 2 : 1);
  }
  if (!idr || firstVcl == au.size()) return {};

  const bool first = !track.initialized || pts <= track.lastPts;
  auto ts = payload(tsUuid, sample.timestamp);
  auto sm = payload(smUuid, sample.timestamp);
  auto erm = payload(ermUuid, sample.timestamp);
  counters(sm, sample.session, track.previous.session, first);
  counters(erm, sample.rendition, track.previous.rendition, first);
  Bytes result(au.begin(), au.begin() + firstVcl);
  for (const Bytes *data : {&ts, &sm, &erm}) appendSei(result, *data, hevc, h0, h1);
  result.insert(result.end(), au.begin() + firstVcl, au.end());
  track.previous = sample;
  track.lastPts = pts;
  track.initialized = true;
  return result;
}
} // namespace synqframe
