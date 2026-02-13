#include "cors.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>

namespace agentd {

static std::string header_get_lc(const std::map<std::string, std::string>& headers, const char* key_lc) {
  auto it = headers.find(key_lc);
  if (it == headers.end()) return "";
  return it->second;
}

static bool cors_enabled(const CorsConfig& cfg) {
  return !cfg.origins.empty() || !cfg.routes.empty();
}

static std::string lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

static std::string trim_ws(std::string s) {
  size_t start = 0;
  size_t end = s.size();
  while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) start++;
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) end--;
  if (start == 0 && end == s.size()) return s;
  return s.substr(start, end - start);
}

static CorsOriginMatcher build_origin_matcher(const std::vector<std::string>& patterns) {
  CorsOriginMatcher out;
  for (const std::string& raw : patterns) {
    std::string p = trim_ws(raw);
    if (p.empty()) continue;
    if (p == "*") {
      out.any = true;
      continue;
    }
    if (p.rfind("re:", 0) == 0) {
      std::string src = trim_ws(p.substr(3));
      if (src.empty()) continue;
      try {
        out.regex.emplace_back(src);
      } catch (...) {
        // Ignore invalid regex.
      }
      continue;
    }
    out.exact[lower_copy(p)] = true;
  }
  return out;
}

static bool matcher_matches(const CorsOriginMatcher& m, const std::string& origin, bool* out_exact, bool* out_regex) {
  if (out_exact) *out_exact = false;
  if (out_regex) *out_regex = false;
  const std::string trimmed = trim_ws(origin);
  if (trimmed.empty()) return false;
  if (!m.exact.empty()) {
    if (m.exact.find(lower_copy(trimmed)) != m.exact.end()) {
      if (out_exact) *out_exact = true;
      return true;
    }
  }
  for (const auto& re : m.regex) {
    if (std::regex_search(trimmed, re)) {
      if (out_regex) *out_regex = true;
      return true;
    }
  }
  if (m.any) {
    return true;
  }
  return false;
}

void cors_compile(CorsConfig* cfg) {
  if (!cfg) return;
  cfg->matcher = build_origin_matcher(cfg->origins);
  for (auto& r : cfg->routes) {
    r.matcher = build_origin_matcher(r.origins);
  }
}

static const CorsRoute* select_route(const std::string& path, const std::vector<CorsRoute>& routes) {
  const CorsRoute* best = nullptr;
  size_t best_len = 0;
  for (const auto& r : routes) {
    const std::string pfx = r.path_prefix;
    if (pfx.empty() || path.rfind(pfx, 0) == 0) {
      if (!best || pfx.size() > best_len) {
        best = &r;
        best_len = pfx.size();
      }
    }
  }
  return best;
}

static std::optional<std::string> cors_allowed_origin(const HttpRequest& req, const CorsConfig& cfg) {
  if (!cors_enabled(cfg)) return std::nullopt;
  const CorsRoute* route = select_route(req.path, cfg.routes);
  const std::vector<std::string>* patterns = &cfg.origins;
  const CorsOriginMatcher* matcher = &cfg.matcher;
  if (route) {
    patterns = &route->origins;
    matcher = &route->matcher;
  }
  const std::string origin = header_get_lc(req.headers, "origin");
  if (origin.empty()) return std::nullopt;
  if (patterns == nullptr || patterns->empty()) return std::nullopt;

  bool exact = false;
  bool regex = false;
  if (!matcher_matches(*matcher, origin, &exact, &regex)) {
    return std::nullopt;
  }

  if (exact || regex) {
    return origin;
  }
  if (cfg.allow_credentials) {
    return origin;
  }
  return std::string("*");
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
  if (cfg.allow_credentials) {
    resp->headers["Access-Control-Allow-Credentials"] = "true";
  }
  resp->headers["Access-Control-Expose-Headers"] = "X-Request-Id, X-Trace-Id";
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
  if (cfg.allow_credentials) {
    oss << "Access-Control-Allow-Credentials: true\r\n";
  }
  oss << "Access-Control-Expose-Headers: X-Request-Id, X-Trace-Id\r\n";
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
