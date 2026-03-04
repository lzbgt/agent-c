#include "sandbox_endpoints.h"

#include "cors.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "mount_allowlist.h"

#include <json/json.h>

namespace agentd {

void handle_sandbox_mount_validate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    resp->body = json_error_body(perr.empty() ? "invalid json" : perr);
    return;
  }

  if (!args.isMember("host_path") || !args["host_path"].isString()) {
    resp->status = 400;
    resp->body = json_error_body("missing host_path");
    return;
  }
  if (!args.isMember("container_path") || !args["container_path"].isString()) {
    resp->status = 400;
    resp->body = json_error_body("missing container_path");
    return;
  }

  MountAllowlistInput input;
  input.host_path = args["host_path"].asString();
  input.container_path = args["container_path"].asString();
  input.container_prefix = "/workspace/extra";
  input.is_main = true;
  if (args.isMember("container_prefix") && args["container_prefix"].isString()) {
    input.container_prefix = args["container_prefix"].asString();
  }
  if (args.isMember("is_main") && args["is_main"].isBool()) {
    input.is_main = args["is_main"].asBool();
  }

  const MountAllowlist* allow = mount_allowlist_or_null();
  const MountAllowlistDecision decision = mount_allowlist_validate(allow, input);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["allowed"] = decision.allowed;
  out["readonly"] = decision.readonly;
  out["reason"] = decision.reason;
  if (!decision.resolved_host_path.empty()) {
    out["resolved_host_path"] = decision.resolved_host_path;
  }
  if (!decision.resolved_container_path.empty()) {
    out["resolved_container_path"] = decision.resolved_container_path;
  }
  if (!decision.matched_root.empty()) {
    out["matched_root"] = decision.matched_root;
  }
  if (!decision.blocked_pattern.empty()) {
    out["blocked_pattern"] = decision.blocked_pattern;
  }
  resp->body = json_stringify(out);
}

}  // namespace agentd
