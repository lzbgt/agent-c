#include "workflow_endpoints.h"

#include "daemon_auth.h"
#include "drain_state.h"
#include "http_util.h"
#include "json_util.h"
#include "workflow_submit.h"

#include <json/json.h>

#include <string>
#include <utility>

namespace agentd {
namespace {

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

}  // namespace

void handle_workflow_submit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (drain_is_active()) {
    resp->status = 503;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "draining";
    const int64_t until_ms = drain_until_unix_ms();
    if (until_ms > 0) o["drain_until_unix_ms"] = (Json::Int64)until_ms;
    const std::string reason = drain_reason();
    if (!reason.empty()) o["drain_reason"] = reason;
    resp->body = json_stringify_compact(o);
    return;
  }

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  workflow_submit_handle(cfg, db_or_null, req, std::move(args), resp);
}

}  // namespace agentd
