#include "config_endpoint.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "runtime_config.h"
#include "provider_util.h"
#include "string_util.h"

#include <json/json.h>

namespace agentd {

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
