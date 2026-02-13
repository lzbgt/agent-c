#include "caps_endpoint.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "sandbox_policy.h"
#include "string_util.h"

#include <chrono>
#include <cstdlib>
#include <json/json.h>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count();
}

int64_t uptime_ms(std::chrono::steady_clock::time_point start_time) {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now() - start_time)
           .count();
}

std::string getenv_trim(const char* key) {
  if (!key) return "";
  const char* v = std::getenv(key);
  if (!v || !v[0]) return "";
  return trim_copy(std::string(v));
}

bool env_truthy(const std::string& v) {
  const std::string s = lower_copy(trim_copy(v));
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

size_t read_env_size(const char* key, size_t def, size_t max_cap) {
  std::string s = getenv_trim(key);
  if (s.empty()) return def;
  if (s == "0") return 0;
  try {
    unsigned long long parsed = std::stoull(s);
    if (parsed == 0) return 0;
    if (parsed > max_cap) parsed = max_cap;
    return (size_t)parsed;
  } catch (...) {
    return def;
  }
}

int read_env_int(const char* key, int def, int max_cap) {
  std::string s = getenv_trim(key);
  if (s.empty()) return def;
  if (s == "0") return 0;
  try {
    long long parsed = std::stoll(s);
    if (parsed == 0) return 0;
    if (parsed > max_cap) parsed = max_cap;
    if (parsed < 0) parsed = def;
    return (int)parsed;
  } catch (...) {
    return def;
  }
}

}  // namespace

