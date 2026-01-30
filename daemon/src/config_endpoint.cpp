#include "config_endpoint.h"

#include "daemon_auth.h"
#include "json_util.h"

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
  daemon["auth_enabled"] = !cfg.auth_token.empty();
  daemon["allow_unauthenticated_non_loopback"] = cfg.allow_unauthenticated_non_loopback;
  daemon["db_path"] = cfg.db_path.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.db_path);
  daemon["state_dir"] = cfg.state_dir.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.state_dir);
  daemon["sessions_root_dir"] = cfg.sessions_root_dir.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.sessions_root_dir);
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
  sandbox["tools_root"] = cfg.tools_root.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.tools_root);
  sandbox["host_scope_root"] = cfg.host_scope_root.empty() ? Json::Value(Json::nullValue) : Json::Value(cfg.host_scope_root);
  sandbox["yolo_default"] = cfg.yolo_default;
  sandbox["host_policy"] = host_policy_to_string(cfg.host_policy);
  out["sandbox"] = sandbox;

  Json::Value jobs(Json::objectValue);
  jobs["job_ttl_ms"] = (Json::Int64)cfg.job_ttl_ms;
  jobs["max_jobs"] = (Json::UInt64)cfg.max_jobs;
  out["jobs"] = jobs;

  resp->body = json_stringify(out);
  return;
}

}  // namespace agentd
