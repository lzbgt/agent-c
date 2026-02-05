#include "config_endpoint.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "runtime_config.h"
#include "provider_util.h"
#include "string_util.h"

#include <json/json.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace agentd {

namespace {

static bool validate_cidr_token_best_effort(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  const size_t slash = s.find('/');
  if (slash == std::string::npos) return false;
  const std::string host = trim_copy(s.substr(0, slash));
  const std::string pref_s = trim_copy(s.substr(slash + 1));
  if (host.empty() || pref_s.empty()) return false;
  int pref = 0;
  try {
    pref = std::stoi(pref_s);
  } catch (...) {
    return false;
  }
  uint8_t buf[16];
  if (::inet_pton(AF_INET, host.c_str(), buf) == 1) {
    return pref >= 0 && pref <= 32;
  }
  if (::inet_pton(AF_INET6, host.c_str(), buf) == 1) {
    return pref >= 0 && pref <= 128;
  }
  return false;
}

static bool validate_hostport_token_best_effort(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 512) return false;
  // No whitespace.
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
  }
  // Allow:
  // - host
  // - host:port (single ':')
  // - [ipv6] or [ipv6]:port
  std::string host;
  std::string port_s;
  if (!s.empty() && s[0] == '[') {
    const size_t rb = s.find(']');
    if (rb == std::string::npos) return false;
    host = s.substr(1, rb - 1);
    if (rb + 1 < s.size()) {
      if (s[rb + 1] != ':') return false;
      port_s = s.substr(rb + 2);
    }
  } else {
    const size_t col = s.rfind(':');
    if (col != std::string::npos && s.find(':') == col) {
      host = s.substr(0, col);
      port_s = s.substr(col + 1);
    } else {
      host = s;
    }
  }
  host = trim_copy(host);
  port_s = trim_copy(port_s);
  if (host.empty()) return false;
  if (!port_s.empty()) {
    int p = 0;
    try {
      p = std::stoi(port_s);
    } catch (...) {
      return false;
    }
    if (p < 1 || p > 65535) return false;
  }
  return true;
}

static bool read_string_array_best_effort(
  const Json::Value& obj,
  const char* k,
  std::vector<std::string>* out,
  size_t max_n,
  size_t max_len,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out) return false;
  out->clear();
  if (!k || !obj.isMember(k)) return true;
  const Json::Value& v = obj[k];
  if (v.isNull()) return true;
  if (!v.isArray()) {
    if (out_err) *out_err = std::string(k) + " must be an array";
    return false;
  }
  if (v.size() > (Json::ArrayIndex)max_n) {
    if (out_err) *out_err = std::string(k) + " too large";
    return false;
  }
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (!v[i].isString()) continue;
    std::string s = trim_copy(v[i].asString());
    if (s.empty()) continue;
    if (s.size() > max_len) {
      if (out_err) *out_err = std::string(k) + " entry too long";
      return false;
    }
    out->push_back(std::move(s));
  }
  return true;
}

}  // namespace

void handle_config_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
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
  out["have_jsoncpp"] = true;
#if defined(AGENT_HAVE_SQLITE3)
  out["have_sqlite3"] = true;
#else
  out["have_sqlite3"] = false;
