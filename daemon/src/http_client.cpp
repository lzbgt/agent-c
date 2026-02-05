#include "http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>

namespace agentd {

static void ensure_curl_global_init() {
  static std::once_flag once;
  std::call_once(once, []() {
    (void)curl_global_init(CURL_GLOBAL_DEFAULT);
  });
}

static const char* pick_env_proxy() {
  const char* p = std::getenv("HTTPS_PROXY");
  if (!p || !p[0]) p = std::getenv("https_proxy");
  if (!p || !p[0]) p = std::getenv("HTTP_PROXY");
  if (!p || !p[0]) p = std::getenv("http_proxy");
  return (p && p[0]) ? p : nullptr;
}

static std::string lowercase_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

struct WriteCtx {
  std::string* out = nullptr;
  size_t cap = 0;
  bool too_large = false;
};

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<WriteCtx*>(userdata);
  const size_t n = size * nmemb;
  if (!ctx || !ctx->out || !ptr || n == 0) return n;

  const size_t cur = ctx->out->size();
  if (cur + n > ctx->cap) {
    ctx->too_large = true;
    // Returning 0 aborts the transfer (CURLE_WRITE_ERROR), which we treat as a fail-closed cap violation.
    return 0;
  }
  ctx->out->append(ptr, ptr + n);
  return n;
}

struct HeaderCtx {
  std::map<std::string, std::string>* out = nullptr;
  int64_t retry_after_ms = -1;
};

static std::optional<int64_t> parse_retry_after_seconds(std::string_view v) {
  // RFC allows HTTP-date, but we only parse integer seconds (best-effort).
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n')) v.remove_suffix(1);
  if (v.empty()) return std::nullopt;
  int64_t sec = 0;
  for (char c : v) {
    if (c < '0' || c > '9') return std::nullopt;
    sec = sec * 10 + (c - '0');
    if (sec > 3600) break;
  }
  return sec;
}

static size_t header_cb(char* buffer, size_t size, size_t nmemb, void* userdata) {
  auto* ctx = static_cast<HeaderCtx*>(userdata);
  const size_t n = size * nmemb;
  if (!ctx || !ctx->out || !buffer || n == 0) return n;

  std::string_view line(buffer, n);
  // Ignore status line.
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) return n;

  std::string key(line.substr(0, colon));
  std::string value(line.substr(colon + 1));
  // Trim.
  while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();

  key = lowercase_ascii(std::move(key));
  (*ctx->out)[key] = value;

  if (key == "retry-after") {
    if (auto sec = parse_retry_after_seconds(value)) {
      ctx->retry_after_ms = (*sec) * 1000;
    }
  }
  return n;
}

HttpClientResult http_request(
  const std::string& url,
  const std::string& method,
  const std::map<std::string, std::string>& headers,
  const std::string& body,
  int64_t timeout_ms,
  size_t max_response_bytes,
  const std::string& proxy_url,
  const HttpClientPinnedResolve* pinned_resolve_or_null
) {
  HttpClientResult out;
  out.http_status = 0;
  out.retry_after_ms = -1;

  ensure_curl_global_init();

  if (url.empty()) {
    out.error = "url is empty";
    return out;
  }
  if (method.empty()) {
    out.error = "method is empty";
    return out;
  }
  if (max_response_bytes == 0) {
    out.error = "max_response_bytes is 0";
    return out;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    out.error = "curl_easy_init failed";
    return out;
  }

  struct curl_slist* hdrs = nullptr;
  for (const auto& kv : headers) {
    if (kv.first.empty()) continue;
    hdrs = curl_slist_append(hdrs, (kv.first + ": " + kv.second).c_str());
  }

  std::string response_body;
  response_body.reserve(std::min<size_t>(max_response_bytes, 64 * 1024));
  WriteCtx wctx;
  wctx.out = &response_body;
  wctx.cap = max_response_bytes;

  std::map<std::string, std::string> response_headers;
  HeaderCtx hctx;
  hctx.out = &response_headers;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);
  // Make timeouts reliable even when used from background threads.
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  // Defense-in-depth: pin DNS results (best-effort) to mitigate DNS rebinding between policy check and connect.
  // Uses CURLOPT_RESOLVE to bypass DNS at connect-time (SNI/cert verification still use the hostname).
  struct curl_slist* resolve = nullptr;
  if (pinned_resolve_or_null && !pinned_resolve_or_null->host.empty() && pinned_resolve_or_null->port > 0 &&
      !pinned_resolve_or_null->addrs.empty()) {
    out.resolved_addrs = pinned_resolve_or_null->addrs;
    for (const auto& ip_raw : pinned_resolve_or_null->addrs) {
      if (ip_raw.empty()) continue;
      const bool is_v6 = (ip_raw.find(':') != std::string::npos);
      const std::string ip = is_v6 ? ("[" + ip_raw + "]") : ip_raw;
      resolve = curl_slist_append(
        resolve,
        (pinned_resolve_or_null->host + ":" + std::to_string(pinned_resolve_or_null->port) + ":" + ip).c_str()
      );
    }
    if (resolve) {
      curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    }
  }

  if (timeout_ms > 0) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
  }

  const char* proxy = nullptr;
  if (!proxy_url.empty()) proxy = proxy_url.c_str();
  else proxy = pick_env_proxy();
  if (proxy && proxy[0]) {
    curl_easy_setopt(curl, CURLOPT_PROXY, proxy);
  }

  std::string m = method;
  for (char& c : m) c = (char)std::toupper((unsigned char)c);
  if (m == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (m == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  } else {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, m.c_str());
    if (!body.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }
  }

  const CURLcode rc = curl_easy_perform(curl);
  if (wctx.too_large) {
    out.response_too_large = true;
    out.error = "response exceeded max_bytes cap";
  } else if (rc != CURLE_OK) {
    out.error = curl_easy_strerror(rc);
    if (rc == CURLE_OPERATION_TIMEDOUT) {
      out.timed_out = true;
    }
  }

  long status = 0;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  out.http_status = status;

  if (resolve) curl_slist_free_all(resolve);
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(curl);

  out.response_body = std::move(response_body);
  out.response_headers = std::move(response_headers);
  out.retry_after_ms = hctx.retry_after_ms;

  if (out.error.empty()) {
    out.ok = true;
  }
  return out;
}

}  // namespace agentd
