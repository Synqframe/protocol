// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace synqframe {

// Wire format: AWS IVS Multitrack Video integration guide, BPM TS/SM/ERM.
// Counters are measured cumulatively; each track emits deltas since its last IDR.
struct BpmSample {
  std::string timestamp; // RFC3339 UTC, measured at packet interleave
  std::array<uint32_t, 4> session{}; // rendered, lagged, network dropped, output
  std::array<uint32_t, 3> rendition{}; // input, skipped, encoded
};

struct BpmTrack {
  bool initialized = false;
  int64_t lastPts = 0;
  BpmSample previous;
};

// Returns an empty vector unless the Annex-B access unit contains an IDR.
// Preserves every original byte, inserting three separate SEI NALs immediately
// before the first VCL NAL (after any AUD and parameter sets).
std::vector<uint8_t> insertBpm(std::span<const uint8_t> accessUnit, bool hevc,
                               int64_t pts, const BpmSample &sample, BpmTrack &track);

} // namespace synqframe
