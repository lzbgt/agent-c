#include "run_request_parse.h"

#include "daemon_config.h"
#include "json_util.h"
#include "sandbox_policy.h"
#include "string_util.h"
#include "trace_id_util.h"

namespace agentd {
namespace {

Json::Value make_error(int rpc_status, const std::string& msg) {
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["rpc_status"] = rpc_status;
  o["error"] = msg;
  return o;
}

}  // namespace

bool parse_run_request_base(
  const DaemonConfig& daemon_cfg,
  const std::string& request_body,
  RunRequestParseResult* out,
  Json::Value* error_out
) {
  if (!out || !error_out) return false;
  Json::Value args;
  std::string perr;
  if (!json_parse_object(request_body, &args, &perr)) {
    *error_out = make_error(400, std::string("invalid JSON: ") + perr);
    return false;
  }

  const std::string prompt = args.isMember("prompt") && args["prompt"].isString() ? args["prompt"].asString() : "";
  if (prompt.empty()) {
    *error_out = make_error(400, "missing prompt");
    return false;
  }

  std::string trace_id;
  if (args.isMember("trace_id") && args["trace_id"].isString()) trace_id = trim_copy(args["trace_id"].asString());
  if (!trace_id.empty() && !trace_id_is_safe(trace_id)) {
    *error_out = make_error(400, "invalid trace_id");
    return false;
  }
  if (trace_id.empty()) trace_id = make_uuidish_trace_id();

  const bool requested_tools_set = args.isMember("tools") && args["tools"].isString();
  const std::string requested_tools = requested_tools_set ? args["tools"].asString() : daemon_cfg.tools;
  std::string daemon_tools;
  if (!normalize_tools_mode(daemon_cfg.tools, &daemon_tools)) {
    *error_out = make_error(500, "invalid daemon tools configuration");
    return false;
  }
  std::string tools = daemon_tools;
  if (requested_tools_set) {
    std::string requested_norm;
    if (!normalize_tools_mode(requested_tools, &requested_norm)) {
      *error_out = make_error(400, "invalid tools (expected: none|basic|host)");
      return false;
    }
    if (!tools_mode_allows(daemon_tools, requested_norm)) {
      *error_out = make_error(
        400,
        std::string("tools request exceeds daemon tools policy (daemon=") + daemon_tools +
          ", requested=" + requested_norm + ")"
      );
      return false;
    }
    tools = requested_norm;
  }

  out->args = std::move(args);
  out->prompt = prompt;
  out->trace_id = trace_id;
  out->tools = tools;
  return true;
}

}  // namespace agentd
