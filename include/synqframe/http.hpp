// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#pragma once

#include <string>

// Small blocking HTTP calls: to control-api with a node's bearer token, and to services on
// this machine (MediaMTX's API) with none. Never logs the token, and callers never log URLs
// that carry one. Redirects are not followed: the token must not travel to wherever they point.
namespace common {

struct HttpResult {
  long status = 0; // 0: no response (connection failed or timed out)
  std::string body;
  std::string location; // where a 3xx points
  std::string server;   // the answer's Server header: who answered (control-api's nginx, or a proxy)
  std::string error;    // status 0: why, as libcurl says it ("Connection reset by peer", ...)
  bool timedOut = false; // status 0 because the call's time ran out
  bool ok() const { return status >= 200 && status < 300; }
  // "HTTP 503", or "no response (<why>)": for logs. Never holds the URL or the token.
  std::string describe() const;
};

struct HttpOptions {
  std::string token;     // empty: no Authorization header, and no proxy (a local service)
  long timeoutMs = 10'000;
  bool remote = false;   // a service on the internet even without a token: the proxy, if any, applies
  size_t maxBodyBytes = 0; // the answer's body is read no further than this (0: no cap); past it the call fails
};

HttpResult httpGet(const std::string &url, const std::string &token);
HttpResult httpPostJson(const std::string &url, const std::string &token, const std::string &body);
// method "GET", "POST", or another (DELETE, PATCH) sent as is; body null: none sent (a POST
// with an empty body). A call that failed before any answer (the connection refused, reset or
// closed, or the time ran out before the request went out; not a timeout waiting for the
// answer) is made once more: the node's calls are status reports, reads, and MediaMTX config
// changes that come out the same done twice, safe to repeat. The connect may take the call's
// whole timeoutMs.
HttpResult httpRequest(const std::string &method, const std::string &url, const std::string *body, const HttpOptions &options);

} // namespace common
