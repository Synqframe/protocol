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

Code taken from OBS Studio or its plugins never goes here: it is GPL, and this library is not.

## Build

```cmake
add_subdirectory(vendor/synqframe-protocol)
target_link_libraries(my-target PRIVATE synqframe-protocol)      # SEI + NTP
target_link_libraries(my-target PRIVATE synqframe-protocol-net)  # also HTTP + WebSocket
```

`capture-time-sei-test` and `ntp-sample-test` are its checks.
