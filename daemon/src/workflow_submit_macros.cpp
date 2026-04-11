#include "workflow_submit_macros.h"

#include "daemon_config.h"
#include "edge_util.h"
#include "http_allowlist.h"
#include "http_client.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string join_base_path(std::string base, const std::string& path) {
  if (base.empty()) return path;
  while (!base.empty() && base.back() == '/') base.pop_back();
  if (path.empty()) return base;
  if (path[0] == '/') return base + path;
  return base + "/" + path;
}

static bool memory_scope_id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 160) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string normalize_memory_scope_mode(const std::string& raw) {
  const std::string v = lower_copy(trim_copy(raw));
  if (v.empty()) return "";
  if (v == "read_only" || v == "readonly" || v == "ro") return "read_only";
  if (v == "read_write" || v == "readwrite" || v == "read-write" || v == "rw") return "read_write";
  return "";
}

static bool set_header_checked(
  Json::Value* headers,
  const std::string& key,
  const std::string& value,
  std::string* out_error
) {
  if (!headers || key.empty()) return false;
  if (!headers->isObject()) *headers = Json::Value(Json::objectValue);
  const std::string want = lower_copy(key);
  for (const auto& k : headers->getMemberNames()) {
    if (lower_copy(k) != want) continue;
    if (!(*headers)[k].isString() || trim_copy((*headers)[k].asString()) != value) {
      if (out_error) *out_error = "conflicting " + key + " header";
      return false;
    }
    return true;
  }
  (*headers)[key] = value;
  return true;
}

static void set_macro_error(
  HttpResponse* resp,
  int status,
  const std::string& task_id,
  const std::string& error,
  const Json::Value* details = nullptr
) {
  if (!resp) return;
  resp->status = status;
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["error"] = error;
  if (!task_id.empty()) o["task_id"] = task_id;
  if (details) o["details"] = *details;
  resp->body = json_stringify_compact(o);
}

static bool url_has_http_scheme(const std::string& url) {
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

static bool env_name_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  const auto is_alpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
  const auto is_num = [](char c) { return (c >= '0' && c <= '9'); };
  if (!(is_alpha(s[0]) || s[0] == '_')) return false;
  for (char c : s) {
    if (!(is_alpha(c) || is_num(c) || c == '_')) return false;
  }
  return true;
}

static bool id_prefix_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static bool append_agentd_parallel_broker_registry_target(
  Json::Value* targets,
  const std::string& task_id,
  const std::string& broker_base_url,
  const std::string& id_prefix,
  const std::string& agent_id,
  const std::string& deployment_id,
  int64_t max_targets,
  HttpResponse* resp
) {
  if (!targets || !targets->isArray()) return false;
  if ((int64_t)targets->size() >= max_targets) return true;
  if (!id_is_safe(agent_id)) {
    Json::Value d(Json::objectValue);
    d["agent_id"] = agent_id;
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry agent_id must be id-safe", &d);
    return false;
  }
  if (!deployment_id.empty() && !id_is_safe(deployment_id)) {
    Json::Value d(Json::objectValue);
    d["agent_id"] = agent_id;
    d["deployment_id"] = deployment_id;
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry deployment_id must be id-safe", &d);
    return false;
  }

  std::string target_id = id_prefix + agent_id;
  if (!deployment_id.empty()) target_id += ":" + deployment_id;
  if (!id_is_safe(target_id)) {
    Json::Value d(Json::objectValue);
    d["target_id"] = target_id;
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry produced invalid target id", &d);
    return false;
  }

  Json::Value bp(Json::objectValue);
  bp["broker_base_url"] = broker_base_url;
  bp["agent_id"] = agent_id;
  if (!deployment_id.empty()) bp["deployment_id"] = deployment_id;

  Json::Value tgt(Json::objectValue);
  tgt["id"] = target_id;
  tgt["broker_proxy"] = bp;
  targets->append(tgt);
  return true;
}

