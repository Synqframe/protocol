// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once
// SNTP (RFC 4330) client-mode packets and the offset/uncertainty they give. Pure
// byte handling, no sockets: the plugin's time client sends and receives.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace synqframe {

using NtpPacket = std::array<uint8_t, 48>;

// A client request whose transmit timestamp is `sentUnixNs` (the server echoes it
// back as the originate timestamp, which ties the reply to this request).
NtpPacket ntpRequest(int64_t sentUnixNs);

struct NtpSample {
  // Server time - local time: add to the local clock to get the server's.
  int64_t offsetNs = 0;
  // Round trip minus the server's own processing time.
  int64_t delayNs = 0;
  // Bound on the error of (local + offset) against the server's reference:
  // half the round trip, plus the server's own root delay/2 and root dispersion.
  int64_t uncertaintyNs = 0;
  int stratum = 0;
};

// Reads a reply to the request sent at sentUnixNs and received at receivedUnixNs
// (local clock). nullopt when it isn't a usable answer: wrong size or mode, not
// ours, kiss-o'-death, or a server that says it isn't synchronized (leap 3).
std::optional<NtpSample> readNtpReply(std::span<const uint8_t> reply, int64_t sentUnixNs, int64_t receivedUnixNs);

// The sample to trust from one round: the smallest round trip carries the least
// queueing asymmetry.
std::optional<NtpSample> bestNtpSample(std::span<const NtpSample> samples);

}
