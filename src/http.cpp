// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Synqframe

#include <synqframe/http.hpp>

#include <curl/curl.h>

#include <algorithm>

namespace common {
namespace {

struct Body {
  std::string *text;
  size_t max; // 0: no cap
};

// Past the cap the write is refused, which fails the call (CURLE_WRITE_ERROR): nothing larger
// than asked for is ever held.
size_t collect(char *data, size_t size, size_t count, void *out)
{
  auto &body = *static_cast<Body *>(out);
  if (body.max && body.text->size() + size * count > body.max) return 0;
  body.text->append(data, size * count);
  return size * count;
}

size_t header(char *data, size_t size, size_t count, void *out)
{
  const std::string line(data, size * count);
  if (line.size() > 7 && (line.compare(0, 7, "Server:") == 0 || line.compare(0, 7, "server:") == 0)) {
    std::string value = line.substr(7);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();
    *static_cast<std::string *>(out) = value;
  }
  return size * count;
}

// A failure before any answer that a second try can get past: the far end (or a proxy in
// between) refused, reset or closed the connection, or the time ran out before the request
// went out at all (sent: false), as when SYNs to the proxy are lost (seen as
// connects that took 1, 3 or 7 s). A timeout after the request went out is not one.
bool worthRetrying(CURLcode code, bool sent)
{
  return code == CURLE_COULDNT_CONNECT || code == CURLE_SEND_ERROR || code == CURLE_RECV_ERROR || code == CURLE_GOT_NOTHING ||
         code == CURLE_SSL_CONNECT_ERROR || code == CURLE_PARTIAL_FILE || (code == CURLE_OPERATION_TIMEDOUT && !sent);
}

HttpResult attempt(const std::string &method, const std::string &url, const std::string *body, const HttpOptions &options,
                   CURLcode &code, bool &sent)
{
  HttpResult result;
  code = CURLE_FAILED_INIT;
  sent = false;
  CURL *handle = curl_easy_init();
  if (!handle) {
    result.error = "libcurl did not start";
    return result;
  }
  curl_slist *headers = nullptr;
  if (!options.token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + options.token).c_str());
  else if (!options.remote) curl_easy_setopt(handle, CURLOPT_NOPROXY, "*"); // a service on this machine
  if (body) headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  // The connect has the call's whole time: a lost SYN is resent after 1 s and again 2 s later,
  // so a 3 s cap gave up just as the second resend would have connected.
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, options.timeoutMs);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, options.timeoutMs);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  Body body_{&result.body, options.maxBodyBytes};
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body_);
  if (options.maxBodyBytes) curl_easy_setopt(handle, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(options.maxBodyBytes));
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, header);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &result.server);
  // libcurl sends none by default, and some proxies and firewalls refuse a request without one.
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "synqframe");
  if (method == "POST") {
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body ? body->c_str() : "");
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, body ? static_cast<long>(body->size()) : 0L);
  } else if (method != "GET") {
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (body) {
      curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body->c_str());
      curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(body->size()));
    }
  }
  code = curl_easy_perform(handle);
  if (code == CURLE_OK) {
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
    char *location = nullptr;
    if (curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &location) == CURLE_OK && location) result.location = location;
  } else {
    result.error = curl_easy_strerror(code);
    result.timedOut = code == CURLE_OPERATION_TIMEDOUT;
    // Zero until the connection (and its TLS) was up and the request about to go out.
    double pretransfer = 0;
    sent = curl_easy_getinfo(handle, CURLINFO_PRETRANSFER_TIME, &pretransfer) != CURLE_OK || pretransfer > 0;
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  return result;
}

} // namespace

std::string HttpResult::describe() const
{
  if (status) return "HTTP " + std::to_string(status);
  return error.empty() ? "no response" : "no response (" + error + ")";
}

HttpResult httpRequest(const std::string &method, const std::string &url, const std::string *body, const HttpOptions &options)
{
  CURLcode code = CURLE_OK;
  bool sent = false;
  HttpResult result = attempt(method, url, body, options, code, sent);
  if (!result.status && worthRetrying(code, sent)) {
    const std::string first = result.error;
    result = attempt(method, url, body, options, code, sent);
    if (!result.status) result.error += ", and before that: " + first;
  }
  return result;
}

HttpResult httpGet(const std::string &url, const std::string &token)
{
  return httpRequest("GET", url, nullptr, {token});
}

HttpResult httpPostJson(const std::string &url, const std::string &token, const std::string &body)
{
  return httpRequest("POST", url, &body, {token});
}

} // namespace common
