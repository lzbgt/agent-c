#include "blob_object_store.h"

#include "agent/hmac_sha256.h"
#include "agent_sha256.h"
#include "string_util.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace agentd {
namespace {

struct PresignHeader {
  std::string name;
  std::string value;
};

struct PresignQueryParam {
  std::string name;
  std::string value;
};

struct ParsedEndpoint {
  std::string scheme;
  std::string host;
  int port = 0;
  std::string base_path;
  bool https = true;
};

static std::string to_lower_ascii(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static std::string trim_slashes(std::string s) {
  s = trim_copy(s);
  while (!s.empty() && s.front() == '/') s.erase(s.begin());
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

static std::string normalize_header_value(const std::string& s) {
  std::string out;
  std::string trimmed = trim_copy(s);
  out.reserve(trimmed.size());
  bool in_space = false;
  for (unsigned char c : trimmed) {
    if (c == ' ' || c == '\t') {
      if (!in_space) {
        out.push_back(' ');
        in_space = true;
      }
      continue;
    }
    in_space = false;
    out.push_back((char)c);
  }
  return out;
}

static bool parse_endpoint(const std::string& endpoint, ParsedEndpoint* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  out->scheme.clear();
  out->host.clear();
  out->port = 0;
  out->base_path.clear();
  out->https = true;
  const std::string ep = trim_copy(endpoint);
  const size_t scheme_sep = ep.find("://");
  if (scheme_sep == std::string::npos) {
    if (out_error) *out_error = "endpoint missing scheme";
    return false;
  }
  const std::string scheme = to_lower_ascii(ep.substr(0, scheme_sep));
  if (scheme != "http" && scheme != "https") {
    if (out_error) *out_error = "endpoint scheme must be http or https";
    return false;
  }
  const std::string rest = ep.substr(scheme_sep + 3);
  if (rest.empty()) {
    if (out_error) *out_error = "endpoint missing host";
    return false;
  }
  const size_t slash = rest.find('/');
  const std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  const std::string path = (slash == std::string::npos) ? "" : rest.substr(slash);
  if (hostport.empty()) {
    if (out_error) *out_error = "endpoint missing host";
    return false;
  }
  std::string host;
  std::string port_s;
  if (!hostport.empty() && hostport.front() == '[') {
    const size_t rb = hostport.find(']');
    if (rb == std::string::npos) {
      if (out_error) *out_error = "endpoint invalid ipv6 host";
      return false;
    }
    host = hostport.substr(1, rb - 1);
    if (rb + 1 < hostport.size()) {
      if (hostport[rb + 1] != ':') {
        if (out_error) *out_error = "endpoint invalid host:port";
        return false;
      }
      port_s = hostport.substr(rb + 2);
    }
  } else {
    const size_t col = hostport.rfind(':');
    if (col != std::string::npos && hostport.find(':') == col) {
      host = hostport.substr(0, col);
      port_s = hostport.substr(col + 1);
    } else {
      host = hostport;
    }
  }
  host = trim_copy(host);
  port_s = trim_copy(port_s);
  if (host.empty()) {
    if (out_error) *out_error = "endpoint host empty";
    return false;
  }
  int port = 0;
  if (!port_s.empty()) {
    try {
      port = std::stoi(port_s);
    } catch (...) {
      if (out_error) *out_error = "endpoint port invalid";
      return false;
    }
    if (port < 1 || port > 65535) {
      if (out_error) *out_error = "endpoint port invalid";
      return false;
    }
  } else {
    port = (scheme == "https") ? 443 : 80;
  }
  std::string base_path = path;
  if (base_path == "/") base_path.clear();
  if (!base_path.empty()) {
    while (base_path.size() > 1 && base_path.back() == '/') base_path.pop_back();
    if (!base_path.empty() && base_path.front() != '/') base_path = "/" + base_path;
  }
  out->scheme = scheme;
  out->host = host;
  out->port = port;
  out->base_path = base_path;
  out->https = (scheme == "https");
  return true;
}

static std::string uri_encode(const std::string& s, bool encode_slash) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    const bool unreserved =
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved || (!encode_slash && c == '/')) {
      out.push_back((char)c);
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

static bool format_amz_dates(int64_t now_utc_ms, std::string* out_amz_date, std::string* out_date) {
  if (out_amz_date) out_amz_date->clear();
  if (out_date) out_date->clear();
  const time_t t = (time_t)(now_utc_ms / 1000);
  std::tm tm{};
#if defined(_WIN32)
  if (gmtime_s(&tm, &t) != 0) return false;
#else
  if (!gmtime_r(&t, &tm)) return false;
#endif
  char date_buf[9];
  char amz_buf[17];
  if (std::snprintf(date_buf, sizeof(date_buf), "%04d%02d%02d",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) <= 0) {
    return false;
  }
  if (std::snprintf(amz_buf, sizeof(amz_buf), "%04d%02d%02dT%02d%02d%02dZ",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec) <= 0) {
    return false;
  }
  if (out_date) *out_date = date_buf;
  if (out_amz_date) *out_amz_date = amz_buf;
  return true;
}

static std::string sha256_hex(const std::string& s) {
  char hex[65];
  agent_sha256_hex_of_bytes(s.data(), s.size(), hex);
  return std::string(hex);
}

static void hmac_sha256_bytes(const uint8_t* key, size_t key_len, const std::string& msg, uint8_t out32[32]) {
  agent_hmac_sha256(key, key_len, msg.data(), msg.size(), out32);
}

static std::string hex_of_bytes(const uint8_t* bytes, size_t n) {
  static const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    const uint8_t b = bytes[i];
    out.push_back(hex[(b >> 4) & 0xF]);
    out.push_back(hex[b & 0xF]);
  }
  return out;
}

static bool build_s3_target(
  const DaemonConfig& cfg,
  const std::string& key_in,
  ParsedEndpoint* out_ep,
  std::string* out_host,
  std::string* out_canonical_uri,
  std::string* out_url,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_ep || !out_host || !out_canonical_uri || !out_url) return false;
  if (cfg.blob_store_endpoint.empty()) {
    if (out_error) *out_error = "blob_store_endpoint is empty";
    return false;
  }
  if (cfg.blob_store_bucket.empty()) {
    if (out_error) *out_error = "blob_store_bucket is empty";
    return false;
  }
  if (!parse_endpoint(cfg.blob_store_endpoint, out_ep, out_error)) return false;

  std::string key = key_in;
  while (!key.empty() && key.front() == '/') key.erase(key.begin());

  std::string host = cfg.blob_store_path_style
    ? out_ep->host
    : (cfg.blob_store_bucket + "." + out_ep->host);

  const bool default_port = (out_ep->https && out_ep->port == 443) || (!out_ep->https && out_ep->port == 80);
  const std::string hostport = default_port ? host : (host + ":" + std::to_string(out_ep->port));

  std::string path = out_ep->base_path;
  auto append_seg = [&](const std::string& seg) {
    if (seg.empty()) return;
    if (path.empty() || path.back() != '/') path.push_back('/');
    path += seg;
  };
  if (cfg.blob_store_path_style) {
    append_seg(cfg.blob_store_bucket);
  }
  append_seg(key);
  if (path.empty()) path = "/";
  if (path.front() != '/') path = "/" + path;

  const std::string canonical_uri = uri_encode(path, /*encode_slash=*/false);

  *out_host = hostport;
  *out_canonical_uri = canonical_uri;
  *out_url = out_ep->scheme + "://" + hostport + canonical_uri;
  return true;
}

static bool parse_http_date_utc(const std::string& s, int64_t* out_ms) {
  if (out_ms) *out_ms = -1;
  if (!out_ms) return false;
  char wday[4] = {0};
  char mon[4] = {0};
  char tz[4] = {0};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (std::sscanf(s.c_str(), "%3s, %d %3s %d %d:%d:%d %3s", wday, &day, mon, &year, &hour, &minute, &second, tz) != 8) {
    return false;
  }
  if (std::strcmp(tz, "GMT") != 0) return false;
  std::string mon_s = to_lower_ascii(mon);
  int mon_idx = -1;
  if (mon_s == "jan") mon_idx = 0;
  else if (mon_s == "feb") mon_idx = 1;
  else if (mon_s == "mar") mon_idx = 2;
  else if (mon_s == "apr") mon_idx = 3;
  else if (mon_s == "may") mon_idx = 4;
  else if (mon_s == "jun") mon_idx = 5;
  else if (mon_s == "jul") mon_idx = 6;
  else if (mon_s == "aug") mon_idx = 7;
  else if (mon_s == "sep") mon_idx = 8;
  else if (mon_s == "oct") mon_idx = 9;
  else if (mon_s == "nov") mon_idx = 10;
  else if (mon_s == "dec") mon_idx = 11;
  if (mon_idx < 0) return false;
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = mon_idx;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
#if defined(_WIN32)
  time_t t = _mkgmtime(&tm);
#else
  time_t t = timegm(&tm);
#endif
  if (t < 0) return false;
  *out_ms = static_cast<int64_t>(t) * 1000;
  return true;
}

static bool presign_url_internal(
  const DaemonConfig& cfg,
  const std::string& method,
  const std::string& key,
  int64_t now_utc_ms,
  int64_t expires_sec,
  const std::vector<PresignQueryParam>& extra_query,
  const std::vector<PresignHeader>& extra_headers,
  std::string* out_url,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_url) out_url->clear();
  std::string err;
  if (!blob_object_store_is_configured(cfg, &err)) {
    if (out_error) *out_error = err.empty() ? "object store not configured" : err;
    return false;
  }
  ParsedEndpoint ep;
  std::string host;
  std::string canonical_uri;
  std::string base_url;
  if (!build_s3_target(cfg, key, &ep, &host, &canonical_uri, &base_url, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to build object store URL" : err;
    return false;
  }

  std::string amz_date;
  std::string date_stamp;
  if (!format_amz_dates(now_utc_ms, &amz_date, &date_stamp)) {
    if (out_error) *out_error = "failed to format timestamp";
    return false;
  }

  const std::string region = cfg.blob_store_region.empty() ? "us-east-1" : cfg.blob_store_region;
  const int64_t ttl = std::max<int64_t>(1, std::min<int64_t>(604800, expires_sec));
  const std::string scope = date_stamp + "/" + region + "/s3/aws4_request";
  const std::string credential = cfg.blob_store_access_key + "/" + scope;

  std::vector<std::pair<std::string, std::string>> params;
  params.emplace_back("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
  params.emplace_back("X-Amz-Credential", credential);
  params.emplace_back("X-Amz-Date", amz_date);
  params.emplace_back("X-Amz-Expires", std::to_string(ttl));

  std::vector<std::pair<std::string, std::string>> header_pairs;
  header_pairs.emplace_back("host", normalize_header_value(to_lower_ascii(host)));
  for (const auto& h : extra_headers) {
    const std::string name = to_lower_ascii(trim_copy(h.name));
    if (name.empty()) continue;
    header_pairs.emplace_back(name, normalize_header_value(h.value));
  }
  std::sort(header_pairs.begin(), header_pairs.end(), [](const auto& a, const auto& b) {
    if (a.first < b.first) return true;
    if (a.first > b.first) return false;
    return a.second < b.second;
  });
  std::string canonical_headers;
  std::string signed_headers;
  for (size_t i = 0; i < header_pairs.size(); i++) {
    canonical_headers.append(header_pairs[i].first);
    canonical_headers.push_back(':');
    canonical_headers.append(header_pairs[i].second);
    canonical_headers.push_back('\n');
    if (i > 0) signed_headers.push_back(';');
    signed_headers.append(header_pairs[i].first);
  }
  params.emplace_back("X-Amz-SignedHeaders", signed_headers);
  if (!cfg.blob_store_session_token.empty()) {
    params.emplace_back("X-Amz-Security-Token", cfg.blob_store_session_token);
  }
  for (const auto& qp : extra_query) {
    params.emplace_back(qp.name, qp.value);
  }

  std::vector<std::pair<std::string, std::string>> enc;
  enc.reserve(params.size());
  for (const auto& kv : params) {
    enc.emplace_back(uri_encode(kv.first, true), uri_encode(kv.second, true));
  }
  std::sort(enc.begin(), enc.end(), [](const auto& a, const auto& b) {
    if (a.first < b.first) return true;
    if (a.first > b.first) return false;
    return a.second < b.second;
  });
  std::string canonical_query;
  for (size_t i = 0; i < enc.size(); i++) {
    if (i > 0) canonical_query.push_back('&');
    canonical_query.append(enc[i].first);
    canonical_query.push_back('=');
    canonical_query.append(enc[i].second);
  }

  const std::string payload_hash = "UNSIGNED-PAYLOAD";

  const std::string canonical_request =
    method + "\n" +
    canonical_uri + "\n" +
    canonical_query + "\n" +
    canonical_headers + "\n" +
    signed_headers + "\n" +
    payload_hash;

  const std::string string_to_sign =
    std::string("AWS4-HMAC-SHA256\n") +
    amz_date + "\n" +
    scope + "\n" +
    sha256_hex(canonical_request);

  const std::string k_secret = "AWS4" + cfg.blob_store_secret_key;
  std::array<uint8_t, 32> k_date{};
  std::array<uint8_t, 32> k_region{};
  std::array<uint8_t, 32> k_service{};
  std::array<uint8_t, 32> k_signing{};
  hmac_sha256_bytes(reinterpret_cast<const uint8_t*>(k_secret.data()), k_secret.size(), date_stamp, k_date.data());
  hmac_sha256_bytes(k_date.data(), k_date.size(), region, k_region.data());
  hmac_sha256_bytes(k_region.data(), k_region.size(), "s3", k_service.data());
  hmac_sha256_bytes(k_service.data(), k_service.size(), "aws4_request", k_signing.data());
  std::array<uint8_t, 32> sig{};
  hmac_sha256_bytes(k_signing.data(), k_signing.size(), string_to_sign, sig.data());

  const std::string signature = hex_of_bytes(sig.data(), sig.size());
  const std::string url = base_url + "?" + canonical_query + "&X-Amz-Signature=" + signature;
  if (out_url) *out_url = url;
  return true;
}

}  // namespace

bool blob_object_store_is_configured(const DaemonConfig& cfg, std::string* out_error) {
  if (out_error) out_error->clear();
  if (cfg.blob_store_endpoint.empty()) {
    if (out_error) *out_error = "blob_store_endpoint not set";
    return false;
  }
  if (cfg.blob_store_bucket.empty()) {
    if (out_error) *out_error = "blob_store_bucket not set";
    return false;
  }
  if (cfg.blob_store_access_key.empty()) {
    if (out_error) *out_error = "blob_store_access_key not set";
    return false;
  }
  if (cfg.blob_store_secret_key.empty()) {
    if (out_error) *out_error = "blob_store_secret_key not set";
    return false;
  }
  return true;
}

bool blob_object_store_is_enabled(const DaemonConfig& cfg, std::string* out_error) {
  if (cfg.blob_store_mode != "object") {
    if (out_error) out_error->clear();
    return false;
  }
  return blob_object_store_is_configured(cfg, out_error);
}

std::string blob_object_store_key_for_hex(const DaemonConfig& cfg, const std::string& hex) {
  if (hex.size() != 64) return "";
  std::string prefix = trim_slashes(cfg.blob_store_prefix.empty() ? "blobs/sha256" : cfg.blob_store_prefix);
  std::string out;
  if (!prefix.empty()) {
    out = prefix + "/";
  }
  out += hex.substr(0, 2);
  out.push_back('/');
  out += hex.substr(2, 2);
  out.push_back('/');
  out += hex;
  return out;
}

std::string blob_object_store_key_for_blob_id(const DaemonConfig& cfg, const std::string& blob_id) {
  const std::string prefix = "sha256:";
  if (blob_id.rfind(prefix, 0) != 0) return "";
  return blob_object_store_key_for_hex(cfg, blob_id.substr(prefix.size()));
}

bool blob_object_store_presign_url(
  const DaemonConfig& cfg,
  const std::string& method,
  const std::string& key,
  int64_t now_utc_ms,
  int64_t expires_sec,
  std::string* out_url,
  std::string* out_error
) {
  return presign_url_internal(
    cfg,
    method,
    key,
    now_utc_ms,
    expires_sec,
    {},
    {},
    out_url,
    out_error
  );
}

bool blob_object_store_put(
  const DaemonConfig& cfg,
  const std::string& key,
  const std::string& body,
  const std::string& mime,
  int64_t now_utc_ms,
  std::string* out_etag,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_etag) out_etag->clear();
  std::string url;
  std::string err;
  if (!blob_object_store_presign_url(cfg, "PUT", key, now_utc_ms, cfg.blob_store_presign_ttl_sec, &url, &err)) {
    if (out_error) *out_error = err.empty() ? "presign failed" : err;
    return false;
  }
  std::map<std::string, std::string> headers;
  if (!mime.empty()) headers["Content-Type"] = mime;
  HttpClientResult res = http_request(
    url,
    "PUT",
    headers,
    body,
    cfg.blob_store_timeout_ms,
    /*max_response_bytes=*/64 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve=*/nullptr
  );
  if (!res.ok || (res.http_status < 200 || res.http_status >= 300)) {
    if (out_error) {
      *out_error = res.error.empty()
        ? ("object store PUT failed: status " + std::to_string(res.http_status))
        : res.error;
    }
    return false;
  }
  auto it = res.response_headers.find("etag");
  if (it != res.response_headers.end()) {
    if (out_etag) *out_etag = it->second;
  }
  return true;
}

