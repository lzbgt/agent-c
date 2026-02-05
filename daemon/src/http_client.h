#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace agentd {

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
  bool dns_pin
);

}  // namespace agentd
