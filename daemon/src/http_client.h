#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace agentd {

struct HttpClientPinnedResolve {
  // Hostname (lowercased) and port to pin for this request.
  // The host must match the request URL host (without IPv6 brackets).
  std::string host;
  int port = 0;
  // IP literals to pin (IPv4 like "1.2.3.4" or IPv6 like "2001:db8::1").
  // Empty means "no pinning".
  std::vector<std::string> addrs;
};

struct HttpClientResult {
  bool ok = false;
  long http_status = 0;
  std::string error;

  // Bounded by caller-provided max_response_bytes.
  std::string response_body;
  // Lowercased header keys.
  std::map<std::string, std::string> response_headers;

  // Parsed from Retry-After header when it is an integer number of seconds (best-effort).
  // -1 means unknown / not provided.
  int64_t retry_after_ms = -1;

  // Best-effort DNS resolution evidence (primarily used when outbound HTTP pinning is enabled).
  // - empty for literal IP targets and for requests without pinning
  // - capped internally (<=16)
  std::vector<std::string> resolved_addrs;

  bool timed_out = false;
  bool response_too_large = false;
};

// Minimal HTTP helper for deterministic workflow tasks.
//
// - Supports http/https (URL scheme must be validated by the caller).
// - Uses libcurl (already required by host build).
// - Bounded response capture (fail-closed when response exceeds max_response_bytes).
HttpClientResult http_request(
  const std::string& url,
  const std::string& method,
  const std::map<std::string, std::string>& headers,
  const std::string& body,
  int64_t timeout_ms,
  size_t max_response_bytes,
  const std::string& proxy_url,
  const HttpClientPinnedResolve* pinned_resolve_or_null
);

}  // namespace agentd