bool blob_object_store_delete(
  const DaemonConfig& cfg,
  const std::string& key,
  int64_t now_utc_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  std::string url;
  std::string err;
  if (!blob_object_store_presign_url(cfg, "DELETE", key, now_utc_ms, cfg.blob_store_presign_ttl_sec, &url, &err)) {
    if (out_error) *out_error = err.empty() ? "presign failed" : err;
    return false;
  }
  HttpClientResult res = http_request(
    url,
    "DELETE",
    {},
    "",
    cfg.blob_store_timeout_ms,
    /*max_response_bytes=*/64 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve=*/nullptr
  );
  if (res.http_status == 404) {
    return true;
  }
  if (!res.ok || (res.http_status < 200 || res.http_status >= 300)) {
    if (out_error) {
      *out_error = res.error.empty()
        ? ("object store DELETE failed: status " + std::to_string(res.http_status))
        : res.error;
    }
    return false;
  }
  return true;
}

bool blob_object_store_get(
  const DaemonConfig& cfg,
  const std::string& key,
  const std::string& range_header,
  size_t max_bytes,
  int64_t now_utc_ms,
  HttpClientResult* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_result) *out_result = HttpClientResult();
  std::string url;
  std::string err;
  if (!blob_object_store_presign_url(cfg, "GET", key, now_utc_ms, cfg.blob_store_presign_ttl_sec, &url, &err)) {
    if (out_error) *out_error = err.empty() ? "presign failed" : err;
    return false;
  }
  std::map<std::string, std::string> headers;
  if (!range_header.empty()) {
    headers["Range"] = range_header;
  }
  HttpClientResult res = http_request(
    url,
    "GET",
    headers,
    "",
    cfg.blob_store_timeout_ms,
    max_bytes > 0 ? max_bytes : (size_t)(32 * 1024 * 1024),
    /*proxy_url=*/"",
    /*pinned_resolve=*/nullptr
  );
  if (out_result) *out_result = res;
  if (!res.ok || (res.http_status != 200 && res.http_status != 206)) {
    if (out_error) {
      *out_error = res.error.empty()
        ? ("object store GET failed: status " + std::to_string(res.http_status))
        : res.error;
    }
    return false;
  }
  return true;
}

