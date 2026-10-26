// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/ntp-sample.hpp>

#include <algorithm>

namespace synqframe {
namespace {
// Seconds from the NTP era-0 epoch (1900) to the Unix epoch (1970).
constexpr int64_t NtpToUnixSeconds = 2208988800LL;

uint64_t readBig(const uint8_t *p, int bytes)
{
  uint64_t value = 0;
  for (int i = 0; i < bytes; ++i) value = (value << 8) | p[i];
  return value;
}

void writeBig(uint8_t *p, uint64_t value, int bytes)
{
  for (int i = bytes - 1; i >= 0; --i, value >>= 8) p[i] = static_cast<uint8_t>(value);
}

uint64_t toNtp(int64_t unixNs)
{
  const int64_t seconds = unixNs / 1'000'000'000 + NtpToUnixSeconds;
  const int64_t remainder = unixNs % 1'000'000'000;
  return (static_cast<uint64_t>(seconds) << 32) | static_cast<uint64_t>((remainder << 32) / 1'000'000'000);
}

int64_t toUnixNs(uint64_t ntp)
{
  const int64_t seconds = static_cast<int64_t>(ntp >> 32) - NtpToUnixSeconds;
  const int64_t fraction = static_cast<int64_t>(((ntp & 0xffffffffULL) * 1'000'000'000ULL) >> 32);
  return seconds * 1'000'000'000 + fraction;
}

// NTP short format: 16.16 fixed-point seconds.
int64_t shortToNs(uint32_t value)
{
  return static_cast<int64_t>((static_cast<uint64_t>(value) * 1'000'000'000ULL) >> 16);
}
} // namespace

NtpPacket ntpRequest(int64_t sentUnixNs)
{
  NtpPacket packet{};
  packet[0] = (0 << 6) | (4 << 3) | 3; // no leap warning, version 4, client
  writeBig(packet.data() + 40, toNtp(sentUnixNs), 8);
  return packet;
}

std::optional<NtpSample> readNtpReply(std::span<const uint8_t> reply, int64_t sentUnixNs, int64_t receivedUnixNs)
{
  if (reply.size() < 48) return std::nullopt;
  const int leap = reply[0] >> 6;
  const int mode = reply[0] & 0x7;
  const int stratum = reply[1];
  if (mode != 4 || leap == 3 || stratum == 0 || stratum > 15) return std::nullopt;
  if (readBig(reply.data() + 24, 8) != toNtp(sentUnixNs)) return std::nullopt; // not our request

  const int64_t t1 = sentUnixNs;
  const int64_t t2 = toUnixNs(readBig(reply.data() + 32, 8)); // server received
  const int64_t t3 = toUnixNs(readBig(reply.data() + 40, 8)); // server sent
  const int64_t t4 = receivedUnixNs;
  if (t3 < t2) return std::nullopt;
  NtpSample sample;
  sample.offsetNs = ((t2 - t1) + (t3 - t4)) / 2;
  sample.delayNs = std::max<int64_t>(0, (t4 - t1) - (t3 - t2));
  const int64_t rootDelay = shortToNs(static_cast<uint32_t>(readBig(reply.data() + 4, 4)));
  const int64_t rootDispersion = shortToNs(static_cast<uint32_t>(readBig(reply.data() + 8, 4)));
  sample.uncertaintyNs = sample.delayNs / 2 + rootDelay / 2 + rootDispersion;
  sample.stratum = stratum;
  return sample;
}

std::optional<NtpSample> bestNtpSample(std::span<const NtpSample> samples)
{
  if (samples.empty()) return std::nullopt;
  return *std::min_element(samples.begin(), samples.end(),
                           [](const NtpSample &a, const NtpSample &b) { return a.delayNs < b.delayNs; });
}

}
