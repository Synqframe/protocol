# synqframe-protocol

The wire formats and clients that the Synqframe OBS plugin and the Synqframe workers both speak.
It is MIT licensed so that each side can build it in: the plugin is GPL, the workers are not.

| Part | What it is | Needs |
|---|---|---|
| `capture-time-sei.hpp` | Writes and reads the capture-time SEI that stamps each frame with its capture time on a shared clock ([docs/capture-time-sei.md](docs/capture-time-sei.md)) | nothing |
| `ntp-sample.hpp` | SNTP request/reply math for that clock | nothing |
| `http.hpp` | Small blocking HTTP calls | libcurl |
| `ws-client.hpp` | A reconnecting WebSocket client; logs through a callback | libcurl |
| `slot-mask.hpp` | A slot's built-in mask (circle, ellipse, rounded corners), drawn at the slot's own size, the same on both sides | nothing |
| `bpm.hpp` | Inserts Twitch's BPM SEI (AWS IVS multitrack metrics) into H.264/HEVC access units | nothing |
| `packed-alpha.hpp` | The packed-alpha transport matte, as OBS pack/unpack filters | libobs |
| `text-raster.hpp` | A layout text source's pixels, laid out as the studio's editor lays them out, for OBS, the worker and the relay alike | nothing (stb_truetype is built in) |
| `text-source.hpp` | That text as an OBS source, `synqframe_text` | libobs |
| `sha256.hpp` | SHA-256, to check a fetched font or asset against the sha256 its layout names | nothing |

Code taken from OBS Studio or its plugins never goes here: it is GPL, and this library is not.

## Build

```cmake
add_subdirectory(vendor/synqframe-protocol)
target_link_libraries(my-target PRIVATE synqframe-protocol)      # SEI + NTP
target_link_libraries(my-target PRIVATE synqframe-protocol-net)  # also HTTP + WebSocket
```

`capture-time-sei-test`, `ntp-sample-test`, `bpm-test` and `text-raster-test` (run with
`tests/fonts/manrope-700.ttf`) are its checks.

## Third-party code

- `third_party/stb/stb_truetype.h`: stb_truetype v1.26 (nothings/stb `003501f`), MIT or public
  domain at your choice. Built into `synqframe-protocol` privately.
- `tests/fonts/manrope-700.ttf`: Manrope 700, the platform's font, for the checks only; SIL Open
  Font License 1.1 (`tests/fonts/OFL.txt`).