bool blob_object_store_head(
  const DaemonConfig& cfg,
  const std::string& key,
  int64_t now_utc_ms,
  HttpClientResult* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_result) *out_result = HttpClientResult();
  std::string url;
  std::string err;
  if (!blob_object_store_presign_url(cfg, "HEAD", key, now_utc_ms, cfg.blob_store_presign_ttl_sec, &url, &err)) {
    if (out_error) *out_error = err.empty() ? "presign failed" : err;
    return false;
  }
  HttpClientResult res = http_request(
    url,
    "HEAD",
    {},
    "",
    cfg.blob_store_timeout_ms,
    /*max_response_bytes=*/8 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve=*/nullptr
  );
  if (out_result) *out_result = res;
  if (!res.ok || res.http_status < 200 || res.http_status >= 300) {
    if (out_error) {
      *out_error = res.error.empty()
        ? ("object store HEAD failed: status " + std::to_string(res.http_status))
        : res.error;
    }
    return false;
  }
  return true;
}

bool blob_object_store_copy(
  const DaemonConfig& cfg,
  const std::string& key,
  const std::string& source_key,
  const std::string& storage_class,
  int64_t now_utc_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!blob_object_store_is_configured(cfg, out_error)) return false;
  if (cfg.blob_store_bucket.empty()) {
    if (out_error) *out_error = "blob_store_bucket is empty";
    return false;
  }
  std::string src = "/" + cfg.blob_store_bucket;
  if (!source_key.empty() && source_key.front() != '/') {
    src.push_back('/');
  }
  src += source_key;
  const std::string copy_source = uri_encode(src, /*encode_slash=*/false);

  std::vector<PresignHeader> signed_headers;
  signed_headers.push_back({"x-amz-copy-source", copy_source});
  if (!storage_class.empty()) {
    signed_headers.push_back({"x-amz-storage-class", storage_class});
  }
  std::string url;
  std::string err;
  if (!presign_url_internal(
        cfg,
        "PUT",
        key,
        now_utc_ms,
        cfg.blob_store_presign_ttl_sec,
        {},
        signed_headers,
        &url,
        &err)) {
    if (out_error) *out_error = err.empty() ? "presign failed" : err;
    return false;
  }
  std::map<std::string, std::string> headers;
  headers["x-amz-copy-source"] = copy_source;
  if (!storage_class.empty()) {
    headers["x-amz-storage-class"] = storage_class;
  }
  HttpClientResult res = http_request(
    url,
    "PUT",
    headers,
    "",
    cfg.blob_store_timeout_ms,
    /*max_response_bytes=*/256 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve=*/nullptr
  );
  if (!res.ok || res.http_status < 200 || res.http_status >= 300) {
    if (out_error) {
      *out_error = res.error.empty()
        ? ("object store COPY failed: status " + std::to_string(res.http_status))
        : res.error;
    }
    return false;
  }
  return true;
}

