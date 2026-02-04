#include "memory_endpoints.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "memory_consolidator.h"

#include <json/json.h>

namespace agentd {

void handle_memory_consolidate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args(Json::objectValue);
  if (!req.body.empty()) {
    std::string perr;
    if (!json_parse_object(req.body, &args, &perr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = std::string("invalid JSON: ") + perr;
      resp->body = json_stringify(o);
      return;
    }
  }

  MemoryConsolidateOptions opt;
  opt.daily_days = cfg.memory_consolidate_daily_days;
  opt.keep_checkpoints = cfg.memory_consolidate_keep_checkpoints;

  if (args.isMember("daily_days") && args["daily_days"].isInt()) {
    opt.daily_days = std::max(0, args["daily_days"].asInt());
  }
  if (args.isMember("keep_checkpoints") && args["keep_checkpoints"].isInt()) {
    opt.keep_checkpoints = std::max(1, args["keep_checkpoints"].asInt());
  }
  if (args.isMember("max_entries") && args["max_entries"].isInt()) {
    opt.max_entries = std::max(1, args["max_entries"].asInt());
  }
  if (args.isMember("dry_run") && args["dry_run"].isBool()) {
    opt.dry_run = args["dry_run"].asBool();
  }

  Json::Value report;
  std::string err;
  if (!memory_consolidate_once(cfg, opt, &report, &err)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = err.empty() ? "memory consolidation failed" : err;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["data"] = report;
  resp->body = json_stringify(o);
  return;
}

}  // namespace agentd