void handle_caps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  std::chrono::steady_clock::time_point start_time,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["service"] = "agentd";
  out["version"] = "0.1";
  out["api_version"] = "v1";
  out["now_unix_ms"] = Json::Int64(now_unix_ms());
  out["uptime_ms"] = Json::Int64(uptime_ms(start_time));

  Json::Value features(Json::objectValue);
  {
    Json::Value auth(Json::objectValue);
    auth["enabled"] = !cfg.auth_token.empty();
    auth["allow_unauthenticated_non_loopback"] = cfg.allow_unauthenticated_non_loopback;
    features["auth"] = auth;
  }
  {
    Json::Value tools(Json::objectValue);
    tools["default_mode"] = cfg.tools;
    tools["yolo_default"] = cfg.yolo_default;
    tools["host_policy"] = host_policy_to_string(cfg.host_policy);
    tools["no_default_system"] = cfg.no_default_system;
    tools["system_profile"] = cfg.system_profile;
    features["tools"] = tools;
  }
  {
    Json::Value jobs(Json::objectValue);
    jobs["enabled"] = true;
    jobs["engine_max_concurrency"] = cfg.job_engine_max_concurrency;
    jobs["engine_poll_ms"] = cfg.job_engine_poll_ms;
    features["jobs"] = jobs;
  }
  {
    Json::Value wf(Json::objectValue);
    wf["enabled"] = true;
    Json::Value engine(Json::objectValue);
    engine["max_concurrency"] = cfg.workflow_engine_max_concurrency;
    engine["poll_ms"] = cfg.workflow_engine_poll_ms;
    engine["fair_queue_policy"] = cfg.workflow_engine_fair_queue_policy;
    engine["fair_queue_max_session_weight"] = cfg.workflow_engine_fair_queue_max_session_weight;
    engine["fair_queue_max_schedule_len"] = cfg.workflow_engine_fair_queue_max_schedule_len;
    engine["drr_cost_model"] = cfg.workflow_engine_drr_cost_model;
    wf["engine"] = engine;

    Json::Value fairness(Json::objectValue);
    fairness["max_inflight_per_workflow"] = cfg.workflow_engine_max_inflight_per_workflow;
    fairness["max_inflight_per_session"] = cfg.workflow_engine_max_inflight_per_session;
    wf["fairness"] = fairness;

    Json::Value admission(Json::objectValue);
    admission["max_inflight_tasks_per_session"] = cfg.workflow_admit_max_inflight_tasks_per_session;
    admission["max_inflight_tasks_total"] = cfg.workflow_admit_max_inflight_tasks_total;
    wf["admission"] = admission;

    Json::Value http_tasks(Json::objectValue);
    http_tasks["enabled"] = cfg.workflow_enable_http_tasks;
    {
      Json::Value arr(Json::arrayValue);
      for (const auto& h : cfg.workflow_http_allow_hosts) arr.append(h);
      http_tasks["allow_hosts"] = arr;
    }
    {
      Json::Value arr(Json::arrayValue);
      for (const auto& c : cfg.workflow_http_allow_cidrs) arr.append(c);
      http_tasks["allow_cidrs"] = arr;
    }
    {
      Json::Value arr(Json::arrayValue);
      for (const auto& c : cfg.workflow_http_deny_cidrs) arr.append(c);
      http_tasks["deny_cidrs"] = arr;
    }
    http_tasks["deny_private_addrs"] = cfg.workflow_http_deny_private_addrs;
    http_tasks["dns_pin"] = cfg.workflow_http_dns_pin;
    wf["http_tasks"] = http_tasks;

    features["workflow"] = wf;
  }
  {
    Json::Value edge(Json::objectValue);
    edge["enabled"] = true;
    edge["auth_required"] = cfg.edge_auth_required;
    edge["auth_require_ts"] = cfg.edge_auth_require_ts;
    edge["auth_max_skew_ms"] = Json::Int64(cfg.edge_auth_max_skew_ms);
    edge["auth_require_seq"] = cfg.edge_auth_require_seq;
    edge["auth_kid_policy"] = cfg.edge_auth_kid_policy;
    features["edge"] = edge;
  }
  {
    Json::Value mem(Json::objectValue);
    mem["consolidate_interval_ms"] = Json::Int64(cfg.memory_consolidate_interval_ms);
    mem["consolidate_daily_days"] = cfg.memory_consolidate_daily_days;
    mem["consolidate_keep_checkpoints"] = cfg.memory_consolidate_keep_checkpoints;
    features["memory"] = mem;
  }
  {
    Json::Value avm(Json::objectValue);
#if defined(_WIN32)
    const bool avm_supported = false;
#else
    const bool avm_supported = true;
#endif
    const std::string avm_bin = getenv_trim("AGENTD_AVM_BIN");
    const bool avm_exec = env_truthy(getenv_trim("AGENTD_AVM_EXEC"));
    avm["supported"] = avm_supported;
    avm["bin_set"] = !avm_bin.empty();
    avm["exec_enabled"] = avm_exec;
    features["avm"] = avm;
  }
  out["features"] = features;

  Json::Value limits(Json::objectValue);
  limits["upload_max_bytes"] = Json::UInt64(cfg.upload_max_bytes);
  limits["max_chars_default"] = Json::UInt64(cfg.max_chars_default);
  limits["keep_last_default"] = Json::UInt64(cfg.keep_last_default);
  limits["max_steps_default"] = Json::UInt64(cfg.max_steps_default);
  limits["max_tool_calls_total_default"] = Json::UInt64(cfg.max_tool_calls_total_default);
  limits["max_tool_calls_per_tool_default"] = Json::UInt64(cfg.max_tool_calls_per_tool_default);
  limits["max_tool_call_args_chars_default"] = Json::UInt64(cfg.max_tool_call_args_chars_default);
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : cfg.tool_call_limits_default) {
      Json::Value o(Json::objectValue);
      o["tool"] = p.first;
      o["max_calls"] = Json::UInt64(p.second);
      arr.append(o);
    }
    limits["tool_call_limits_default"] = arr;
  }
  {
    Json::Value job_gc(Json::objectValue);
    job_gc["job_ttl_ms"] = Json::Int64(cfg.job_ttl_ms);
    job_gc["max_jobs"] = Json::UInt64(cfg.max_jobs);
    limits["jobs"] = job_gc;
  }
  {
    Json::Value http(Json::objectValue);
    http["max_body_bytes"] = Json::UInt64(read_env_size("AGENTD_HTTP_MAX_BODY_BYTES", 64ull * 1024ull * 1024ull, 512ull * 1024ull * 1024ull));
    http["max_header_bytes"] = Json::UInt64(read_env_size("AGENTD_HTTP_MAX_HEADER_BYTES", 1ull * 1024ull * 1024ull, 32ull * 1024ull * 1024ull));
    http["read_timeout_ms"] = read_env_int("AGENTD_HTTP_READ_TIMEOUT_MS", 15000, 300000);
    limits["http"] = http;
  }
  out["limits"] = limits;

  resp->body = json_stringify(out);
}

}  // namespace agentd
