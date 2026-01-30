#include "cors.h"

#include <optional>
#include <sstream>

namespace agentd {

static std::string header_get_lc(const std::map<std::string, std::string>& headers, const char* key_lc) {
  auto it = headers.find(key_lc);
  if (it == headers.end()) return "";
  return it->second;
}

static bool cors_enabled(const CorsConfig& cfg) {
  return !cfg.origins.empty();
}

static bool cors_allow_any_origin(const CorsConfig& cfg) {
  for (const std::string& o : cfg.origins) {
    if (o == "*") return true;
  }
  return false;
}

static std::optional<std::string> cors_allowed_origin(const HttpRequest& req, const CorsConfig& cfg) {
  if (!cors_enabled(cfg)) return std::nullopt;
  if (cors_allow_any_origin(cfg)) return std::string("*");

  const std::string origin = header_get_lc(req.headers, "origin");
  if (origin.empty()) return std::nullopt;
  for (const std::string& o : cfg.origins) {
    if (o == origin) return origin;
  }
  return std::nullopt;
}

static void vary_append(HttpResponse* resp, const std::string& token) {
  if (!resp) return;
  auto it = resp->headers.find("Vary");
  if (it == resp->headers.end() || it->second.empty()) {
    resp->headers["Vary"] = token;
    return;
  }
  const std::string cur = it->second;
  if (cur.find(token) != std::string::npos) {
    return;
  }
  resp->headers["Vary"] = cur + ", " + token;
}

void cors_apply(const HttpRequest& req, HttpResponse* resp, const CorsConfig& cfg) {
  if (!resp) return;
  const auto origin = cors_allowed_origin(req, cfg);
  if (!origin) return;

  resp->headers["Access-Control-Allow-Origin"] = *origin;
  if (!cfg.allow_methods.empty()) {
    resp->headers["Access-Control-Allow-Methods"] = cfg.allow_methods;
  }
  if (!cfg.allow_headers.empty()) {
    resp->headers["Access-Control-Allow-Headers"] = cfg.allow_headers;
  }
  if (cfg.max_age_seconds > 0) {
    resp->headers["Access-Control-Max-Age"] = std::to_string(cfg.max_age_seconds);
  }

  if (*origin != "*") {
    vary_append(resp, "Origin");
  }
}

std::string cors_wire_headers(const HttpRequest& req, const CorsConfig& cfg) {
  const auto origin = cors_allowed_origin(req, cfg);
  if (!origin) return "";

  std::ostringstream oss;
  oss << "Access-Control-Allow-Origin: " << *origin << "\r\n";
  if (!cfg.allow_methods.empty()) {
    oss << "Access-Control-Allow-Methods: " << cfg.allow_methods << "\r\n";
  }
  if (!cfg.allow_headers.empty()) {
    oss << "Access-Control-Allow-Headers: " << cfg.allow_headers << "\r\n";
  }
  if (cfg.max_age_seconds > 0) {
    oss << "Access-Control-Max-Age: " << cfg.max_age_seconds << "\r\n";
  }
  if (*origin != "*") {
    oss << "Vary: Origin\r\n";
  }
  return oss.str();
}

}  // namespace agentd