static bool append_agentd_parallel_targets_from_broker_registry(
  const DaemonConfig& cfg,
  const Json::Value& spec,
  const std::string& task_id,
  Json::Value* targets,
  HttpResponse* resp
) {
  if (!spec.isObject()) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry must be an object");
    return false;
  }
  if (!cfg.workflow_enable_http_tasks) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry requires --workflow-enable-http-tasks");
    return false;
  }
  if (!targets || !targets->isArray()) {
    set_macro_error(resp, 400, task_id, "invalid agentd_parallel targets accumulator");
    return false;
  }

  const std::string broker_base_url =
    spec.isMember("broker_base_url") && spec["broker_base_url"].isString()
    ? trim_copy(spec["broker_base_url"].asString())
    : "";
  if (broker_base_url.empty()) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.broker_base_url is required");
    return false;
  }
  if (!url_has_http_scheme(broker_base_url)) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.broker_base_url must start with http:// or https://");
    return false;
  }
  if (broker_base_url.size() > 4096) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.broker_base_url is too long");
    return false;
  }

  std::string id_prefix =
    spec.isMember("id_prefix") && spec["id_prefix"].isString() ? trim_copy(spec["id_prefix"].asString()) : "";
  if (!id_prefix.empty() && !id_prefix_is_safe(id_prefix)) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.id_prefix must be id-safe");
    return false;
  }

  bool connected_only = true;
  if (spec.isMember("connected_only")) {
    if (!spec["connected_only"].isBool()) {
      set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.connected_only must be a bool");
      return false;
    }
    connected_only = spec["connected_only"].asBool();
  }
  bool enabled_only = true;
  if (spec.isMember("enabled_only")) {
    if (!spec["enabled_only"].isBool()) {
      set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.enabled_only must be a bool");
      return false;
    }
    enabled_only = spec["enabled_only"].asBool();
  }

  std::string deployment_policy =
    spec.isMember("deployment_policy") && spec["deployment_policy"].isString()
    ? lower_copy(trim_copy(spec["deployment_policy"].asString()))
    : "prefer_deployments";
  if (deployment_policy == "deployments") deployment_policy = "prefer_deployments";
  if (deployment_policy == "all") deployment_policy = "all_connected";
  if (deployment_policy == "none") deployment_policy = "agent";
  if (deployment_policy != "prefer_deployments" && deployment_policy != "all_connected" && deployment_policy != "agent") {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.deployment_policy must be prefer_deployments, all_connected, or agent");
    return false;
  }

  int64_t max_targets = 16;
  if (spec.isMember("max_targets")) {
    if (!(spec["max_targets"].isInt64() || spec["max_targets"].isUInt64() || spec["max_targets"].isInt() || spec["max_targets"].isUInt())) {
      set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.max_targets must be an integer");
      return false;
    }
    max_targets = spec["max_targets"].asInt64();
  }
  if (max_targets < 1 || max_targets > 32) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.max_targets must be between 1 and 32");
    return false;
  }

  int64_t timeout_ms = 10000;
  if (spec.isMember("timeout_ms")) {
    if (!(spec["timeout_ms"].isInt64() || spec["timeout_ms"].isUInt64() || spec["timeout_ms"].isInt() || spec["timeout_ms"].isUInt())) {
      set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.timeout_ms must be an integer");
      return false;
    }
    timeout_ms = spec["timeout_ms"].asInt64();
  }
  if (timeout_ms < 1) timeout_ms = 1;
  if (timeout_ms > 60000) timeout_ms = 60000;

  size_t max_bytes = 1024 * 1024;
  if (spec.isMember("max_bytes")) {
    if (!(spec["max_bytes"].isInt64() || spec["max_bytes"].isUInt64() || spec["max_bytes"].isInt() || spec["max_bytes"].isUInt())) {
      set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.max_bytes must be an integer");
      return false;
    }
    const int64_t v = spec["max_bytes"].asInt64();
    if (v > 0) max_bytes = (size_t)v;
  }
  if (max_bytes < 1024) max_bytes = 1024;
  if (max_bytes > 16 * 1024 * 1024) max_bytes = 16 * 1024 * 1024;

  std::unordered_set<std::string> include_agent_ids;
  if (spec.isMember("agent_ids")) {
    if (!spec["agent_ids"].isArray()) {
      set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.agent_ids must be an array");
      return false;
    }
    for (Json::ArrayIndex i = 0; i < spec["agent_ids"].size(); i++) {
      if (!spec["agent_ids"][i].isString()) {
        set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.agent_ids entries must be strings");
        return false;
      }
      const std::string agent_id = trim_copy(spec["agent_ids"][i].asString());
      if (!id_is_safe(agent_id)) {
        Json::Value d(Json::objectValue);
        d["agent_id"] = agent_id;
        set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.agent_ids entries must be id-safe", &d);
        return false;
      }
      include_agent_ids.insert(agent_id);
    }
  }

  std::map<std::string, std::string> headers;
  headers["Accept"] = "application/json";
  if (spec.isMember("bearer_env") && spec["bearer_env"].isString()) {
    const std::string env = trim_copy(spec["bearer_env"].asString());
    if (!env.empty()) {
      if (!env_name_is_safe(env)) {
        set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.bearer_env must be a safe env var name");
        return false;
      }
      const char* v = std::getenv(env.c_str());
      if (!v || !v[0]) {
        Json::Value d(Json::objectValue);
        d["bearer_env"] = env;
        set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.bearer_env env var is missing/empty", &d);
        return false;
      }
      headers["Authorization"] = std::string("Bearer ") + v;
    }
  } else if (spec.isMember("bearer_env") && !spec["bearer_env"].isNull()) {
    set_macro_error(resp, 400, task_id, "agentd_parallel.targets_from_broker_registry.bearer_env must be a string");
    return false;
  }

  const std::string agents_url = join_base_path(broker_base_url, "/v1/agents");
  WorkflowHttpUrlCheck check;
  {
    std::string why;
    if (!workflow_http_url_check(cfg, agents_url, &check, &why)) {
      Json::Value d(Json::objectValue);
      d["url"] = agents_url;
      d["reason"] = why;
      set_macro_error(resp, 400, task_id, "agentd_parallel broker registry URL is not allowed by workflow_http outbound policy", &d);
      return false;
    }
    if (cfg.workflow_http_dns_pin && !check.host_is_ip && check.resolved_addrs.empty()) {
      Json::Value d(Json::objectValue);
      d["url"] = agents_url;
      set_macro_error(resp, 400, task_id, "agentd_parallel broker registry DNS pin is enabled but DNS resolution produced no addresses", &d);
      return false;
    }
  }

  HttpClientPinnedResolve pin;
  const HttpClientPinnedResolve* pinp = nullptr;
  if (cfg.workflow_http_dns_pin && !check.host_is_ip) {
    pin.host = check.host;
    pin.port = check.port;
    for (const auto& ip : check.resolved_addrs) {
      if (!ip.empty() && ip.find(':') == std::string::npos) pin.addrs.push_back(ip);
    }
    if (pin.addrs.empty()) pin.addrs = check.resolved_addrs;
    pinp = &pin;
  }

  const HttpClientResult r = http_request(agents_url, "GET", headers, "", timeout_ms, max_bytes, cfg.proxy_url, pinp);
  if (!r.ok || r.http_status < 200 || r.http_status >= 300) {
    Json::Value d(Json::objectValue);
    d["url"] = agents_url;
    d["http_status"] = (Json::Int64)r.http_status;
    if (!r.error.empty()) d["http_error"] = r.error;
    if (!r.response_body.empty()) d["response_text"] = r.response_body.substr(0, 4096);
    set_macro_error(resp, 502, task_id, "agentd_parallel broker registry request failed", &d);
    return false;
  }

  Json::Value root;
  std::string perr;
  if (!json_parse_any(r.response_body, &root, &perr) || !root.isObject() || !root.isMember("agents") || !root["agents"].isArray()) {
    Json::Value d(Json::objectValue);
    d["parse_error"] = perr;
    d["response_text"] = r.response_body.substr(0, 4096);
    set_macro_error(resp, 502, task_id, "agentd_parallel broker registry response must be an object with agents array", &d);
    return false;
  }

  const int64_t before = (int64_t)targets->size();
  const Json::Value agents = root["agents"];
  for (Json::ArrayIndex i = 0; i < agents.size(); i++) {
    if ((int64_t)targets->size() - before >= max_targets) break;
    const Json::Value& agent = agents[i];
    if (!agent.isObject()) continue;
    const std::string agent_id =
      agent.isMember("agent_id") && agent["agent_id"].isString() ? trim_copy(agent["agent_id"].asString()) : "";
    if (agent_id.empty()) continue;
    if (!include_agent_ids.empty() && !include_agent_ids.count(agent_id)) continue;
    if (enabled_only && (!agent.isMember("enabled") || !agent["enabled"].isBool() || !agent["enabled"].asBool())) continue;
    if (connected_only && (!agent.isMember("connected") || !agent["connected"].isBool() || !agent["connected"].asBool())) continue;

    bool appended_deployment = false;
    if (deployment_policy != "agent" && agent.isMember("deployments") && agent["deployments"].isArray()) {
      const Json::Value deployments = agent["deployments"];
      for (Json::ArrayIndex di = 0; di < deployments.size(); di++) {
        if ((int64_t)targets->size() - before >= max_targets) break;
        const Json::Value& dep = deployments[di];
        if (!dep.isObject()) continue;
        const std::string deployment_id =
          dep.isMember("deployment_id") && dep["deployment_id"].isString() ? trim_copy(dep["deployment_id"].asString()) : "";
        if (deployment_id.empty()) continue;
        if (connected_only && dep.isMember("connected") && dep["connected"].isBool() && !dep["connected"].asBool()) continue;
        if (!append_agentd_parallel_broker_registry_target(
              targets, task_id, broker_base_url, id_prefix, agent_id, deployment_id, before + max_targets, resp)) {
          return false;
        }
        appended_deployment = true;
      }
    }
    if (deployment_policy == "agent" || (deployment_policy == "prefer_deployments" && !appended_deployment)) {
      if (!append_agentd_parallel_broker_registry_target(
            targets, task_id, broker_base_url, id_prefix, agent_id, "", before + max_targets, resp)) {
        return false;
      }
    }
  }

  if ((int64_t)targets->size() == before) {
    set_macro_error(resp, 409, task_id, "agentd_parallel broker registry produced no matching targets");
    return false;
  }
  return true;
}