#endif

  Json::Value daemon(Json::objectValue);
  daemon["listen_host"] = cfg.listen_host;
  daemon["listen_port"] = (Json::UInt64)cfg.listen_port;
  daemon["base_url"] = cfg.base_url;
  daemon["model"] = cfg.model;
  daemon["summary_model"] = cfg.summary_model.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.summary_model);
  daemon["summary_max_chars"] = (Json::UInt64)cfg.summary_max_chars;
  daemon["timeout_ms"] = (Json::Int64)cfg.timeout_ms;
  daemon["proxy_url_set"] = !cfg.proxy_url.empty();
  daemon["api_key_set"] = !cfg.api_key.empty();
  {
    Json::Value keys(Json::objectValue);
    auto has_key = [&](const char* p) -> bool {
      const auto it = cfg.provider_keys.find(p ? p : "");
      return it != cfg.provider_keys.end() && !it->second.empty();
    };
    keys["deepseek"] = has_key("deepseek");
    keys["openrouter"] = has_key("openrouter");
    keys["moonshot"] = has_key("moonshot");
    keys["openai"] = has_key("openai");
    daemon["provider_keys_set"] = keys;
  }
  daemon["auth_enabled"] = !cfg.auth_token.empty();
  daemon["allow_unauthenticated_non_loopback"] = cfg.allow_unauthenticated_non_loopback;
  daemon["db_path"] = cfg.db_path.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.db_path);
  daemon["state_dir"] = cfg.state_dir.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.state_dir);
  daemon["sessions_root_dir"] = cfg.sessions_root_dir.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.sessions_root_dir);
  daemon["max_steps_default"] = (Json::UInt64)cfg.max_steps_default;
  daemon["max_tool_calls_total_default"] = (Json::UInt64)cfg.max_tool_calls_total_default;
  daemon["max_tool_calls_per_tool_default"] = (Json::UInt64)cfg.max_tool_calls_per_tool_default;
  {
    Json::Value arr(Json::arrayValue);
    for (const auto& p : cfg.tool_call_limits_default) {
      Json::Value o(Json::objectValue);
      o["tool"] = p.first;
      o["max_calls"] = (Json::UInt64)p.second;
      arr.append(o);
    }
    daemon["tool_call_limits_default"] = arr;
  }
  out["daemon"] = daemon;

  Json::Value cors(Json::objectValue);
  cors["enabled"] = !cors_cfg.origins.empty();
  Json::Value origins(Json::arrayValue);
  for (const auto& o : cors_cfg.origins) origins.append(o);
  cors["origins"] = origins;
  cors["allow_headers"] = cors_cfg.allow_headers;
  cors["allow_methods"] = cors_cfg.allow_methods;
  cors["max_age_seconds"] = cors_cfg.max_age_seconds;
  out["cors"] = cors;

  Json::Value sandbox(Json::objectValue);
  sandbox["tools"] = cfg.tools;
  sandbox["yolo_default"] = cfg.yolo_default;
  sandbox["host_policy"] = host_policy_to_string(cfg.host_policy);
  sandbox["system_profile"] = cfg.system_profile;
  out["sandbox"] = sandbox;

  Json::Value jobs(Json::objectValue);
  jobs["job_ttl_ms"] = (Json::Int64)cfg.job_ttl_ms;
  jobs["max_jobs"] = (Json::UInt64)cfg.max_jobs;
  out["jobs"] = jobs;

  Json::Value engines(Json::objectValue);
  engines["job_max_concurrency"] = cfg.job_engine_max_concurrency;
  engines["job_poll_ms"] = cfg.job_engine_poll_ms;
  engines["workflow_max_concurrency"] = cfg.workflow_engine_max_concurrency;
  engines["workflow_poll_ms"] = cfg.workflow_engine_poll_ms;
  engines["workflow_max_inflight_per_workflow"] = cfg.workflow_engine_max_inflight_per_workflow;
  engines["workflow_max_inflight_per_session"] = cfg.workflow_engine_max_inflight_per_session;
  engines["workflow_fair_queue_policy"] = cfg.workflow_engine_fair_queue_policy;
  engines["workflow_fair_queue_max_session_weight"] = cfg.workflow_engine_fair_queue_max_session_weight;
  engines["workflow_fair_queue_max_schedule_len"] = cfg.workflow_engine_fair_queue_max_schedule_len;
  engines["workflow_drr_cost_model"] = cfg.workflow_engine_drr_cost_model;
  engines["workflow_admit_max_inflight_tasks_per_session"] = cfg.workflow_admit_max_inflight_tasks_per_session;
  engines["workflow_admit_max_inflight_tasks_total"] = cfg.workflow_admit_max_inflight_tasks_total;
  engines["workflow_enable_http_tasks"] = cfg.workflow_enable_http_tasks;
  if (!cfg.workflow_http_allow_hosts.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& h : cfg.workflow_http_allow_hosts) {
      if (!h.empty()) arr.append(h);
    }
    engines["workflow_http_allow_hosts"] = arr;
  }
  if (!cfg.workflow_http_allow_cidrs.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& c : cfg.workflow_http_allow_cidrs) {
      if (!c.empty()) arr.append(c);
    }
    engines["workflow_http_allow_cidrs"] = arr;
  }
  engines["workflow_http_deny_private_addrs"] = cfg.workflow_http_deny_private_addrs;
  if (!cfg.workflow_http_deny_cidrs.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& c : cfg.workflow_http_deny_cidrs) {
      if (!c.empty()) arr.append(c);
    }
    engines["workflow_http_deny_cidrs"] = arr;
  }
  engines["workflow_http_dns_pin"] = cfg.workflow_http_dns_pin;
  out["engines"] = engines;

  Json::Value memory(Json::objectValue);
  memory["consolidate_interval_ms"] = (Json::Int64)cfg.memory_consolidate_interval_ms;
  memory["consolidate_daily_days"] = cfg.memory_consolidate_daily_days;
  memory["consolidate_keep_checkpoints"] = cfg.memory_consolidate_keep_checkpoints;
  out["memory"] = memory;

  resp->body = json_stringify(out);
  return;
}

