#include "cors.h"

#include <cassert>
#include <string>

using agentd::CorsConfig;
using agentd::CorsRoute;
using agentd::cors_apply;
using agentd::cors_compile;
using agentd::cors_wire_headers;

using HttpRequest = agentd::HttpRequest;
using HttpResponse = agentd::HttpResponse;

static std::string header_get(const HttpResponse& resp, const char* key) {
  auto it = resp.headers.find(key);
  if (it == resp.headers.end()) return "";
  return it->second;
}

static HttpRequest make_req_with_origin(const std::string& origin) {
  HttpRequest req;
  req.method = "GET";
  req.path = "/api/v1/health";
  if (!origin.empty()) {
    req.headers["origin"] = origin; // daemon lowercases keys at parse-time; match that.
  }
  return req;
}

static HttpRequest make_req_with_origin_and_path(const std::string& origin, const std::string& path) {
  HttpRequest req;
  req.method = "GET";
  req.path = path;
  if (!origin.empty()) {
    req.headers["origin"] = origin;
  }
  return req;
}

int main() {
  {
    // Disabled CORS => no headers.
    CorsConfig cfg;
    cfg.origins.clear();
    cfg.allow_headers = "Content-Type";
    cfg.allow_methods = "GET, POST, OPTIONS";
    cors_compile(&cfg);

    HttpResponse resp;
    cors_apply(make_req_with_origin("http://localhost:5173"), &resp, cfg);
    assert(resp.headers.empty());
    assert(cors_wire_headers(make_req_with_origin("http://localhost:5173"), cfg).empty());
  }

  {
    // Allow any => always returns "*", no Vary.
    CorsConfig cfg;
    cfg.origins = {"*"};
    cfg.allow_headers = "Content-Type, Authorization, X-OpenRouter-Key";
    cfg.allow_methods = "GET, POST, DELETE, OPTIONS";
    cors_compile(&cfg);

    HttpResponse resp;
    cors_apply(make_req_with_origin("http://evil.example"), &resp, cfg);
    assert(header_get(resp, "Access-Control-Allow-Origin") == "*");
    assert(header_get(resp, "Vary").empty());

    const std::string wire = cors_wire_headers(make_req_with_origin("http://evil.example"), cfg);
    assert(wire.find("Access-Control-Allow-Origin: *\r\n") != std::string::npos);
    assert(wire.find("Vary:") == std::string::npos);
    assert(wire.find("X-OpenRouter-Key") != std::string::npos);
  }

  {
    // Exact allowlist => reflect only allowed origins and set Vary: Origin.
    CorsConfig cfg;
    cfg.origins = {"http://localhost:5173"};
    cfg.allow_headers = "Content-Type";
    cfg.allow_methods = "GET, POST, OPTIONS";
    cors_compile(&cfg);

    HttpResponse ok;
    cors_apply(make_req_with_origin("http://localhost:5173"), &ok, cfg);
    assert(header_get(ok, "Access-Control-Allow-Origin") == "http://localhost:5173");
    assert(header_get(ok, "Vary") == "Origin");

    HttpResponse bad;
    cors_apply(make_req_with_origin("http://evil.example"), &bad, cfg);
    assert(header_get(bad, "Access-Control-Allow-Origin").empty());
    assert(bad.headers.empty());

    const std::string wire_ok = cors_wire_headers(make_req_with_origin("http://localhost:5173"), cfg);
    assert(wire_ok.find("Access-Control-Allow-Origin: http://localhost:5173\r\n") != std::string::npos);
    assert(wire_ok.find("Vary: Origin\r\n") != std::string::npos);

    const std::string wire_bad = cors_wire_headers(make_req_with_origin("http://evil.example"), cfg);
    assert(wire_bad.empty());
  }

  {
    // Regex allowlist + credentials -> reflect origin and include credentials header.
    CorsConfig cfg;
    cfg.origins = {"re:^https://.*\\.example$"};
    cfg.allow_credentials = true;
    cors_compile(&cfg);

    HttpResponse resp;
    cors_apply(make_req_with_origin("https://ui.example"), &resp, cfg);
    assert(header_get(resp, "Access-Control-Allow-Origin") == "https://ui.example");
    assert(header_get(resp, "Access-Control-Allow-Credentials") == "true");
    assert(header_get(resp, "Vary") == "Origin");
  }

  {
    // Wildcard + credentials -> reflect origin (not "*").
    CorsConfig cfg;
    cfg.origins = {"*"};
    cfg.allow_credentials = true;
    cors_compile(&cfg);

    HttpResponse resp;
    cors_apply(make_req_with_origin("https://app.example"), &resp, cfg);
    assert(header_get(resp, "Access-Control-Allow-Origin") == "https://app.example");
    assert(header_get(resp, "Access-Control-Allow-Credentials") == "true");
    assert(header_get(resp, "Vary") == "Origin");
  }

  {
    // Route precedence: longest path prefix wins.
    CorsConfig cfg;
    cfg.origins = {"https://global.example"};
    CorsRoute r1;
    r1.path_prefix = "/api/v1";
    r1.origins = {"https://route.example"};
    CorsRoute r2;
    r2.path_prefix = "/api/v1/admin";
    r2.origins = {"https://admin.example"};
    cfg.routes = {r1, r2};
    cors_compile(&cfg);

    HttpResponse resp;
    cors_apply(make_req_with_origin_and_path("https://admin.example", "/api/v1/admin/foo"), &resp, cfg);
    assert(header_get(resp, "Access-Control-Allow-Origin") == "https://admin.example");

    HttpResponse resp2;
    cors_apply(make_req_with_origin_and_path("https://route.example", "/api/v1/health"), &resp2, cfg);
    assert(header_get(resp2, "Access-Control-Allow-Origin") == "https://route.example");
  }

  return 0;
}
