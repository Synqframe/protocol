// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/capture-time-sei.hpp>

#include <algorithm>
#include <cstring>

namespace synqframe {
namespace {
constexpr uint8_t UserDataUnregistered = 5;
constexpr uint8_t H264SeiType = 6;
constexpr uint8_t HevcPrefixSeiType = 39;
constexpr uint8_t HevcSuffixSeiType = 40;
// UUID, version, flags, capture time, uncertainty, sequence.
constexpr size_t PayloadSize = 16 + 1 + 1 + 8 + 4 + 4;
constexpr uint8_t FlagSynced = 0x01;

struct Nal {
  size_t start;  // first byte of the start code
  size_t header; // first byte of the NAL header
  size_t end;    // one past the last byte (before the next start code)
};

std::vector<Nal> splitAnnexB(const uint8_t *data, size_t size)
{
  std::vector<Nal> nals;
  for (size_t i = 0; i + 3 <= size; ++i) {
    if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) continue;
    const size_t start = i > 0 && data[i - 1] == 0 ? i - 1 : i;
    if (!nals.empty()) nals.back().end = start;
    nals.push_back({start, i + 3, size});
    i += 2;
  }
  return nals;
}

uint8_t nalType(VideoCodec codec, uint8_t header)
{
  return codec == VideoCodec::Hevc ? (header >> 1) & 0x3f : header & 0x1f;
}

bool isPicture(VideoCodec codec, uint8_t type)
{
  return codec == VideoCodec::Hevc ? type <= 31 : type >= 1 && type <= 5;
}

void putBig(std::vector<uint8_t> &out, uint64_t value, int bytes)
{
  for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

uint64_t getBig(const uint8_t *data, int bytes)
{
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value = (value << 8) | data[i];
  return value;
}

// Inserts emulation-prevention bytes so the payload can never contain a start code.
void appendEscaped(std::vector<uint8_t> &out, const std::vector<uint8_t> &rbsp)
{
  int zeros = 0;
  for (uint8_t byte : rbsp) {
    if (zeros >= 2 && byte <= 3) {
      out.push_back(3);
      zeros = 0;
    }
    out.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
}

std::vector<uint8_t> unescape(const uint8_t *data, size_t size)
{
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size);
  int zeros = 0;
  for (size_t i = 0; i < size; ++i) {
    if (zeros >= 2 && data[i] == 3) {
      zeros = 0;
      continue;
    }
    rbsp.push_back(data[i]);
    zeros = data[i] == 0 ? zeros + 1 : 0;
  }
  return rbsp;
}

std::optional<CaptureTime> parsePayload(const uint8_t *payload, size_t size)
{
  if (size < PayloadSize || !std::equal(CaptureTimeSeiUuid.begin(), CaptureTimeSeiUuid.end(), payload))
    return std::nullopt;
  const uint8_t *p = payload + 16;
  // A newer version may append fields but keeps these in place.
  if (p[0] < 1) return std::nullopt;
  CaptureTime time;
  time.synced = (p[1] & FlagSynced) != 0;
  time.captureTimeNs = static_cast<int64_t>(getBig(p + 2, 8));
  time.uncertaintyUs = static_cast<uint32_t>(getBig(p + 10, 4));
  time.sequence = static_cast<uint32_t>(getBig(p + 14, 4));
  return time;
}

// Walks the sei_message()s of one SEI RBSP (after the NAL header).
std::optional<CaptureTime> parseSeiRbsp(const std::vector<uint8_t> &rbsp)
{
  size_t i = 0;
  while (i < rbsp.size() && !(rbsp[i] == 0x80 && i + 1 == rbsp.size())) {
    size_t type = 0, size = 0;
    while (i < rbsp.size() && rbsp[i] == 0xff) type += rbsp[i++];
    if (i >= rbsp.size()) break;
    type += rbsp[i++];
    while (i < rbsp.size() && rbsp[i] == 0xff) size += rbsp[i++];
    if (i >= rbsp.size()) break;
    size += rbsp[i++];
    if (size > rbsp.size() - i) break;
    if (type == UserDataUnregistered)
      if (auto time = parsePayload(rbsp.data() + i, size)) return time;
    i += size;
  }
  return std::nullopt;
}
}

std::vector<uint8_t> buildCaptureTimeSeiNal(VideoCodec codec, const CaptureTime &time)
{
  std::vector<uint8_t> rbsp;
  rbsp.push_back(UserDataUnregistered);
  rbsp.push_back(static_cast<uint8_t>(PayloadSize));
  rbsp.insert(rbsp.end(), CaptureTimeSeiUuid.begin(), CaptureTimeSeiUuid.end());
  rbsp.push_back(CaptureTimeSeiVersion);
  rbsp.push_back(time.synced ? FlagSynced : 0);
  putBig(rbsp, static_cast<uint64_t>(time.captureTimeNs), 8);
  putBig(rbsp, time.uncertaintyUs, 4);
  putBig(rbsp, time.sequence, 4);
  rbsp.push_back(0x80); // rbsp_trailing_bits

  std::vector<uint8_t> nal = {0, 0, 0, 1};
  if (codec == VideoCodec::Hevc) {
    nal.push_back(HevcPrefixSeiType << 1); // layer 0
    nal.push_back(1);                      // temporal id 0
  } else {
    nal.push_back(H264SeiType); // nal_ref_idc 0
  }
  appendEscaped(nal, rbsp);
  return nal;
}

bool insertBeforeFirstPicture(VideoCodec codec, const uint8_t *data, size_t size,
                              const std::vector<uint8_t> &seiNal, std::vector<uint8_t> &out)
{
  out.clear();
  for (const Nal &nal : splitAnnexB(data, size)) {
    if (nal.header >= nal.end || !isPicture(codec, nalType(codec, data[nal.header]))) continue;
    out.reserve(size + seiNal.size());
    out.insert(out.end(), data, data + nal.start);
    out.insert(out.end(), seiNal.begin(), seiNal.end());
    out.insert(out.end(), data + nal.start, data + size);
    return true;
  }
  return false;
}

std::optional<CaptureTime> findCaptureTime(VideoCodec codec, const uint8_t *data, size_t size)
{
  const size_t headerSize = codec == VideoCodec::Hevc ? 2 : 1;
  for (const Nal &nal : splitAnnexB(data, size)) {
    if (nal.end - nal.header <= headerSize) continue;
    const uint8_t type = nalType(codec, data[nal.header]);
    const bool sei = codec == VideoCodec::Hevc ? type == HevcPrefixSeiType || type == HevcSuffixSeiType
                                               : type == H264SeiType;
    if (!sei) continue;
    const auto rbsp = unescape(data + nal.header + headerSize, nal.end - nal.header - headerSize);
    if (auto time = parseSeiRbsp(rbsp)) return time;
  }
  return std::nullopt;
}

}