static bool apply_agentd_parallel_memory_scope(
  Json::Value* call,
  const std::string& memory_scope_id,
  const std::string& memory_scope_mode,
  const std::string& target_id,
  const std::string& target_identity,
  std::string* out_error
) {
  if (!call || !call->isObject()) return false;
  Json::Value wf = call->isMember("workflow") && (*call)["workflow"].isObject()
    ? (*call)["workflow"]
    : Json::Value(Json::nullValue);
  if (!wf.isObject()) {
    if (out_error) *out_error = "agentd_parallel.agentd_call.workflow must be an object";
    return false;
  }

  Json::Value defaults = wf.isMember("defaults") && wf["defaults"].isObject()
    ? wf["defaults"]
    : Json::Value(Json::objectValue);
  const auto require_same_string = [&](const char* key, const std::string& value) -> bool {
    if (!defaults.isMember(key) || defaults[key].isNull()) return true;
    if (!defaults[key].isString() || trim_copy(defaults[key].asString()) != value) {
      if (out_error) *out_error = std::string("agentd_parallel.memory_scope conflicts with agentd_call.workflow.defaults.") + key;
      return false;
    }
    return true;
  };
  if (!require_same_string("memory_scope_id", memory_scope_id)) return false;
  if (!require_same_string("memory_scope_mode", memory_scope_mode)) return false;
  defaults["memory_scope_id"] = memory_scope_id;
  defaults["memory_scope_mode"] = memory_scope_mode;
  wf["defaults"] = defaults;

  Json::Value inputs = wf.isMember("inputs") && wf["inputs"].isObject()
    ? wf["inputs"]
    : Json::Value(Json::objectValue);
  const auto require_same_input_string = [&](const char* key, const std::string& value) -> bool {
    if (!inputs.isMember(key) || inputs[key].isNull()) return true;
    if (!inputs[key].isString() || trim_copy(inputs[key].asString()) != value) {
      if (out_error) *out_error = std::string("agentd_parallel.memory_scope conflicts with agentd_call.workflow.inputs.") + key;
      return false;
    }
    return true;
  };
  if (!require_same_input_string("agentd_parallel_target_id", target_id)) return false;
  if (!require_same_input_string("agentd_parallel_target_identity", target_identity)) return false;
  inputs["agentd_parallel_target_id"] = target_id;
  inputs["agentd_parallel_target_identity"] = target_identity;
  wf["inputs"] = inputs;

  (*call)["workflow"] = wf;
  return true;
}

}  // namespace

