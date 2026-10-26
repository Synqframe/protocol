// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/ws-client.hpp>

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <stop_token>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace {
// How long one wait for the socket lasts: the ceiling on an outbound message's delay, since
// the send queue is looked at between waits.
constexpr int SocketWaitMs = 20;

// Waits until the connection's socket has something to read, or SocketWaitMs passed. Idle, the
// loop then wakes some fifty times a second instead of polling libcurl without pause.
void waitForSocket(CURL *handle)
{
  curl_socket_t socket = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(handle, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD) {
    std::this_thread::sleep_for(std::chrono::milliseconds(SocketWaitMs));
    return;
  }
#ifdef _WIN32
  WSAPOLLFD fd{socket, POLLRDNORM, 0};
  WSAPoll(&fd, 1, SocketWaitMs);
#else
  pollfd fd{socket, POLLIN, 0};
  poll(&fd, 1, SocketWaitMs);
#endif
}

struct Handshake {
  std::stop_token stopped;
  std::chrono::steady_clock::time_point deadline;
};

// Lets stop() interrupt the connect and upgrade phase, and bounds it.
int handshakeProgress(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  const auto &handshake = *static_cast<Handshake *>(data);
  return handshake.stopped.stop_requested() || std::chrono::steady_clock::now() > handshake.deadline ? 1 : 0;
}
}

WsClient::WsClient(WsClientConfig config) : config_(std::move(config)) {}

WsClient::~WsClient()
{
  stop();
}

