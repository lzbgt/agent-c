#pragma once

#include "agentd/http_types.h"

#include <map>
#include <regex>
#include <string>
#include <vector>

namespace agentd {

struct CorsOriginMatcher {
  bool any = false;
  std::map<std::string, bool> exact;
  std::vector<std::regex> regex;
};

struct CorsRoute {
  std::string path_prefix;
  std::vector<std::string> origins;
  CorsOriginMatcher matcher;
};

struct CorsConfig {
  // Allowed origins. Special case: "*" means allow any origin and respond with "*".
  // If empty, CORS is disabled (no CORS headers are emitted).
  std::vector<std::string> origins;
  CorsOriginMatcher matcher;
  // Optional per-route origin policies (path prefix match, longest prefix wins).
  std::vector<CorsRoute> routes;

  // Comma-separated allowlists used both for responses and preflight.
  std::string allow_methods;  // e.g. "GET, POST, DELETE, OPTIONS"
  std::string allow_headers;  // e.g. "Content-Type, Authorization, X-OpenRouter-Key"

  bool allow_credentials = false;
  int max_age_seconds = 600;
};

void cors_compile(CorsConfig* cfg);

void cors_apply(const HttpRequest& req, HttpResponse* resp, const CorsConfig& cfg);

// Returns header lines (each ending with "\r\n") suitable for writing into a raw HTTP response.
// Returns an empty string when CORS is disabled or the request Origin is not allowed.
std::string cors_wire_headers(const HttpRequest& req, const CorsConfig& cfg);

}  // namespace agentd
