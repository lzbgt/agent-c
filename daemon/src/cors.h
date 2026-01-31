#pragma once

#include "agentd/http_types.h"

#include <string>
#include <vector>

namespace agentd {

struct CorsConfig {
  // Allowed origins. Special case: "*" means allow any origin and respond with "*".
  // If empty, CORS is disabled (no CORS headers are emitted).
  std::vector<std::string> origins;

  // Comma-separated allowlists used both for responses and preflight.
  std::string allow_methods;  // e.g. "GET, POST, DELETE, OPTIONS"
  std::string allow_headers;  // e.g. "Content-Type, Authorization, X-OpenRouter-Key"

  int max_age_seconds = 600;
};

void cors_apply(const HttpRequest& req, HttpResponse* resp, const CorsConfig& cfg);

// Returns header lines (each ending with "\r\n") suitable for writing into a raw HTTP response.
// Returns an empty string when CORS is disabled or the request Origin is not allowed.
std::string cors_wire_headers(const HttpRequest& req, const CorsConfig& cfg);

}  // namespace agentd