bool expand_workflow_submit_macros(
  const DaemonConfig& cfg,
  Json::Value* io_tasks_arr,
  const Json::Value& workflow_defaults,
  AgentDb* db_or_null,
  bool allow_sessions,
  bool allow_inline_api_keys,
  const std::string& session_id,
  const std::string& trace_id,
  HttpResponse* resp
) {
  if (!io_tasks_arr || !io_tasks_arr->isArray()) {
    if (resp) {
      resp->status = 400;
      resp->body = json_error_body("invalid tasks (expected array)");
    }
    return false;
  }

  Json::Value out(Json::arrayValue);
  for (Json::ArrayIndex i = 0; i < io_tasks_arr->size(); i++) {
    const Json::Value t = (*io_tasks_arr)[i];
    if (!t.isObject()) {
      out.append(t);
      continue;
    }

    const std::string kind =
      t.isMember("kind") && t["kind"].isString() ? trim_copy(t["kind"].asString()) : std::string();
    if (kind != "delegate_parallel" && kind != "edge_parallel" && kind != "agentd_parallel") {
      out.append(t);
      continue;
    }

    const std::string task_id =
      t.isMember("task_id") && t["task_id"].isString() ? trim_copy(t["task_id"].asString()) : ("task_" + std::to_string((int)i));
    if (!id_is_safe(task_id)) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid task_id";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    if (kind == "agentd_parallel") {
      const Json::Value ap = t.isMember("agentd_parallel") ? t["agentd_parallel"] : Json::Value(Json::nullValue);
      if (!ap.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel task missing agentd_parallel object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      Json::Value targets(Json::arrayValue);
      if (ap.isMember("targets") && !ap["targets"].isNull()) {
        if (!ap["targets"].isArray()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.agentd_parallel.targets must be an array";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        for (Json::ArrayIndex j = 0; j < ap["targets"].size(); j++) targets.append(ap["targets"][j]);
      }
      if (ap.isMember("targets_from_broker_registry") && !ap["targets_from_broker_registry"].isNull()) {
        if (!append_agentd_parallel_targets_from_broker_registry(
              cfg, ap["targets_from_broker_registry"], task_id, &targets, resp)) {
          return false;
        }
      }
      if (!targets.isArray() || targets.empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.agentd_parallel.targets must be a non-empty array or targets_from_broker_registry must discover targets";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      int64_t count = (int64_t)targets.size();
      if (count > 32) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.targets too large (max 32)";
          o["task_id"] = task_id;
          o["count"] = (Json::Int64)count;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      const Json::Value call =
        ap.isMember("agentd_call") ? ap["agentd_call"] : Json::Value(Json::nullValue);
      if (!call.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.agentd_parallel.agentd_call must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (call.isMember("base_url") && call["base_url"].isString() && !trim_copy(call["base_url"].asString()).empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.agentd_call.base_url must be omitted (targets provide base_url)";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      bool require_distinct_targets = false;
      const Json::Value routing_policy =
        ap.isMember("routing_policy") ? ap["routing_policy"] : Json::Value(Json::nullValue);
      if (!routing_policy.isNull()) {
        if (!routing_policy.isObject()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.routing_policy must be an object";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (routing_policy.isMember("require_distinct_targets")) {
          if (!routing_policy["require_distinct_targets"].isBool()) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = "agentd_parallel.routing_policy.require_distinct_targets must be a bool";
              o["task_id"] = task_id;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
          require_distinct_targets = routing_policy["require_distinct_targets"].asBool();
        }
      }

      bool has_memory_scope = false;
      bool memory_scope_per_target = true;
      std::string memory_scope_base_id;
      std::string memory_scope_mode = "read_write";
      const Json::Value memory_scope =
        ap.isMember("memory_scope") ? ap["memory_scope"] : Json::Value(Json::nullValue);
      if (!memory_scope.isNull()) {
        if (!memory_scope.isObject()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.memory_scope must be an object";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        memory_scope_base_id =
          memory_scope.isMember("scope_id") && memory_scope["scope_id"].isString()
          ? trim_copy(memory_scope["scope_id"].asString())
          : "";
        if (!memory_scope_id_is_safe(memory_scope_base_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.memory_scope.scope_id must be id-safe";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (memory_scope.isMember("mode")) {
          if (!memory_scope["mode"].isString()) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = "agentd_parallel.memory_scope.mode must be a string";
              o["task_id"] = task_id;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
          memory_scope_mode = normalize_memory_scope_mode(memory_scope["mode"].asString());
          if (memory_scope_mode.empty()) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = "agentd_parallel.memory_scope.mode must be read_only or read_write";
              o["task_id"] = task_id;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
        }
        if (memory_scope.isMember("per_target")) {
          if (!memory_scope["per_target"].isBool()) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = "agentd_parallel.memory_scope.per_target must be a bool";
              o["task_id"] = task_id;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
          memory_scope_per_target = memory_scope["per_target"].asBool();
        }
        has_memory_scope = true;
      }

      // Macro task fields to preserve/propagate.
      std::vector<std::string> dep_ids;
      if (t.isMember("depends_on") && t["depends_on"].isArray()) {
        for (Json::ArrayIndex j = 0; j < t["depends_on"].size(); j++) {
          if (!t["depends_on"][j].isString()) continue;
          const std::string dep = trim_copy(t["depends_on"][j].asString());
          if (!dep.empty()) dep_ids.push_back(dep);
        }
      }
      const int priority =
        t.isMember("priority") && t["priority"].isInt() ? std::max(-1000, std::min(1000, t["priority"].asInt())) : 0;

      Json::Value attempt_task_ids(Json::arrayValue);
      std::unordered_set<std::string> seen_target_ids;
      std::unordered_set<std::string> seen_target_identities;
      for (Json::ArrayIndex ti = 0; ti < targets.size(); ti++) {
        const Json::Value& tgt = targets[ti];

        std::string base_url;
        std::string target_id;
        std::string target_identity;
        std::string broker_agent_id;
        std::string broker_deployment_id;
        bool allow_error = true;
        Json::Value target_expect(Json::nullValue);
        Json::Value target_max_attempts(Json::nullValue);

        if (tgt.isString()) {
          base_url = trim_copy(tgt.asString());
          target_identity = "url:" + base_url;
          target_id = "t" + std::to_string((int)ti);
        } else if (tgt.isObject()) {
          if (tgt.isMember("base_url") && tgt["base_url"].isString()) base_url = trim_copy(tgt["base_url"].asString());
          if (tgt.isMember("id") && tgt["id"].isString()) target_id = trim_copy(tgt["id"].asString());
          // Convenience: broker-proxy addressing mode (compute base_url from {broker_base_url, agent_id}).
          if (base_url.empty() && tgt.isMember("broker_proxy") && !tgt["broker_proxy"].isNull()) {
            const Json::Value bp = tgt["broker_proxy"];
            if (!bp.isObject()) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy must be an object";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            const std::string broker_base_url =
              bp.isMember("broker_base_url") && bp["broker_base_url"].isString() ? trim_copy(bp["broker_base_url"].asString()) : "";
            const std::string agent_id =
              bp.isMember("agent_id") && bp["agent_id"].isString() ? trim_copy(bp["agent_id"].asString()) : "";
            const std::string deployment_id =
              bp.isMember("deployment_id") && bp["deployment_id"].isString() ? trim_copy(bp["deployment_id"].asString()) : "";
            if (broker_base_url.empty() || agent_id.empty()) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy must include broker_base_url and agent_id";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            if (!id_is_safe(agent_id)) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy.agent_id must be id-safe";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                o["agent_id"] = agent_id;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            if (!deployment_id.empty() && !id_is_safe(deployment_id)) {
              if (resp) {
                resp->status = 400;
                Json::Value o(Json::objectValue);
                o["ok"] = false;
                o["error"] = "agentd_parallel.targets[].broker_proxy.deployment_id must be id-safe";
                o["task_id"] = task_id;
                o["index"] = (Json::Int64)ti;
                o["deployment_id"] = deployment_id;
                resp->body = json_stringify_compact(o);
              }
              return false;
            }
            base_url = join_base_path(broker_base_url, "/v1/agents/" + agent_id + "/proxy");
            broker_agent_id = agent_id;
            broker_deployment_id = deployment_id;
            target_identity = "broker:" + broker_base_url + ":" + agent_id;
            if (!deployment_id.empty()) target_identity += ":" + deployment_id;
            if (target_id.empty()) target_id = agent_id;
          }
          if (target_id.empty()) target_id = "t" + std::to_string((int)ti);
          if (target_identity.empty()) target_identity = "url:" + base_url;
          if (tgt.isMember("allow_error") && tgt["allow_error"].isBool()) allow_error = tgt["allow_error"].asBool();
          if (tgt.isMember("expect") && tgt["expect"].isObject()) target_expect = tgt["expect"];
          if (tgt.isMember("max_attempts") && tgt["max_attempts"].isInt()) target_max_attempts = tgt["max_attempts"];
        } else {
          continue;
        }

        if (base_url.empty()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.targets entry missing base_url";
            o["task_id"] = task_id;
            o["index"] = (Json::Int64)ti;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (target_id.empty() || !id_is_safe(target_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.targets[].id must be id-safe";
            o["task_id"] = task_id;
            o["index"] = (Json::Int64)ti;
            o["id"] = target_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (target_identity.empty()) target_identity = "url:" + base_url;
        if (!seen_target_ids.insert(target_id).second) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "duplicate agentd_parallel target id";
            o["task_id"] = task_id;
            o["id"] = target_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        if (require_distinct_targets && !seen_target_identities.insert(target_identity).second) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "duplicate agentd_parallel routing target identity";
            o["task_id"] = task_id;
            o["id"] = target_id;
            o["target_identity"] = target_identity;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }

        const std::string attempt_task_id = task_id + ":" + target_id;
        if (!id_is_safe(attempt_task_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel produced invalid derived task_id";
            o["task_id"] = task_id;
            o["target_id"] = target_id;
            o["derived_task_id"] = attempt_task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }

        Json::Value at(Json::objectValue);
        at["task_id"] = attempt_task_id;
        at["kind"] = "agentd_call";
        at["allow_error"] = allow_error;
        if (priority != 0) at["priority"] = priority;
        if (t.isMember("inputs") && t["inputs"].isObject()) at["inputs"] = t["inputs"];
        if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
          at["ready_unix_ms"] = t["ready_unix_ms"];
        }
        if (!dep_ids.empty()) {
          Json::Value deps(Json::arrayValue);
          for (const auto& d : dep_ids) deps.append(d);
          at["depends_on"] = deps;
        }

        if (target_max_attempts.isInt()) {
          at["max_attempts"] = target_max_attempts;
        } else if (t.isMember("max_attempts") && t["max_attempts"].isInt()) {
          at["max_attempts"] = t["max_attempts"];
        }
        if (target_expect.isObject()) at["expect"] = target_expect;

        Json::Value call2 = call;
        call2["base_url"] = base_url;
        call2["target_id"] = target_id;
        call2["target_identity"] = target_identity;
        if (!broker_agent_id.empty()) call2["broker_agent_id"] = broker_agent_id;
        if (!broker_deployment_id.empty()) {
          call2["broker_deployment_id"] = broker_deployment_id;
          Json::Value headers = call2.isMember("headers") && call2["headers"].isObject()
            ? call2["headers"]
            : Json::Value(Json::objectValue);
          std::string herr;
          if (!set_header_checked(&headers, "X-Agentd-Deployment", broker_deployment_id, &herr)) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = herr.empty() ? "invalid broker deployment routing header" : herr;
              o["task_id"] = task_id;
              o["index"] = (Json::Int64)ti;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
          call2["headers"] = headers;
        }
        if (has_memory_scope) {
          std::string scope_id = memory_scope_base_id;
          if (memory_scope_per_target) scope_id += ":" + target_id;
          if (!memory_scope_id_is_safe(scope_id)) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = "agentd_parallel.memory_scope produced unsafe or too-long per-target scope_id";
              o["task_id"] = task_id;
              o["id"] = target_id;
              o["memory_scope_id"] = scope_id;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
          std::string merr;
          if (!apply_agentd_parallel_memory_scope(&call2, scope_id, memory_scope_mode, target_id, target_identity, &merr)) {
            if (resp) {
              resp->status = 400;
              Json::Value o(Json::objectValue);
              o["ok"] = false;
              o["error"] = merr.empty() ? "failed to apply agentd_parallel.memory_scope" : merr;
              o["task_id"] = task_id;
              o["id"] = target_id;
              resp->body = json_stringify_compact(o);
            }
            return false;
          }
        }
        at["agentd_call"] = call2;

        out.append(at);
        attempt_task_ids.append(attempt_task_id);
      }

      if (attempt_task_ids.empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel.targets must include at least one valid entry";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      // Replace macro task with aggregate join.
      Json::Value join(Json::objectValue);
      join["task_id"] = task_id;
      join["kind"] = "aggregate";
      if (priority != 0) join["priority"] = priority;
      if (t.isMember("allow_error") && t["allow_error"].isBool()) join["allow_error"] = t["allow_error"];
      if (t.isMember("inputs") && t["inputs"].isObject()) join["inputs"] = t["inputs"];
      if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
        join["ready_unix_ms"] = t["ready_unix_ms"];
      }
      {
        Json::Value deps(Json::arrayValue);
        for (Json::ArrayIndex k = 0; k < attempt_task_ids.size(); k++) deps.append(attempt_task_ids[k]);
        join["depends_on"] = deps;
      }

      Json::Value agg(Json::objectValue);
      if (ap.isMember("aggregate") && !ap["aggregate"].isNull()) {
        if (!ap["aggregate"].isObject()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "agentd_parallel.agentd_parallel.aggregate must be an object";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        agg = ap["aggregate"];
      }

      // agentd_parallel default join behavior: first_ok across targets.
      if (!agg.isMember("mode") || !agg["mode"].isString() || trim_copy(agg["mode"].asString()).empty()) {
        agg["mode"] = "first_ok";
      }
      agg["task_ids"] = attempt_task_ids;

      const std::string agg_mode =
        agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : std::string();
      if (agg_mode != "first_ok" && agg_mode != "quorum_ok" && agg_mode != "strict_all_ok" && agg_mode != "collect" && agg_mode != "best_of_n" && agg_mode != "quorum_hashes") {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "agentd_parallel aggregate.mode must be one of: first_ok, quorum_ok, strict_all_ok, collect, best_of_n, quorum_hashes";
          o["task_id"] = task_id;
          o["mode"] = agg_mode;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      // Ergonomic default for agentd_parallel quorum: node identity is the remote target identity.
      // Aggregate's default node_pointer (/edge/node_id) is correct for edge_invoke, but agentd_call results
      // identify the target under /agentd/target_identity (broker agent/deployment when available, otherwise URL).
      if (agg_mode == "quorum_hashes") {
        const bool has_ptrs =
          agg.isMember("pointers") && agg["pointers"].isArray() && !agg["pointers"].empty();
        if (!has_ptrs) {
          Json::Value ptrs(Json::arrayValue);
          ptrs.append("/agentd/result_sha256");
          agg["pointers"] = ptrs;
        }

        const bool has_node_pointer =
          agg.isMember("node_pointer") && agg["node_pointer"].isString() && !trim_copy(agg["node_pointer"].asString()).empty();
        if (!has_node_pointer) agg["node_pointer"] = "/agentd/target_identity";
      }

      join["aggregate"] = agg;

      if (t.isMember("max_attempts") && t["max_attempts"].isInt()) join["max_attempts"] = t["max_attempts"];
      if (t.isMember("expect") && t["expect"].isObject()) join["expect"] = t["expect"];

      out.append(join);
      continue;
    }

    if (kind == "edge_parallel") {
      const Json::Value ep = t.isMember("edge_parallel") ? t["edge_parallel"] : Json::Value(Json::nullValue);
      if (!ep.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel task missing edge_parallel object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (!db_or_null || !db_or_null->is_open()) {
        if (resp) {
          resp->status = 503;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "db not available (edge_parallel requires node registry)";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      int64_t count = 0;
      if (ep.isMember("count") && (ep["count"].isInt64() || ep["count"].isUInt64() || ep["count"].isInt() || ep["count"].isUInt())) {
        count = std::max<int64_t>(0, ep["count"].asInt64());
      }
      if (count <= 0) count = 2;
      if (count > 32) count = 32;

      Json::Value edge = ep.isMember("edge") && ep["edge"].isObject() ? ep["edge"] : Json::Value(Json::nullValue);
      if (!edge.isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel.edge must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      if (edge.isMember("node_id") && edge["node_id"].isString() && !trim_copy(edge["node_id"].asString()).empty()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel.edge.node_id must be omitted (use match_any fan-out)";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      if (!edge.isMember("match_any") || !edge["match_any"].isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel.edge.match_any must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      const Json::Value m = edge["match_any"];

      auto read_arr = [&](const char* k, std::vector<std::string>* outv) {
        if (!outv) return;
        outv->clear();
        if (!m.isMember(k) || !m[k].isArray()) return;
        for (Json::ArrayIndex j = 0; j < m[k].size(); j++) {
          if (!m[k][j].isString()) continue;
          const std::string s = trim_copy(m[k][j].asString());
          if (!s.empty()) outv->push_back(s);
        }
      };

      std::vector<std::string> requires_tools;
      std::vector<std::string> tags_all;
      std::vector<std::string> tags_any;
      std::vector<std::string> tags_none;
      read_arr("requires_tools", &requires_tools);
      read_arr("tags_all", &tags_all);
      read_arr("tags_any", &tags_any);
      read_arr("tags_none", &tags_none);

      std::unordered_set<std::string> exclude_node_ids;
      if (m.isMember("exclude_node_ids") && m["exclude_node_ids"].isArray()) {
        for (Json::ArrayIndex j = 0; j < m["exclude_node_ids"].size(); j++) {
          if (!m["exclude_node_ids"][j].isString()) continue;
          const std::string s = trim_copy(m["exclude_node_ids"][j].asString());
          if (!s.empty()) exclude_node_ids.insert(s);
        }
      }

      std::vector<AgentDb::EdgeNodeRow> nodes;
      std::string nerr;
      if (!db_or_null->list_edge_nodes(/*max_rows=*/256, &nodes, &nerr)) {
        if (resp) {
          resp->status = 500;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "failed to list edge nodes";
          o["detail"] = nerr;
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      std::vector<std::string> selected_node_ids;
      selected_node_ids.reserve((size_t)count);
      for (const auto& n : nodes) {
        if ((int64_t)selected_node_ids.size() >= count) break;
        if (n.node_id.empty()) continue;
        if (!edge_id_is_safe(n.node_id)) continue;
        if (exclude_node_ids.count(n.node_id)) continue;

        std::unordered_set<std::string> toolset;
        std::unordered_set<std::string> tagset;
        if (!edge_parse_string_set(n.tools_json, &toolset)) continue;
        if (!edge_parse_string_set(n.tags_json, &tagset)) continue;

        bool ok = true;
        for (const auto& tname : requires_tools) {
          if (tname.empty()) continue;
          if (!toolset.count(tname)) { ok = false; break; }
        }
        if (!ok) continue;
        for (const auto& tag : tags_all) {
          if (tag.empty()) continue;
          if (!tagset.count(tag)) { ok = false; break; }
        }
        if (!ok) continue;
        if (!tags_any.empty()) {
          bool any = false;
          for (const auto& tag : tags_any) {
            if (tag.empty()) continue;
            if (tagset.count(tag)) { any = true; break; }
          }
          if (!any) continue;
        }
        for (const auto& tag : tags_none) {
          if (tag.empty()) continue;
          if (tagset.count(tag)) { ok = false; break; }
        }
        if (!ok) continue;

        selected_node_ids.push_back(n.node_id);
        exclude_node_ids.insert(n.node_id);
      }

      if ((int64_t)selected_node_ids.size() < count) {
        if (resp) {
          resp->status = 409;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel: not enough matching nodes";
          o["task_id"] = task_id;
          o["requested"] = (Json::Int64)count;
          o["selected"] = (Json::Int64)selected_node_ids.size();
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      // Macro task fields to preserve/propagate.
      std::vector<std::string> dep_ids;
      if (t.isMember("depends_on") && t["depends_on"].isArray()) {
        for (Json::ArrayIndex j = 0; j < t["depends_on"].size(); j++) {
          if (!t["depends_on"][j].isString()) continue;
          const std::string dep = trim_copy(t["depends_on"][j].asString());
          if (!dep.empty()) dep_ids.push_back(dep);
        }
      }
      const int priority =
        t.isMember("priority") && t["priority"].isInt() ? std::max(-1000, std::min(1000, t["priority"].asInt())) : 0;

      Json::Value attempt_task_ids(Json::arrayValue);
      for (const auto& node_id : selected_node_ids) {
        const std::string attempt_task_id = task_id + ":" + node_id;
        if (!id_is_safe(attempt_task_id)) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "edge_parallel produced invalid derived task_id";
            o["task_id"] = task_id;
            o["node_id"] = node_id;
            o["derived_task_id"] = attempt_task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }

        Json::Value at(Json::objectValue);
        at["task_id"] = attempt_task_id;
        at["kind"] = "edge_invoke";
        at["allow_error"] = true;  // allow errors so the join can deterministically decide.
        if (priority != 0) at["priority"] = priority;
        if (t.isMember("inputs") && t["inputs"].isObject()) at["inputs"] = t["inputs"];
        if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
          at["ready_unix_ms"] = t["ready_unix_ms"];
        }
        if (!dep_ids.empty()) {
          Json::Value deps(Json::arrayValue);
          for (const auto& d : dep_ids) deps.append(d);
          at["depends_on"] = deps;
        }
        if (t.isMember("max_attempts") && t["max_attempts"].isInt()) at["max_attempts"] = t["max_attempts"];
        if (t.isMember("expect") && t["expect"].isObject()) at["expect"] = t["expect"];

        Json::Value e2 = edge;
        e2["node_id"] = node_id;
        e2.removeMember("match_any");  // node is fixed by macro expansion
        at["edge"] = e2;
        out.append(at);
        attempt_task_ids.append(attempt_task_id);
      }

      // Replace macro task with aggregate join.
      Json::Value join(Json::objectValue);
      join["task_id"] = task_id;
      join["kind"] = "aggregate";
      if (priority != 0) join["priority"] = priority;
      if (t.isMember("allow_error") && t["allow_error"].isBool()) join["allow_error"] = t["allow_error"];
      if (t.isMember("inputs") && t["inputs"].isObject()) join["inputs"] = t["inputs"];
      if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
        join["ready_unix_ms"] = t["ready_unix_ms"];
      }
      {
        Json::Value deps(Json::arrayValue);
        for (Json::ArrayIndex k = 0; k < attempt_task_ids.size(); k++) deps.append(attempt_task_ids[k]);
        join["depends_on"] = deps;
      }

      Json::Value agg(Json::objectValue);
      if (ep.isMember("aggregate") && !ep["aggregate"].isNull()) {
        if (!ep["aggregate"].isObject()) {
          if (resp) {
            resp->status = 400;
            Json::Value o(Json::objectValue);
            o["ok"] = false;
            o["error"] = "edge_parallel.edge_parallel.aggregate must be an object";
            o["task_id"] = task_id;
            resp->body = json_stringify_compact(o);
          }
          return false;
        }
        agg = ep["aggregate"];
      }

      // edge_parallel default join behavior: strict_all_ok across attempts.
      if (!agg.isMember("mode") || !agg["mode"].isString() || trim_copy(agg["mode"].asString()).empty()) {
        agg["mode"] = "strict_all_ok";
      }
      agg["task_ids"] = attempt_task_ids;

      const std::string agg_mode =
        agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : std::string();
      if (agg_mode != "first_ok" && agg_mode != "quorum_ok" && agg_mode != "strict_all_ok" && agg_mode != "collect" && agg_mode != "best_of_n" && agg_mode != "quorum_hashes") {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "edge_parallel aggregate.mode must be one of: first_ok, quorum_ok, strict_all_ok, collect, best_of_n, quorum_hashes";
          o["task_id"] = task_id;
          o["mode"] = agg_mode;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      // Ergonomic defaults for edge_parallel quorum:
      // - pointers defaults to the stable edge result hash surface
      // - node_pointer defaults to the selected node identity (for distinct-node quorum)
      if (agg_mode == "quorum_hashes") {
        const bool has_ptrs =
          agg.isMember("pointers") && agg["pointers"].isArray() && !agg["pointers"].empty();
        if (!has_ptrs) {
          Json::Value ptrs(Json::arrayValue);
          ptrs.append("/edge_result_sha256");
          agg["pointers"] = ptrs;
        }
        const bool has_node_pointer =
          agg.isMember("node_pointer") && agg["node_pointer"].isString() && !trim_copy(agg["node_pointer"].asString()).empty();
        if (!has_node_pointer) agg["node_pointer"] = "/edge/node_id";
      }
      join["aggregate"] = agg;

      if (t.isMember("max_attempts") && t["max_attempts"].isInt()) join["max_attempts"] = t["max_attempts"];
      if (t.isMember("expect") && t["expect"].isObject()) join["expect"] = t["expect"];

      out.append(join);
      continue;
    }

    if (!t.isMember("delegate") || !t["delegate"].isObject()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel task missing delegate object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    const Json::Value del = t["delegate"];
    const Json::Value attempt_defaults =
      del.isMember("attempt_defaults") ? del["attempt_defaults"] : Json::Value(Json::nullValue);
    if (del.isMember("attempt_defaults") && !attempt_defaults.isObject() && !attempt_defaults.isNull()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.delegate.attempt_defaults must be an object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }
    const Json::Value attempt_caps =
      del.isMember("attempt_caps") ? del["attempt_caps"] : Json::Value(Json::nullValue);
    if (del.isMember("attempt_caps") && !attempt_caps.isObject() && !attempt_caps.isNull()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.delegate.attempt_caps must be an object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }
    const Json::Value attempts = del.isMember("attempts") ? del["attempts"] : Json::Value(Json::nullValue);
    if (!attempts.isArray() || attempts.empty()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.delegate.attempts must be a non-empty array";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    std::vector<std::string> dep_ids;
    if (t.isMember("depends_on") && t["depends_on"].isArray()) {
      for (Json::ArrayIndex di = 0; di < t["depends_on"].size(); di++) {
        if (!t["depends_on"][di].isString()) continue;
        dep_ids.push_back(trim_copy(t["depends_on"][di].asString()));
      }
    }

    int priority = 0;
    if (t.isMember("priority") && t["priority"].isInt()) priority = t["priority"].asInt();

    Json::Value attempt_task_ids(Json::arrayValue);
    std::unordered_set<std::string> seen_attempt_ids;
    for (Json::ArrayIndex ai = 0; ai < attempts.size(); ai++) {
      const auto& a = attempts[ai];
      if (!a.isObject()) continue;
      const std::string attempt_id =
        a.isMember("id") && a["id"].isString() ? trim_copy(a["id"].asString()) : ("att_" + std::to_string((int)ai));
      if (attempt_id.empty() || !id_is_safe(attempt_id)) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel.attempts[].id must be id-safe";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (!seen_attempt_ids.insert(attempt_id).second) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "duplicate delegate_parallel attempt id";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      if (!a.isMember("request") || !a["request"].isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel attempt missing request object";
          o["task_id"] = task_id;
          o["attempt_id"] = attempt_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      const std::string attempt_task_id = task_id + ":" + attempt_id;
      if (!id_is_safe(attempt_task_id)) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel produced invalid derived task_id";
          o["task_id"] = task_id;
          o["attempt_task_id"] = attempt_task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      Json::Value at(Json::objectValue);
      at["task_id"] = attempt_task_id;
      at["allow_error"] =
        a.isMember("allow_error") && a["allow_error"].isBool() ? a["allow_error"].asBool() : true;
      if (t.isMember("inputs") && t["inputs"].isObject()) at["inputs"] = t["inputs"];
      if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
        at["ready_unix_ms"] = t["ready_unix_ms"];
      }
      if (!dep_ids.empty()) {
        Json::Value deps(Json::arrayValue);
        for (const auto& d : dep_ids) deps.append(d);
        at["depends_on"] = deps;
      }
      if (priority != 0) at["priority"] = priority;
      if (a.isMember("max_attempts") && a["max_attempts"].isInt()) at["max_attempts"] = a["max_attempts"];
      if (a.isMember("expect") && a["expect"].isObject()) at["expect"] = a["expect"];

      Json::Value areq = a["request"];
      if (!areq.isObject()) areq = Json::Value(Json::objectValue);

      // Delegate-parallel attempt requests behave like normal workflow tasks:
      // - workflow-level defaults are merged
      // - sessions/no_session are defaulted
      // - attempt_defaults (delegate-level) are merged with higher priority than workflow defaults
      if (attempt_defaults.isObject()) {
        for (const auto& k : attempt_defaults.getMemberNames()) {
          if (!areq.isMember(k)) areq[k] = attempt_defaults[k];
        }
      }
      if (workflow_defaults.isObject()) {
        for (const auto& k : workflow_defaults.getMemberNames()) {
          if (!areq.isMember(k)) areq[k] = workflow_defaults[k];
        }
      }

      // attempt_caps: hard maximums on per-attempt run knobs.
      // Semantics: if cap <= 0, ignore. Else: missing => set to cap; present => min(present, cap).
      if (attempt_caps.isObject()) {
        auto clamp_key = [&](const char* k) {
          if (!attempt_caps.isMember(k)) return;
          const Json::Value& capv = attempt_caps[k];
          if (!(capv.isInt64() || capv.isUInt64() || capv.isInt() || capv.isUInt())) return;
          const int64_t cap = std::max<int64_t>(0, capv.asInt64());
          if (cap <= 0) return;
          if (areq.isMember(k) && (areq[k].isInt64() || areq[k].isUInt64() || areq[k].isInt() || areq[k].isUInt())) {
            const int64_t cur = std::max<int64_t>(0, areq[k].asInt64());
            areq[k] = (Json::Int64)std::min<int64_t>(cur, cap);
          } else if (!areq.isMember(k)) {
            areq[k] = (Json::Int64)cap;
          }
        };
        clamp_key("timeout_ms");
        clamp_key("max_steps");
        clamp_key("max_tool_calls_total");
        clamp_key("max_tool_calls_per_tool");
        clamp_key("max_tool_call_args_chars");
        clamp_key("max_tool_result_chars");
      }

      if (!allow_sessions) {
        areq["no_session"] = true;
        if (!areq.isMember("tools")) areq["tools"] = "none";
      } else if (!session_id.empty()) {
        if (!areq.isMember("session_id") && (!areq.isMember("no_session") || !areq["no_session"].isBool() || !areq["no_session"].asBool())) {
          areq["session_id"] = session_id;
        }
      }

      if (areq.isMember("api_key") && areq["api_key"].isString() && !areq["api_key"].asString().empty() && !allow_inline_api_keys) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "inline api_key is not allowed for durable workflows (set daemon api_key/provider_keys or pass allow_inline_api_keys=true)";
          o["task_id"] = attempt_task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }

      if (!areq.isMember("trace_id") || !areq["trace_id"].isString() || areq["trace_id"].asString().empty()) {
        areq["trace_id"] = trace_id + ":" + task_id + ":" + attempt_id;
      }

      at["request"] = areq;
      out.append(at);
      attempt_task_ids.append(attempt_task_id);
    }

    if (attempt_task_ids.empty()) {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel.attempts must include at least one object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    // Replace the macro task with a deterministic aggregate join at the same task_id.
    Json::Value join(Json::objectValue);
    join["task_id"] = task_id;
    join["kind"] = "aggregate";
    if (priority != 0) join["priority"] = priority;
    if (t.isMember("allow_error") && t["allow_error"].isBool()) join["allow_error"] = t["allow_error"];
    if (t.isMember("inputs") && t["inputs"].isObject()) join["inputs"] = t["inputs"];
    if (t.isMember("ready_unix_ms") && (t["ready_unix_ms"].isInt64() || t["ready_unix_ms"].isUInt64() || t["ready_unix_ms"].isInt())) {
      join["ready_unix_ms"] = t["ready_unix_ms"];
    }

    Json::Value deps(Json::arrayValue);
    for (Json::ArrayIndex k = 0; k < attempt_task_ids.size(); k++) deps.append(attempt_task_ids[k]);
    join["depends_on"] = deps;

    Json::Value agg(Json::objectValue);
    if (del.isMember("aggregate") && !del["aggregate"].isNull()) {
      if (!del["aggregate"].isObject()) {
        if (resp) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "delegate_parallel.delegate.aggregate must be an object";
          o["task_id"] = task_id;
          resp->body = json_stringify_compact(o);
        }
        return false;
      }
      agg = del["aggregate"];
    }

    // delegate_parallel default join behavior: first_ok across attempts.
    if (!agg.isMember("mode") || !agg["mode"].isString() || trim_copy(agg["mode"].asString()).empty()) {
      agg["mode"] = "first_ok";
    }

    // Always override task_ids to match the derived attempt tasks (ignores any caller-provided aggregate.task_ids).
    agg["task_ids"] = attempt_task_ids;

    // Backward-compatible convenience: delegate.ok_pointer/value_pointer become defaults for the aggregate join
    // when the aggregate object does not explicitly set them.
    if (del.isMember("ok_pointer") && del["ok_pointer"].isString() && !del["ok_pointer"].asString().empty()) {
      if (!agg.isMember("ok_pointer") || !agg["ok_pointer"].isString() || trim_copy(agg["ok_pointer"].asString()).empty()) {
        agg["ok_pointer"] = del["ok_pointer"];
      }
    }
    if (del.isMember("value_pointer") && del["value_pointer"].isString() && !del["value_pointer"].asString().empty()) {
      if (!agg.isMember("value_pointer") || !agg["value_pointer"].isString() || trim_copy(agg["value_pointer"].asString()).empty()) {
        agg["value_pointer"] = del["value_pointer"];
      }
    }

    const std::string agg_mode =
      agg.isMember("mode") && agg["mode"].isString() ? trim_copy(agg["mode"].asString()) : std::string();
    if (agg_mode != "first_ok" && agg_mode != "quorum_ok" && agg_mode != "strict_all_ok" && agg_mode != "collect" && agg_mode != "best_of_n" && agg_mode != "quorum_hashes") {
      if (resp) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "delegate_parallel aggregate.mode must be one of: first_ok, quorum_ok, strict_all_ok, collect, best_of_n, quorum_hashes";
        o["task_id"] = task_id;
        o["mode"] = agg_mode;
        resp->body = json_stringify_compact(o);
      }
      return false;
    }

    // Ergonomic defaults for delegate_parallel quorum:
    // - pointers defaults to assistant_text (common surface for attempt outputs)
    // - note: aggregate's global default pointers (/avm/result_hash, /avm/trace_hash) are not meaningful for LLM attempts
    if (agg_mode == "quorum_hashes") {
      const bool has_ptrs =
        agg.isMember("pointers") && agg["pointers"].isArray() && !agg["pointers"].empty();
      if (!has_ptrs) {
        Json::Value ptrs(Json::arrayValue);
        ptrs.append("/assistant_text");
        agg["pointers"] = ptrs;
      }
      // Distinct-node quorum: use provider base_url as the default node identity for run-attempt tasks.
      // This enables require_distinct_nodes for multi-provider correctness checks.
      const bool has_node_pointer =
        agg.isMember("node_pointer") && agg["node_pointer"].isString() && !trim_copy(agg["node_pointer"].asString()).empty();
      if (!has_node_pointer) agg["node_pointer"] = "/effective_base_url";
    }
    join["aggregate"] = agg;

    if (t.isMember("max_attempts") && t["max_attempts"].isInt()) join["max_attempts"] = t["max_attempts"];
    if (t.isMember("expect") && t["expect"].isObject()) join["expect"] = t["expect"];

    out.append(join);
  }

  *io_tasks_arr = out;
  return true;
}

}  // namespace agentd
