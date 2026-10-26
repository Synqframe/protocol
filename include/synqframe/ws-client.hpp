// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A reconnecting WebSocket client on libcurl, used by the plugin's layout socket and
// client config channel and by the compositor worker's socket. It needs libcurl only;
// it logs through the config's `log` callback.
//
// Behaviour its users depend on:
// - Only the connect and upgrade phase is bounded. CURLOPT_TIMEOUT_MS would stay
//   in force for the life of a connect-only handle and make the automatic pong
//   fail at the server's first keepalive ping.
// - Outgoing messages wait in one slot per key: a newer message replaces an
//   unsent one for the same key, and nothing else, so a burst of edits to one
//   thing never displaces another's. A write the socket cannot take yet is kept.
// - Reconnects back off from 1 s to 32 s; a connection that lasted 10 s resets it.
struct WsClientConfig {
  std::string url;
  // Full header lines, for example "Authorization: Bearer ...". Never logged.
  std::vector<std::string> headers;
  // Log lines: "<connectedLog>" on connect and
  // "<label> disconnected: <curl error>; reconnecting" otherwise, handed to `log` (none when
  // it is empty) on the socket thread. `warning` is true for the disconnect line.
  std::function<void(bool warning, const std::string &line)> log;
  std::string connectedLog;
  std::string label;
  // These run on the socket thread.
  std::function<void()> onConnected;
  std::function<void()> onDisconnected;
  // A complete text message.
  std::function<void(const std::string &)> onMessage;
  // Test only: skip certificate verification, for a local server with a
  // throwaway self-signed certificate.
  bool insecureTls = false;
  // Close codes after which reconnecting cannot help. When this returns true,
  // onFinalClose runs and the socket thread ends.
  std::function<bool(int code)> isFinalClose;
  std::function<void(int code)> onFinalClose;
};

class WsClient {
public:
  explicit WsClient(WsClientConfig config);
  ~WsClient();
  WsClient(const WsClient &) = delete;
  WsClient &operator=(const WsClient &) = delete;

  void start();
  // Ends the socket thread and drops any unsent messages.
  void stop();

  bool connected() const { return connected_; }
  // Queues a message in the slot named `key`, replacing an unsent one there.
  void send(const std::string &key, std::string payload);
  bool hasPending() const;

private:
  void run(std::stop_token stop);

  WsClientConfig config_;
  std::jthread thread_;
  std::atomic_bool connected_{false};
  mutable std::mutex mutex_;
  std::map<std::string, std::string> pending_;
};
