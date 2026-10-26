# Capture-time SEI

**Owned by this repository**, not vendored from the server: the OBS plugin writes
it and the compositor worker reads it. Ingest (MediaMTX) only passes it through;
the server never parses it. Code: `plugins/synqframe/core/{include/synqframe,src}/capture-time-sei.*`.

## Purpose

Every bundle video frame carries the instant it was captured, in a clock
shared by every player and ingest. OBS's own program (the composer-self ladder, or
its SRT fallback) is not stamped: it is no feed, and nothing reads its stamps.
The worker uses the stamps to line feeds up in time,
so a layout switch lands on the right frame even when players publish to
different ingests with different latencies.

## Carriage

- One `user_data_unregistered` SEI message (payloadType 5) per access unit, in
  every frame, keyframe or not.
- **H.264:** its own SEI NAL (type 6, `nal_ref_idc` 0).
  **HEVC:** a prefix SEI NAL (type 39, layer 0, temporal id 0).
- It sits after any AUD and parameter sets and before the first picture (VCL) NAL.
- Standard emulation prevention applies to the NAL payload.
- AV1 is not stamped yet; its frames go unstamped.

Verified 2026-09-28: MediaMTX v1.19.3 carries it unchanged from SRT (MPEG-TS) in
to RTSP out, for H.264 and HEVC.

## Payload

Big-endian, 34 bytes:

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 16 | UUID | `05ef3338-0df1-488e-899c-3097872daff7`; marks a Synqframe capture-time SEI |
| 16 | 1 | version | `1`. A newer version may append fields; readers accept any version ≥ 1 and read only what they know |
| 17 | 1 | flags | bit 0 `synced`: the time-server offset was applied. Other bits are reserved and set to 0 |
| 18 | 8 | captureTime | Signed nanoseconds since the Unix epoch (UTC) when OBS rendered the frame (`encoder_packet_time.cts`) |
| 26 | 4 | uncertainty | Microseconds of clock error bound; `0xFFFFFFFF` = unknown (always when not `synced`) |
| 30 | 4 | sequence | Per-output frame counter from 0, wrapping; a gap means frames were lost after the plugin |

## Clock

- `captureTime` = OBS's monotonic frame time mapped to the machine's UTC clock,
  plus the time client's offset to its ingest's time server when `synced`.
- Every ingest's time server follows the same grandmaster, time.cloudflare.com
  over NTS, so stamps from different ingests are
  comparable to within a few milliseconds. Cloudflare answers at stratum 3, so an
  ingest answers at stratum 4: expected, and no measure of accuracy.
- Unsynced stamps are only comparable within one feed. Don't align feeds on them.
- Frames whose encoder reports no capture time are sent unstamped, never guessed.

## Reading

`findCaptureTime()` scans an access unit's SEI NALs (H.264 type 6; HEVC 39 or 40)
for a user-data-unregistered message with this UUID and skips anything else.
`capture-time-sei-test read h264|hevc <file>` reports the stamps in a captured
Annex-B stream.
