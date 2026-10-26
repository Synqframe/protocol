// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/bpm.hpp>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

using Bytes = std::vector<uint8_t>;
static void check(bool value) { if (!value) throw std::runtime_error("BPM check failed"); }

// Independent Annex-B/RBSP reader: validate the bytes a downstream reader sees.
static std::vector<Bytes> nals(const Bytes &bytes)
{
  std::vector<Bytes> out;
  size_t start = bytes.size();
  for (size_t i = 0; i + 3 <= bytes.size();) {
    size_t prefix = 0;
    if (bytes[i] == 0 && bytes[i + 1] == 0) {
      if (bytes[i + 2] == 1) prefix = 3;
      else if (i + 4 <= bytes.size() && bytes[i + 2] == 0 && bytes[i + 3] == 1) prefix = 4;
    }
    if (!prefix) { ++i; continue; }
    if (start < bytes.size()) out.emplace_back(bytes.begin() + start, bytes.begin() + i);
    start = i + prefix;
    i = start;
  }
  if (start < bytes.size()) out.emplace_back(bytes.begin() + start, bytes.end());
  return out;
}

static Bytes sei(const Bytes &nal, bool hevc)
{
  check(hevc ? (nal[0] >> 1) == 39 && nal[1] == 1 : nal[0] == 6);
  Bytes rbsp;
  unsigned zeros = 0;
  for (size_t i = hevc ? 2 : 1; i < nal.size(); ++i) {
    uint8_t byte = nal[i];
    if (zeros == 2 && byte == 3) { zeros = 0; continue; }
    rbsp.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  check(rbsp[0] == 5);
  size_t size = 0, i = 1;
  while (rbsp.at(i) == 255) { size += 255; ++i; }
  size += rbsp.at(i++);
  check(i + size + 1 == rbsp.size() && rbsp.back() == 0x80);
  return {rbsp.begin() + i, rbsp.begin() + i + size};
}

static std::string hex(std::span<const uint8_t> bytes)
{
  std::string result;
  for (uint8_t byte : bytes) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

static void checkMetrics(const std::vector<Bytes> &units, bool hevc, const synqframe::BpmSample &expected)
{
  const char *ids[] = {"0aecffe752724e2fa62fd19cd61a93b5", "ca60e71c6a8b4388a377151df7bf8ac2", "f1fbc1d5101e4fb5a61eb8ce3c07b8c0"};
  for (size_t k = 0; k < 3; ++k) {
    auto data = sei(units[k + 2], hevc);
    check(hex({data.data(), 16}) == ids[k]);
    check(data[16] == 0 && data[17] == 1 && data[18] == 4);
    const auto end = std::find(data.begin() + 19, data.end(), 0);
    check(end != data.end());
    check(std::string(data.begin() + 19, end) == expected.timestamp);
    size_t pos = static_cast<size_t>(end - data.begin()) + 1;
    if (k == 0) { check(pos == data.size()); continue; }
    const size_t count = k == 1 ? 4 : 3;
    check(data.at(pos++) == count - 1);
    for (size_t j = 0; j < count; ++j) {
      check(data.at(pos++) == j + 1);
      uint32_t value = 0;
      for (int b = 0; b < 4; ++b) value = (value << 8) | data.at(pos++);
      check(value == (k == 1 ? expected.session[j] : expected.rendition[j]));
    }
    check(pos == data.size());
  }
}

int main()
{
  try {
    for (bool hevc : {false, true}) {
      // Parameter sets, AUD, and multiple slices survive byte-for-byte.
      const Bytes au = hevc ? Bytes{0,0,0,1,64,1,0x80, 0,0,1,70,1,0x80, 0,0,0,1,38,1,0xab, 0,0,1,38,1,0xcd}
                            : Bytes{0,0,0,1,0x67,0x80, 0,0,1,9,0x80, 0,0,0,1,0x65,0xab, 0,0,1,0x65,0xcd};
      synqframe::BpmTrack state;
      synqframe::BpmSample sample{"2026-09-30T12:34:56.789Z", {1234,4,7,5678}, {1200,3,600}};
      auto result = synqframe::insertBpm(au, hevc, 0, sample, state);
      auto units = nals(result), original = nals(au);
      check(units.size() == original.size() + 3);
      check(units[0] == original[0] && units[1] == original[1]);
      check(units[5] == original[2] && units[6] == original[3]);
      synqframe::BpmSample zero{sample.timestamp};
      checkMetrics(units, hevc, zero);

      sample.session = {1354,6,8,5918};
      sample.rendition = {1320,4,660};
      result = synqframe::insertBpm(au, hevc, 60, sample, state);
      checkMetrics(nals(result), hevc, {sample.timestamp, {120,2,1,240}, {120,1,60}});
      // Reconnect / PTS reset establishes a new baseline, not a wrapped delta.
      result = synqframe::insertBpm(au, hevc, 0, sample, state);
      checkMetrics(nals(result), hevc, zero);
      // Another rendition has its own initial zero counters.
      synqframe::BpmTrack second;
      result = synqframe::insertBpm(au, hevc, 60, sample, second);
      checkMetrics(nals(result), hevc, zero);
      Bytes nonIdr = hevc ? Bytes{0,0,1,42,1,0x80} : Bytes{0,0,1,0x41,0x80};
      check(synqframe::insertBpm(nonIdr, hevc, 61, sample, state).empty());
      check(state.lastPts == 0);
      for (const Bytes truncated : {Bytes{}, Bytes{0,0,1}, Bytes{0,0,0,1}, Bytes{0,0}})
        check(synqframe::insertBpm(truncated, hevc, 62, sample, state).empty());
    }
    std::puts("BPM: AVC/HEVC NAL placement, UUIDs, UTC timestamps, counter deltas and reconnect checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
