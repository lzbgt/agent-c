#include "cors.h"

#include <cassert>
#include <string>

using agentd::CorsConfig;
using agentd::cors_apply;
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

int main() {
  {
    // Disabled CORS => no headers.
    CorsConfig cfg;
    cfg.origins.clear();
    cfg.allow_headers = "Content-Type";
    cfg.allow_methods = "GET, POST, OPTIONS";

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

  return 0;
}
