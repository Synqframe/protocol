// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

// Checks the SNTP request/reply math (no sockets, no libobs).
#include <synqframe/ntp-sample.hpp>

#include <cstdio>
#include <cstdlib>
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

constexpr int64_t Ms = 1'000'000;
constexpr int64_t NtpToUnix = 2208988800LL;

static void put(NtpPacket &p, int at, uint64_t value, int bytes)
{
  for (int i = bytes - 1; i >= 0; --i, value >>= 8) p[at + i] = static_cast<uint8_t>(value);
}

static uint64_t ntp(int64_t unixNs)
{
  return (static_cast<uint64_t>(unixNs / 1'000'000'000 + NtpToUnix) << 32) |
         static_cast<uint64_t>(((unixNs % 1'000'000'000) << 32) / 1'000'000'000);
}

// A server whose clock is `offset` ahead, reached over `out` ms, answering after
// `processing` ms over `back` ms. Returns the reply and when it arrives locally.
static NtpPacket serve(const NtpPacket &request, int64_t t1, int64_t offset, int64_t out, int64_t processing,
                       int64_t back, int64_t &t4, int leap = 0, int stratum = 3)
{
  NtpPacket reply{};
  reply[0] = static_cast<uint8_t>((leap << 6) | (4 << 3) | 4);
  reply[1] = static_cast<uint8_t>(stratum);
  put(reply, 4, (2 * 65536) / 1000, 4); // root delay ~2 ms (16.16 fixed-point seconds)
  put(reply, 8, (1 * 65536) / 1000, 4); // root dispersion ~1 ms
  for (int i = 0; i < 8; ++i) reply[24 + i] = request[40 + i]; // originate = our transmit
  const int64_t t2 = t1 + out + offset, t3 = t2 + processing;
  put(reply, 32, ntp(t2), 8);
  put(reply, 40, ntp(t3), 8);
  t4 = t1 + out + processing + back;
  return reply;
}

int main()
{
  const int64_t t1 = 1'790'000'000'000'000'000;
  const NtpPacket request = ntpRequest(t1);
  CHECK(request[0] == 0x23); // v4, client

  // 250 ms ahead, 10 ms out, 1 ms processing, 30 ms back: the asymmetry makes the
  // offset read 10 ms low, and the uncertainty has to cover that.
  int64_t t4 = 0;
  const NtpPacket reply = serve(request, t1, 250 * Ms, 10 * Ms, 1 * Ms, 30 * Ms, t4);
  const auto sample = readNtpReply(reply, t1, t4);
  CHECK(sample.has_value());
  if (sample) {
    CHECK(std::llabs(sample->offsetNs - 240 * Ms) < 1000);
    CHECK(std::llabs(sample->delayNs - 40 * Ms) < 1000);
    CHECK(sample->uncertaintyNs >= 20 * Ms && sample->uncertaintyNs < 25 * Ms);
    CHECK(std::llabs(sample->offsetNs - 250 * Ms) <= sample->uncertaintyNs); // the truth is inside the bound
    CHECK(sample->stratum == 3);
  }

  // Rejected: not synchronized yet (leap 3), kiss-o'-death (stratum 0), a reply to
  // some other request, a non-server mode, a short packet.
  int64_t unused = 0;
  CHECK(!readNtpReply(serve(request, t1, 0, Ms, 0, Ms, unused, 3), t1, unused));
  CHECK(!readNtpReply(serve(request, t1, 0, Ms, 0, Ms, unused, 0, 0), t1, unused));
  NtpPacket other = serve(ntpRequest(t1 + 5 * Ms), t1, 0, Ms, 0, Ms, unused);
  CHECK(!readNtpReply(other, t1, unused));
  NtpPacket wrongMode = serve(request, t1, 0, Ms, 0, Ms, unused);
  wrongMode[0] = (wrongMode[0] & ~0x7) | 3;
  CHECK(!readNtpReply(wrongMode, t1, unused));
  CHECK(!readNtpReply(std::span<const uint8_t>(reply.data(), 40), t1, t4));

  // The round's best sample is the shortest round trip.
  const std::vector<NtpSample> samples = {{5 * Ms, 60 * Ms, 31 * Ms, 3}, {2 * Ms, 12 * Ms, 7 * Ms, 3},
                                          {9 * Ms, 40 * Ms, 21 * Ms, 3}};
  const auto best = bestNtpSample(samples);
  CHECK(best && best->delayNs == 12 * Ms);
  CHECK(!bestNtpSample({}));

  std::printf(failures ? "%d failure(s)\n" : "All checks passed.\n", failures);
  return failures ? 1 : 0;
}
