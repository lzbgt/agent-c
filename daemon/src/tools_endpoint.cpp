#include "tools_endpoint.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "sandbox_policy.h"
#include "session_id_util.h"
#include "session_paths.h"

#include "agent/agent.h"
#include "agent/tools.h"

#include "toolset_basic.h"
#include "toolset_host.h"

#include <json/json.h>

#include <filesystem>

namespace agentd {

void handle_tools_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const ToolExtension* tool_ext_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (query_get(req.query, "tools_root").has_value()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"tools_root was removed; omit it and use explicit paths or session_id\"}";
    return;
  }

  std::string tools = cfg.tools;
  if (const auto q = query_get(req.query, "tools"); q && !q->empty()) {
    tools = *q;
  }

  const auto q_yolo = query_get(req.query, "yolo");
  const bool requested_yolo_set = q_yolo.has_value();
  const bool requested_yolo = requested_yolo_set ? string_to_bool(*q_yolo) : cfg.yolo_default;
  const bool yolo = sandbox_tighten_yolo(cfg.yolo_default, requested_yolo, requested_yolo_set);

  HostToolsetPolicyMode requested_policy = cfg.host_policy;
  if (const auto q = query_get(req.query, "host_policy"); q && !q->empty()) {
    HostToolsetPolicyMode p{};
    if (!host_policy_from_string(*q, &p)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid host_policy (expected: full|readonly)\"}";
      return;
    }
    requested_policy = p;
  }
  const HostToolsetPolicyMode effective_policy = tighten_host_policy(cfg.host_policy, requested_policy);
  std::string session_id;
  if (const auto q = query_get(req.query, "session_id"); q && !q->empty()) {
    session_id = *q;
    if (!session_id_is_safe(session_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid session_id\"}";
      return;
    }
  }
  if (!session_id.empty() && !sessions_root_dir.empty()) {
    const std::filesystem::path sr = session_root_path(sessions_root_dir, session_id);
    if (sr.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid session_id\"}";
      return;
    }
    std::error_code ec;
    (void)std::filesystem::create_directories(sr / "work", ec);
    ec.clear();
    (void)std::filesystem::create_directories(sr / "out", ec);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["tools"] = tools;
  out["effective_yolo"] = yolo;
  out["effective_host_policy"] = host_policy_to_string(effective_policy);
  if (!session_id.empty()) {
    out["effective_session_id"] = session_id;
  }

  agent_tool_registry_t* registry = nullptr;
  agent_tool_executor_t executor{};
  bool need_destroy_executor = false;

  if (tools == "none") {
    out["count"] = 0;
    out["defs"] = Json::Value(Json::arrayValue);
    resp->body = json_stringify(out);
    return;
  }
  if (tools == "basic") {
    if (toolset_basic_create(&registry, &executor) != AGENT_OK) {
      resp->status = 500;
      resp->body = R"({"ok":false,"error":"failed to init toolset_basic"})";
      return;
    }
  } else if (tools == "host") {
    HostToolsetConfig hcfg;
    hcfg.policy = effective_policy;
    hcfg.enable_process_exec = yolo;
    hcfg.allow_symlinks = yolo;
    if (!session_id.empty()) {
      hcfg.sessions_root_dir = sessions_root_dir;
      hcfg.session_id = session_id;
    }
    if (toolset_host_create(hcfg, &registry, &executor) != AGENT_OK) {
      resp->status = 500;
      resp->body = R"({"ok":false,"error":"failed to init toolset_host"})";
      return;
    }
    need_destroy_executor = true;
  } else {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid tools (expected: none|basic|host)\"}";
    return;
  }

  if (tool_ext_or_null && tool_ext_or_null->register_tools) {
    const agent_status_t st = tool_ext_or_null->register_tools(tool_ext_or_null->ctx, registry);
    if (st != AGENT_OK) {
      resp->status = 500;
      resp->body = "{\"ok\":false,\"error\":\"tool extension register_tools failed\"}";
      agent_tool_registry_destroy(registry);
      if (need_destroy_executor) {
        toolset_host_destroy(&executor);
      }
      return;
    }
  }

  Json::Value arr(Json::arrayValue);
  const size_t n = agent_tool_registry_count(registry);
  for (size_t i = 0; i < n; i++) {
    agent_tool_def_view_t v{};
    if (agent_tool_registry_get(registry, i, &v) != AGENT_OK) continue;
    Json::Value d(Json::objectValue);
    d["name"] = v.name ? v.name : "";
    d["description"] = v.description ? v.description : "";
    d["parameters_json"] = v.parameters_json ? v.parameters_json : "";
    arr.append(d);
  }
  out["count"] = (Json::UInt64)arr.size();
  out["defs"] = arr;

  agent_tool_registry_destroy(registry);
  if (need_destroy_executor) {
    toolset_host_destroy(&executor);
  }

  resp->body = json_stringify(out);
  return;
}

}  // namespace agentd
