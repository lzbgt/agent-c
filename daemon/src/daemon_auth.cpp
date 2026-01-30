#include "daemon_auth.h"

#include "http_util.h"

namespace agentd {

bool daemon_auth_ok(const DaemonConfig& cfg, const HttpRequest& req) {
  if (cfg.auth_token.empty()) {
    return true;
  }
  const std::string auth = header_get_ci(req.headers, "authorization");
  const std::string got = bearer_token_from_auth_header(auth);
  return !got.empty() && got == cfg.auth_token;
}

bool daemon_require_auth(const DaemonConfig& cfg, const HttpRequest& req, HttpResponse* resp) {
  if (daemon_auth_ok(cfg, req)) {
    return true;
  }
  if (resp) {
    resp->status = 401;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->headers["WWW-Authenticate"] = "Bearer";
    resp->body = R"({"ok":false,"error":"unauthorized"})";
  }
  return false;
}

}  // namespace agentd

