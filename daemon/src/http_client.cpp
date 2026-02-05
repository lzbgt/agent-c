#include "http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <vector>

namespace agentd {

namespace {

struct UrlHostPort {
  std::string host; // lowercased; IPv6 without brackets
  int port = 0;
};

static bool parse_http_url_target_hostport(const std::string& url_in, UrlHostPort* out) {
  if (!out) return false;
  *out = UrlHostPort{};

  const std::string url = url_in;
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return false;
  std::string scheme_s = url.substr(0, scheme);
  for (char& c : scheme_s) c = (char)std::tolower((unsigned char)c);
  if (scheme_s != "http" && scheme_s != "https") return false;

  size_t i = scheme + 3;
  size_t end = url.find_first_of("/?#", i);
  if (end == std::string::npos) end = url.size();
  std::string authority = url.substr(i, end - i);

  // Skip optional userinfo.
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) authority = authority.substr(at + 1);

  // Trim (best-effort).
  while (!authority.empty() && (authority.front() == ' ' || authority.front() == '\t')) authority.erase(authority.begin());
  while (!authority.empty() && (authority.back() == ' ' || authority.back() == '\t')) authority.pop_back();
  if (authority.empty()) return false;

  std::string host;
  std::string port_s;
  if (!authority.empty() && authority[0] == '[') {
    const size_t rb = authority.find(']');
    if (rb == std::string::npos) return false;
    host = authority.substr(1, rb - 1);
    if (rb + 1 < authority.size() && authority[rb + 1] == ':') port_s = authority.substr(rb + 2);
  } else {
    const size_t col = authority.rfind(':');
    if (col != std::string::npos && authority.find(':') == col) {
      host = authority.substr(0, col);
      port_s = authority.substr(col + 1);
    } else {
      host = authority;
    }
  }

  // Trim host and port (best-effort).
  while (!host.empty() && (host.front() == ' ' || host.front() == '\t')) host.erase(host.begin());
  while (!host.empty() && (host.back() == ' ' || host.back() == '\t')) host.pop_back();
  while (!port_s.empty() && (port_s.front() == ' ' || port_s.front() == '\t')) port_s.erase(port_s.begin());
  while (!port_s.empty() && (port_s.back() == ' ' || port_s.back() == '\t')) port_s.pop_back();
  if (host.empty()) return false;

  for (char& c : host) c = (char)std::tolower((unsigned char)c);

  int port = (scheme_s == "https") ? 443 : 80;
  if (!port_s.empty()) {
    try {
      const int p = std::stoi(port_s);
      if (p < 1 || p > 65535) return false;
      port = p;
    } catch (...) {
      return false;
    }
  }

  out->host = host;
  out->port = port;
  return true;
}

static bool host_is_literal_ip(const std::string& host) {
  if (host.empty()) return false;
  uint8_t buf[16];
  if (::inet_pton(AF_INET, host.c_str(), buf) == 1) return true;
  if (::inet_pton(AF_INET6, host.c_str(), buf) == 1) return true;
  return false;
}

static void resolve_host_best_effort(const std::string& host, std::vector<std::string>* out_addrs) {
  if (!out_addrs) return;
  out_addrs->clear();
  if (host.empty()) return;
  if (host_is_literal_ip(host)) {
    out_addrs->push_back(host);
    return;
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;
  struct addrinfo* res = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
  if (rc != 0 || !res) return;
  int kept = 0;
  for (struct addrinfo* p = res; p; p = p->ai_next) {
    if (kept >= 16) break;
    if (!p->ai_addr) continue;
    char ipbuf[INET6_ADDRSTRLEN + 1];
    std::memset(ipbuf, 0, sizeof(ipbuf));
    if (p->ai_family == AF_INET) {
      const struct sockaddr_in* sin = (const struct sockaddr_in*)p->ai_addr;
      if (!::inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof(ipbuf) - 1)) continue;
      out_addrs->push_back(std::string(ipbuf));
      kept++;
    } else if (p->ai_family == AF_INET6) {
      const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)p->ai_addr;
      if (!::inet_ntop(AF_INET6, &sin6->sin6_addr, ipbuf, sizeof(ipbuf) - 1)) continue;
      out_addrs->push_back(std::string(ipbuf));
      kept++;
    }
  }
  ::freeaddrinfo(res);
}

}  // namespace

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
  bool dns_pin
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

  // Defense-in-depth: pin hostname DNS results per request (best-effort).
  //
  // This mitigates DNS rebinding between an "allowlist check" and the actual connect performed by libcurl.
  // - Only applies to hostname targets (not literal IPs).
  // - Uses CURLOPT_RESOLVE to bypass DNS at connect-time (SNI/cert verification still use the hostname).
  struct curl_slist* resolve = nullptr;
  if (dns_pin) {
    UrlHostPort hp;
    if (parse_http_url_target_hostport(url, &hp) && !hp.host.empty() && !host_is_literal_ip(hp.host)) {
      std::vector<std::string> addrs;
      resolve_host_best_effort(hp.host, &addrs);
      for (const auto& ip : addrs) {
        if (ip.empty()) continue;
        resolve = curl_slist_append(resolve, (hp.host + ":" + std::to_string(hp.port) + ":" + ip).c_str());
      }
      if (resolve) {
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
      }
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