bool blob_object_store_restore(
  const DaemonConfig& cfg,
  const std::string& key,
  int64_t now_utc_ms,
  int restore_days,
  const std::string& restore_tier,
  HttpClientResult* out_result,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_result) *out_result = HttpClientResult();
  const int days = restore_days > 0 ? restore_days : 1;
  std::string tier = restore_tier.empty() ? "Standard" : restore_tier;
  std::string body = "<RestoreRequest xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
  body += "<Days>" + std::to_string(days) + "</Days>";
  body += "<GlacierJobParameters><Tier>" + tier + "</Tier></GlacierJobParameters>";
  body += "</RestoreRequest>";

  std::vector<PresignQueryParam> extra_query;
  extra_query.push_back({"restore", ""});
  std::string url;
  std::string err;
  if (!presign_url_internal(
        cfg,
        "POST",
        key,
        now_utc_ms,
        cfg.blob_store_presign_ttl_sec,
        extra_query,
        {},
        &url,
        &err)) {
    if (out_error) *out_error = err.empty() ? "presign failed" : err;
    return false;
  }
  std::map<std::string, std::string> headers;
  headers["Content-Type"] = "application/xml";
  HttpClientResult res = http_request(
    url,
    "POST",
    headers,
    body,
    cfg.blob_store_timeout_ms,
    /*max_response_bytes=*/256 * 1024,
    /*proxy_url=*/"",
    /*pinned_resolve=*/nullptr
  );
  if (out_result) *out_result = res;
  if (!res.ok || (res.http_status != 200 && res.http_status != 202)) {
    if (out_error) {
      *out_error = res.error.empty()
        ? ("object store RESTORE failed: status " + std::to_string(res.http_status))
        : res.error;
    }
    return false;
  }
  return true;
}

bool blob_object_store_parse_restore_header(const std::string& value, BlobObjectRestoreState* out) {
  if (!out) return false;
  *out = BlobObjectRestoreState();
  out->raw_header = value;
  if (value.empty()) return true;
  out->has_header = true;
  const std::string lower = to_lower_ascii(value);
  const std::string ongoing_key = "ongoing-request=\"";
  size_t pos = lower.find(ongoing_key);
  if (pos != std::string::npos) {
    out->ongoing_known = true;
    pos += ongoing_key.size();
    size_t end = lower.find('"', pos);
    if (end != std::string::npos && end > pos) {
      const std::string flag = lower.substr(pos, end - pos);
      out->ongoing = (flag == "true");
    }
  }
  const std::string expiry_key = "expiry-date=\"";
  pos = lower.find(expiry_key);
  if (pos != std::string::npos) {
    pos += expiry_key.size();
    size_t end = lower.find('"', pos);
    if (end != std::string::npos && end > pos) {
      const std::string expiry = value.substr(pos, end - pos);
      int64_t expiry_ms = -1;
      if (parse_http_date_utc(expiry, &expiry_ms)) {
        out->expiry_utc_ms = expiry_ms;
      }
    }
  }
  return true;
}

}  // namespace agentd
