// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once
// Capture-time SEI: every contribution video frame carries the time it was
// captured, in the shared (time-server) clock, so the compositor worker can line
// feeds up across players and ingests. Format: docs/capture-time-sei.md.
//
// Pure byte handling, no libobs: the plugin writes these and the compositor
// worker reads them with the same code.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace synqframe {

enum class VideoCodec { H264, Hevc };

// user_data_unregistered UUID that marks a Synqframe capture-time SEI.
inline constexpr std::array<uint8_t, 16> CaptureTimeSeiUuid = {
  0x05, 0xef, 0x33, 0x38, 0x0d, 0xf1, 0x48, 0x8e, 0x89, 0x9c, 0x30, 0x97, 0x87, 0x2d, 0xaf, 0xf7};
inline constexpr uint8_t CaptureTimeSeiVersion = 1;
inline constexpr uint32_t CaptureTimeUncertaintyUnknown = 0xffffffffu;

struct CaptureTime {
  // Set when the plugin applied a time-server offset; unset means the stamp is
  // the machine's own clock and only comparable to itself.
  bool synced = false;
  // Capture instant, nanoseconds since the Unix epoch (UTC).
  int64_t captureTimeNs = 0;
  // Bound on the clock error, microseconds; CaptureTimeUncertaintyUnknown if unsynced.
  uint32_t uncertaintyUs = CaptureTimeUncertaintyUnknown;
  // Per-output frame counter; a gap means frames were lost between plugin and reader.
  uint32_t sequence = 0;
};

// One complete NAL unit (4-byte start code, header, escaped payload).
std::vector<uint8_t> buildCaptureTimeSeiNal(VideoCodec codec, const CaptureTime &time);

// Copies an Annex-B access unit with the SEI inserted before its first picture
// (VCL) NAL, after any AUD/parameter sets. Returns false and leaves `out` empty
// when the access unit has no start code or no picture NAL.
bool insertBeforeFirstPicture(VideoCodec codec, const uint8_t *data, size_t size,
                              const std::vector<uint8_t> &seiNal, std::vector<uint8_t> &out);

// Finds a Synqframe capture-time SEI in an Annex-B access unit.
std::optional<CaptureTime> findCaptureTime(VideoCodec codec, const uint8_t *data, size_t size);

}