void handle_config_update_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!cfg_store) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"missing cfg_store"})";
    return;
  }
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }
  const DaemonConfig old_cfg = cfg_store->snapshot();
  if (!daemon_require_auth(old_cfg, req, resp)) return;

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify(o);
    return;
  }

  DaemonConfig next = old_cfg;

  // Apply updates (non-secret defaults).
  if (args.isMember("base_url") && args["base_url"].isString()) {
    next.base_url = args["base_url"].asString();
  }
  if (args.isMember("model") && args["model"].isString()) {
    next.model = args["model"].asString();
  }
  if (args.isMember("summary_model")) {
    if (args["summary_model"].isNull()) next.summary_model.clear();
    else if (args["summary_model"].isString()) next.summary_model = args["summary_model"].asString();
  }
  if (args.isMember("summary_max_chars") && args["summary_max_chars"].isInt64()) {
    const auto n = args["summary_max_chars"].asInt64();
    if (n >= 0) next.summary_max_chars = (size_t)n;
  }
  if (args.isMember("proxy_url")) {
    if (args["proxy_url"].isNull()) next.proxy_url.clear();
    else if (args["proxy_url"].isString()) next.proxy_url = args["proxy_url"].asString();
  }
  if (args.isMember("timeout_ms") && args["timeout_ms"].isInt64()) {
    const auto n = args["timeout_ms"].asInt64();
    if (n > 0) next.timeout_ms = (long)n;
  }
  if (args.isMember("workflow_admit_max_inflight_tasks_per_session") && args["workflow_admit_max_inflight_tasks_per_session"].isInt()) {
    const int n = args["workflow_admit_max_inflight_tasks_per_session"].asInt();
    next.workflow_admit_max_inflight_tasks_per_session = std::max(0, std::min(100000, n));
  }
  if (args.isMember("workflow_admit_max_inflight_tasks_total") && args["workflow_admit_max_inflight_tasks_total"].isInt()) {
    const int n = args["workflow_admit_max_inflight_tasks_total"].asInt();
    next.workflow_admit_max_inflight_tasks_total = std::max(0, std::min(1000000, n));
  }
  if (args.isMember("system_profile") && args["system_profile"].isString()) {
    const std::string p = trim_copy(args["system_profile"].asString());
    auto is_valid = [&](const std::string& s) -> bool {
      if (s.empty() || s.size() > 64) return false;
      for (const char c : s) {
        const bool ok =
          (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') ||
          c == '-' || c == '_' || c == '.';
        if (!ok) return false;
      }
      return true;
    };
    if (is_valid(p)) {
      if (p == "default" || p == "jules_codex") {
        next.system_profile = p;
      }
    }
  }

  // Workflow outbound HTTP policy knobs (non-secret, runtime-mutable).
  {
    std::vector<std::string> allow_hosts;
    std::vector<std::string> allow_cidrs;
    std::vector<std::string> deny_cidrs;
    std::string verr;
    if (!read_string_array_best_effort(args, "workflow_http_allow_hosts", &allow_hosts, /*max_n=*/128, /*max_len=*/512, &verr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid workflow_http_allow_hosts" : verr;
      resp->body = json_stringify(o);
      return;
    }
    if (!read_string_array_best_effort(args, "workflow_http_allow_cidrs", &allow_cidrs, /*max_n=*/128, /*max_len=*/256, &verr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid workflow_http_allow_cidrs" : verr;
      resp->body = json_stringify(o);
      return;
    }
    if (!read_string_array_best_effort(args, "workflow_http_deny_cidrs", &deny_cidrs, /*max_n=*/128, /*max_len=*/256, &verr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = verr.empty() ? "invalid workflow_http_deny_cidrs" : verr;
      resp->body = json_stringify(o);
      return;
    }

    // If fields are present, validate and apply.
    if (args.isMember("workflow_http_allow_hosts")) {
      for (const auto& h : allow_hosts) {
        if (!validate_hostport_token_best_effort(h)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "invalid workflow_http_allow_hosts entry";
          o["value"] = h;
          resp->body = json_stringify(o);
          return;
        }
      }
      next.workflow_http_allow_hosts = allow_hosts;
    }
    if (args.isMember("workflow_http_allow_cidrs")) {
      for (const auto& c : allow_cidrs) {
        if (!validate_cidr_token_best_effort(c)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "invalid workflow_http_allow_cidrs entry";
          o["value"] = c;
          resp->body = json_stringify(o);
          return;
        }
      }
      next.workflow_http_allow_cidrs = allow_cidrs;
    }
    if (args.isMember("workflow_http_deny_cidrs")) {
      for (const auto& c : deny_cidrs) {
        if (!validate_cidr_token_best_effort(c)) {
          resp->status = 400;
          Json::Value o(Json::objectValue);
          o["ok"] = false;
          o["error"] = "invalid workflow_http_deny_cidrs entry";
          o["value"] = c;
          resp->body = json_stringify(o);
          return;
        }
      }
      next.workflow_http_deny_cidrs = deny_cidrs;
    }
    if (args.isMember("workflow_http_deny_private_addrs") && args["workflow_http_deny_private_addrs"].isBool()) {
      next.workflow_http_deny_private_addrs = args["workflow_http_deny_private_addrs"].asBool();
    }
    if (args.isMember("workflow_http_dns_pin") && args["workflow_http_dns_pin"].isBool()) {
      next.workflow_http_dns_pin = args["workflow_http_dns_pin"].asBool();
    }
  }

  // Provider keys:
  // - provider_keys: { deepseek:"...", openrouter:"...", openai:"..." }
  // - or (provider + api_key): set a single provider key
  auto set_provider_key = [&](const std::string& provider, const std::string& key) {
    if (provider.empty()) return;
    // Empty string clears.
    if (key.empty()) {
      next.provider_keys.erase(provider);
      return;
    }
    next.provider_keys[provider] = key;
  };
  if (args.isMember("provider_keys") && args["provider_keys"].isObject()) {
    const auto& pk = args["provider_keys"];
    for (const auto& name : pk.getMemberNames()) {
      const auto& v = pk[name];
      if (!v.isString() && !v.isNull()) continue;
      const std::string provider = name;
      if (!v.isString()) {
        set_provider_key(provider, "");
      } else {
        set_provider_key(provider, v.asString());
      }
    }
  } else {
    const std::string provider =
      args.isMember("provider") && args["provider"].isString() ? args["provider"].asString()
      : provider_from_base_url(next.base_url);
    if (args.isMember("api_key") && args["api_key"].isString()) {
      set_provider_key(provider, args["api_key"].asString());
    }
  }

  // Persist to daemon DB so defaults survive restarts.
  std::string werr;
  if (!save_runtime_config_best_effort(*db, next, &werr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = werr.empty() ? "failed to persist runtime config" : werr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  std::string serr;
  if (!save_runtime_secrets_best_effort(*db, next, &serr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = serr.empty() ? "failed to persist runtime secrets" : serr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  cfg_store->replace(next);

  // Return a safe snapshot (no secrets).
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["base_url"] = next.base_url;
  o["model"] = next.model;
  o["system_profile"] = next.system_profile;
  o["summary_model"] = next.summary_model.empty() ? Json::Value(Json::nullValue) : Json::Value(next.summary_model);
  o["summary_max_chars"] = (Json::UInt64)next.summary_max_chars;
  o["timeout_ms"] = (Json::Int64)next.timeout_ms;
  o["proxy_url_set"] = !next.proxy_url.empty();
  {
    Json::Value engines(Json::objectValue);
    Json::Value ah(Json::arrayValue);
    for (const auto& s : next.workflow_http_allow_hosts) if (!s.empty()) ah.append(s);
    Json::Value ac(Json::arrayValue);
    for (const auto& s : next.workflow_http_allow_cidrs) if (!s.empty()) ac.append(s);
    Json::Value dc(Json::arrayValue);
    for (const auto& s : next.workflow_http_deny_cidrs) if (!s.empty()) dc.append(s);
    engines["workflow_http_allow_hosts"] = ah;
    engines["workflow_http_allow_cidrs"] = ac;
    engines["workflow_http_deny_cidrs"] = dc;
    engines["workflow_http_deny_private_addrs"] = next.workflow_http_deny_private_addrs;
    engines["workflow_http_dns_pin"] = next.workflow_http_dns_pin;
    o["engines"] = engines;
  }
  {
    Json::Value keys(Json::objectValue);
    auto has_key = [&](const char* p) -> bool {
      const auto it = next.provider_keys.find(p ? p : "");
      return it != next.provider_keys.end() && !it->second.empty();
    };
    keys["deepseek"] = has_key("deepseek");
    keys["openrouter"] = has_key("openrouter");
    keys["moonshot"] = has_key("moonshot");
    keys["openai"] = has_key("openai");
    o["provider_keys_set"] = keys;
  }
  resp->body = json_stringify(o);
  return;
}

}  // namespace agentd
