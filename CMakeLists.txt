cmake_minimum_required(VERSION 3.28...3.30)
project(synqframe-protocol VERSION 0.1.0 LANGUAGES CXX)

# synqframe-protocol: the wire formats and clients the OBS plugin and the Synqframe workers both
# speak. MIT licensed (LICENSE), so the GPL plugin and the workers can each build it in.
#
# - synqframe-protocol: the capture-time SEI writer/reader (docs/capture-time-sei.md), the
#   SNTP sample math, and Twitch's BPM SEI (bpm.hpp). No dependencies.
# - synqframe-protocol-net: blocking HTTP calls and the reconnecting WebSocket client, on libcurl.
# - Header-only: slot-mask.hpp (no dependencies) and packed-alpha.hpp (an OBS filter: the
#   includer links libobs).

add_library(synqframe-protocol STATIC src/bpm.cpp src/capture-time-sei.cpp src/ntp-sample.cpp)
target_include_directories(synqframe-protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(synqframe-protocol PUBLIC cxx_std_20)
set_target_properties(synqframe-protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(NOT TARGET CURL::libcurl)
  find_package(CURL REQUIRED)
endif()

add_library(synqframe-protocol-net STATIC src/http.cpp src/ws-client.cpp)
target_link_libraries(synqframe-protocol-net PUBLIC synqframe-protocol CURL::libcurl)
if(WIN32)
  target_link_libraries(synqframe-protocol-net PUBLIC ws2_32)
endif()
set_target_properties(synqframe-protocol-net PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Capture-time SEI writer/reader checks.
add_executable(capture-time-sei-test tests/capture-time-sei-test.cpp)
target_link_libraries(capture-time-sei-test PRIVATE synqframe-protocol)

# SNTP request/reply math checks; no sockets.
add_executable(ntp-sample-test tests/ntp-sample-test.cpp)
target_link_libraries(ntp-sample-test PRIVATE synqframe-protocol)

# BPM SEI insertion checks.
add_executable(bpm-test tests/bpm-test.cpp)
target_link_libraries(bpm-test PRIVATE synqframe-protocol)