void WsClient::start()
{
  if (thread_.joinable()) return;
  thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void WsClient::stop()
{
  if (thread_.joinable()) {
    thread_.request_stop();
    thread_.join();
  }
  connected_ = false;
  std::lock_guard lock(mutex_);
  pending_.clear();
}

void WsClient::send(const std::string &key, std::string payload)
{
  std::lock_guard lock(mutex_);
  pending_[key] = std::move(payload);
}

bool WsClient::hasPending() const
{
  std::lock_guard lock(mutex_);
  return !pending_.empty();
}

void WsClient::run(std::stop_token stop)
{
  unsigned int consecutiveFailures = 0;
  while (!stop.stop_requested()) {
    CURL *handle = curl_easy_init();
    if (!handle) return;
    curl_slist *headers = nullptr;
    for (const std::string &header : config_.headers) headers = curl_slist_append(headers, header.c_str());
    curl_easy_setopt(handle, CURLOPT_URL, config_.url.c_str());
    if (headers) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);
    if (config_.insecureTls) {
      curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    Handshake handshake{stop, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, handshakeProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &handshake);
    CURLcode result = curl_easy_perform(handle);
    const auto connectedAt = std::chrono::steady_clock::now();
    int closeCode = 0;
    // For the disconnect line: a peer that stops sending (pings included) well before the drop
    // is stalled, while one that sends until the end lost the connection under it.
    auto lastFrameAt = connectedAt;
    std::optional<std::chrono::steady_clock::time_point> lastPingAt;
    if (result == CURLE_OK) {
      connected_ = true;
      if (config_.log) config_.log(false, config_.connectedLog);
      if (config_.onConnected) config_.onConnected();
      std::string message;
      while (!stop.stop_requested()) {
        std::map<std::string, std::string> outbound;
        {
          std::lock_guard lock(mutex_);
          outbound.swap(pending_);
        }
        CURLcode sendResult = CURLE_OK;
        for (auto it = outbound.begin(); it != outbound.end(); ++it) {
          // curl_ws_send can return CURLE_OK having written fewer bytes than asked (a
          // nonblocking socket accepting a partial write, documented libcurl behavior);
          // finish this exact message -- resending only the unsent tail, same flags --
          // before moving on, or the remainder is silently dropped.
          size_t offset = 0;
          while (offset < it->second.size()) {
            size_t sent = 0;
            sendResult = curl_ws_send(handle, it->second.data() + offset, it->second.size() - offset, &sent, 0,
                                      CURLWS_TEXT);
            if (sendResult == CURLE_AGAIN) {
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
              if (stop.stop_requested()) break;
              continue;
            }
            if (sendResult != CURLE_OK) break;
            offset += sent;
          }
          if (sendResult == CURLE_OK && offset >= it->second.size()) continue;
          // A nonblocking socket can temporarily reject a write outright, or shutdown was
          // requested mid-send. Keep every unsent message, unless a newer one for its key
          // has been queued -- including this one if it was only partly written: a frame
          // can't be resumed mid-payload on a new connection, so keep the whole message.
          std::lock_guard lock(mutex_);
          for (; it != outbound.end(); ++it) pending_.emplace(it->first, it->second);
          break;
        }
        if (sendResult == CURLE_AGAIN) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          continue;
        }
        if (sendResult != CURLE_OK) {
          result = sendResult;
          break;
        }
        char buffer[65536];
        size_t received = 0;
        const struct curl_ws_frame *frame = nullptr;
        result = curl_ws_recv(handle, buffer, sizeof(buffer), &received, &frame);
        if (result == CURLE_AGAIN) {
          waitForSocket(handle);
          continue;
        }
        if (result != CURLE_OK || !frame) break;
        lastFrameAt = std::chrono::steady_clock::now();
        if (frame->flags & CURLWS_CLOSE) {
          if (received >= 2)
            closeCode = (static_cast<unsigned char>(buffer[0]) << 8) | static_cast<unsigned char>(buffer[1]);
          result = CURLE_GOT_NOTHING;
          break;
        }
        if (frame->flags & CURLWS_PING) {
          lastPingAt = lastFrameAt;
          size_t sent = 0;
          curl_ws_send(handle, buffer, received, &sent, 0, CURLWS_PONG);
          continue;
        }
        if ((frame->flags & CURLWS_TEXT) || !message.empty()) message.append(buffer, received);
        // bytesleft == 0 only means this frame's payload is fully read into buffer, not that
        // the message is complete: a message can span multiple frames, and CURLWS_CONT marks
        // every frame but the last one of such a message (per libcurl's curl_ws_recv docs).
        // Dispatching on frame boundaries alone hands onMessage incomplete JSON fragments.
        if (frame->bytesleft == 0 && !(frame->flags & CURLWS_CONT) && !message.empty()) {
          if (config_.onMessage) config_.onMessage(message);
          message.clear();
        }
      }
    }
    // Never log the URL or the headers: they carry ids and tokens.
    if (result != CURLE_OK && !stop.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();
      const auto ago = [&](std::chrono::steady_clock::time_point at) {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - at).count());
      };
      if (config_.log) {
        std::string line = config_.label + " disconnected: " + curl_easy_strerror(result);
        if (connected_) {
          const std::string closed = closeCode ? "close code " + std::to_string(closeCode) : "no close frame";
          const std::string ping = lastPingAt ? std::to_string(ago(*lastPingAt)) + " ms ago" : "none";
          line += " (" + closed + "; up " + std::to_string(ago(connectedAt)) + " ms, last frame " +
                  std::to_string(ago(lastFrameAt)) + " ms ago, last ping " + ping + ")";
        }
        config_.log(true, line + "; reconnecting");
      }
    }
    connected_ = false;
    if (config_.onDisconnected) config_.onDisconnected();
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    if (closeCode != 0 && config_.isFinalClose && config_.isFinalClose(closeCode)) {
      if (config_.onFinalClose) config_.onFinalClose(closeCode);
      return;
    }
    const bool stable = std::chrono::steady_clock::now() - connectedAt >= std::chrono::seconds(10);
    consecutiveFailures = stable ? 0 : (std::min)(consecutiveFailures + 1, 5u);
    const int backoffSteps = 20 * (1 << consecutiveFailures);
    for (int step = 0; step < backoffSteps && !stop.stop_requested(); ++step)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
