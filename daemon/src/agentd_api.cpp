#include "agentd/api.h"

#include "agent_db.h"
#include "config_endpoint.h"
#include "config_store.h"
#include "cors.h"
#include "daemon_auth.h"
#include "db_query_endpoints.h"
#include "file_endpoint.h"
#include "job_endpoints.h"
#include "orchestrate_endpoints.h"
#include "openrouter_models_endpoint.h"
#include "openrouter_util.h"
#include "provider_util.h"
#include "run_endpoints.h"
#include "trace_endpoints.h"
#include "runtime_config.h"
#include "secrets_file.h"
#include "session_endpoints.h"
#include "string_util.h"
#include "tools_endpoint.h"

#include "openai_client.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <string>
#include <thread>

namespace agentd {
namespace {

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) return h;
  return std::filesystem::current_path().string();
}

static std::string state_dir_best_effort() {
  // Default daemon state to its startup working directory (container-friendly).
  // Operators should set the daemon's working directory (or AGENT_WD/AGENTD_STATE_DIR) to control where
  // session folders and agentd.db are created.
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec && !cwd.empty()) return cwd.string();
  return home_dir_best_effort();
}

static bool host_is_loopback(std::string host) {
  host = lower_copy(std::move(host));
  if (host == "localhost") return true;
  if (host == "::1" || host == "[::1]") return true;
  if (host.rfind("127.", 0) == 0) return true;
  if (host == "127.0.0.1") return true;
  return false;
}

static CorsConfig cors_cfg_from_config(const DaemonConfig& cfg) {
  CorsConfig cors_cfg;
  cors_cfg.max_age_seconds = cfg.cors_max_age_seconds;
  cors_cfg.allow_headers = cfg.cors_allow_headers.empty()
    ? std::string("Content-Type, Authorization, X-OpenRouter-Key")
    : cfg.cors_allow_headers;
  cors_cfg.allow_methods = cfg.cors_allow_methods.empty()
    ? std::string("GET, POST, DELETE, OPTIONS")
    : cfg.cors_allow_methods;
  if (cfg.cors_disabled) {
    cors_cfg.origins.clear();
  } else if (cfg.cors_origins_set) {
    cors_cfg.origins = cfg.cors_origins;
  } else {
    if (host_is_loopback(cfg.listen_host)) {
      cors_cfg.origins = {"*"};
    } else {
      cors_cfg.origins.clear();
    }
  }
  return cors_cfg;
}

static void fill_env_defaults(DaemonConfig* cfg) {
  if (!cfg) return;

  // High-level operator-friendly root. If set, it pins default state_dir/sessions_root_dir unless overridden
  // by the more specific AGENTD_* vars.
  if (cfg->state_dir.empty()) {
    if (const char* d = getenv_s("AGENT_WD")) cfg->state_dir = d;
  }

  if (cfg->base_url.empty()) {
    if (const char* b = getenv_s("OPENAI_API_BASE")) cfg->base_url = b;
    else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) cfg->base_url = b2;
    else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) cfg->base_url = b3;
    else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) cfg->base_url = b4;
    else if (const char* b5 = getenv_s("MOONSHOT_API_BASE")) cfg->base_url = b5;
  }

  if (cfg->api_key.empty()) {
    if (url_contains_ci(cfg->base_url, "deepseek")) {
      if (const char* k = getenv_s("DEEPSEEK_API_KEY")) cfg->api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) cfg->api_key = k2;
      else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) cfg->api_key = k3;
    } else if (url_contains_ci(cfg->base_url, "openrouter")) {
      if (const char* k = getenv_s("OPENROUTER_API_KEY")) cfg->api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) cfg->api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg->api_key = k3;
    } else if (url_contains_ci(cfg->base_url, "moonshot")) {
      if (const char* k = getenv_s("KIMI_API_KEY_CN")) cfg->api_key = k;
      else if (const char* k2 = getenv_s("MOONSHOT_API_KEY")) cfg->api_key = k2;
      else if (const char* k3 = getenv_s("OPENAI_API_KEY")) cfg->api_key = k3;
    } else {
      if (const char* k = getenv_s("OPENAI_API_KEY")) cfg->api_key = k;
      else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) cfg->api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg->api_key = k3;
    }
  }

  if (cfg->api_key.empty()) {
    const std::string provider = provider_from_base_url(cfg->base_url);
    if (auto k = load_provider_key_best_effort(provider)) {
      cfg->api_key = *k;
    }
  }

  if (cfg->model.empty()) {
    if (const char* m = getenv_s("AGENT_MODEL")) cfg->model = m;
  }
  if (cfg->auth_token.empty()) {
    if (const char* t = getenv_s("AGENTD_AUTH_TOKEN")) cfg->auth_token = t;
  }
  if (cfg->db_path.empty()) {
    if (const char* p = getenv_s("AGENTD_DB_PATH")) cfg->db_path = p;
  }
  if (cfg->state_dir.empty()) {
    if (const char* d = getenv_s("AGENTD_STATE_DIR")) cfg->state_dir = d;
  }
  if (cfg->sessions_root_dir.empty()) {
    if (const char* d = getenv_s("AGENT_WD")) cfg->sessions_root_dir = d;
    if (const char* d = getenv_s("AGENTD_SESSIONS_ROOT")) cfg->sessions_root_dir = d;
  }
}

static void fill_path_defaults(DaemonConfig* cfg) {
  if (!cfg) return;
  if (cfg->state_dir.empty()) {
    // If sessions_root_dir was explicitly configured (e.g. --sessions-root), use it as the default state_dir too
    // so the daemon DB and session folders live under the same operator-chosen root.
    cfg->state_dir = cfg->sessions_root_dir.empty() ? state_dir_best_effort() : cfg->sessions_root_dir;
  }
  if (cfg->sessions_root_dir.empty()) {
    // Session root directory is `<state_dir>/session_<session_id>/`.
    cfg->sessions_root_dir = cfg->state_dir;
  }

  // DB is mandatory (canonical daemon state store). Default to agentd.db under state_dir.
  if (cfg->db_path.empty()) {
    cfg->db_path = (std::filesystem::path(cfg->state_dir) / "agentd.db").string();
  } else {
    const std::filesystem::path p(cfg->db_path);
    if (p.is_relative()) {
      cfg->db_path = (std::filesystem::path(cfg->state_dir) / p).lexically_normal().string();
    }
  }
}

static OpenAIClientConfig ocfg_from_cfg(const DaemonConfig& c) {
  OpenAIClientConfig ocfg;
  ocfg.base_url = c.base_url;
  ocfg.api_key = c.api_key;
  ocfg.model = c.model;
  ocfg.proxy_url = c.proxy_url;
  ocfg.timeout_ms = c.timeout_ms;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) ocfg.openrouter_http_referer = r;
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) ocfg.openrouter_x_title = t;
  return ocfg;
}

}  // namespace

struct AgentdApi::Impl {
  explicit Impl(Options options) : options(std::move(options)) {}

  Options options;
  bool initialized = false;
  CorsConfig cors_cfg{};
  AgentDb db;
  std::unique_ptr<DaemonConfigStore> cfg_store;

  std::map<RouteKey, Handler> routes;

  const ToolExtension* tool_ext_or_null() const {
    if (!options.enable_tool_extension) return nullptr;
    return &options.tool_extension;
  }

  void route(const std::string& method, const std::string& path, Handler h) {
    routes[RouteKey{method, path}] = h;
  }
};

AgentdApi::AgentdApi(Options options) : impl_(new Impl(std::move(options))) {}
AgentdApi::~AgentdApi() {
  delete impl_;
  impl_ = nullptr;
}

bool AgentdApi::init(std::string* out_error) {
  if (out_error) out_error->clear();
  if (!impl_) return false;
  if (impl_->initialized) return true;

  (void)::signal(SIGPIPE, SIG_IGN);

  DaemonConfig cfg = impl_->options.cfg;
  fill_env_defaults(&cfg);
  fill_path_defaults(&cfg);
  impl_->cors_cfg = cors_cfg_from_config(cfg);

  {
    std::string db_err;
    if (!impl_->db.open(cfg.db_path, &db_err)) {
      if (out_error) *out_error = db_err.empty() ? "failed to open agentd DB" : db_err;
      return false;
    }
  }

  // Load runtime-configured daemon defaults (model/base_url/proxy/timeout + provider keys) from the DB.
  {
    std::string err;
    if (!load_runtime_config_best_effort(impl_->db, &cfg, &err)) {
      std::cerr << "Warning: failed to load runtime config from DB: " << err << "\n";
    }
  }

  impl_->options.cfg = cfg;
  impl_->cfg_store = std::make_unique<DaemonConfigStore>(cfg);

  // Route registration.
  impl_->route("GET", "/api/v1/health", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":true,"service":"agentd","version":"0.1"})";
  });

  impl_->route("GET", "/api/v1/config", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_config_endpoint(cur, self->cors_cfg, req, resp);
  });

  impl_->route("POST", "/api/v1/config/update", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_config_update_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });

  impl_->route("GET", "/api/v1/tools", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_tools_endpoint(cur, self->cors_cfg, cur.sessions_root_dir, self->tool_ext_or_null(), req, resp);
  });

  impl_->route("GET", "/api/v1/openrouter/models", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = self->cfg_store->snapshot();
    if (!daemon_require_auth(cur, req, resp)) return;
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_openrouter_models_endpoint(ocfg, !cur.auth_token.empty(), req, resp);
  });

  impl_->route("GET", "/api/v1/file", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_file_endpoint(cur, self->cors_cfg, req, resp);
  });

  impl_->route("GET", "/api/v1/sessions", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_sessions_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/session/new", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_new_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/audit", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_audit_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/client_events", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_client_events_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/clients", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_clients_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/artifacts", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_artifacts_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/scene", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_scene_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/session/scene/apply", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_scene_apply_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/session/ui_event", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_ui_event_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/session/client_event", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_ui_event_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/session/upload", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_upload_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("DELETE", "/api/v1/session", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_delete_endpoint(cur, self->cors_cfg, &self->db, cur.sessions_root_dir, req, resp);
  });

  // DB query endpoints (read-only).
  impl_->route("GET", "/api/v1/db/runs", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_runs_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/run", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_run_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/artifacts", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_artifacts_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/ui_actions", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_ui_actions_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/sessions", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_sessions_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/messages", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_messages_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/client_events", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_client_events_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  // Run endpoints.
  impl_->route("POST", "/api/v1/run", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_run_endpoint(cur, ocfg, self->cors_cfg, &self->db, self->tool_ext_or_null(), cur.sessions_root_dir, req, resp);
  });
  impl_->route("POST", "/api/v1/run_async", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_run_async_endpoint(cur, ocfg, self->cors_cfg, &self->db, self->tool_ext_or_null(), cur.sessions_root_dir, req, resp);
  });
  impl_->route("POST", "/api/v1/orchestrate", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_orchestrate_endpoint(cur, ocfg, self->cors_cfg, &self->db, self->tool_ext_or_null(), cur.sessions_root_dir, req, resp);
  });

  impl_->route("GET", "/api/v1/trace", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_trace_lookup_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  // Job endpoints (non-streaming).
  impl_->route("GET", "/api/v1/job", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_job_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/job/cancel", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_job_cancel_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("DELETE", "/api/v1/job", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_job_delete_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  impl_->initialized = true;
  return true;
}

bool AgentdApi::handle(const HttpRequest& req, HttpResponse* resp) {
  if (!resp) return false;
  resp->status = 200;
  resp->headers.clear();
  resp->body.clear();
  if (!impl_ || !impl_->initialized) {
    resp->status = 500;
    resp->body = "{\"ok\":false,\"error\":\"agentd api not initialized\"}";
    return true;
  }

  // Minimal OPTIONS behavior (used by HTTP transport CORS preflight).
  if (req.method == "OPTIONS") {
    resp->status = 204;
    resp->body.clear();
    cors_apply(req, resp, impl_->cors_cfg);
    return true;
  }

  const auto it = impl_->routes.find(RouteKey{req.method, req.path});
  if (it == impl_->routes.end()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"not found\"}";
    cors_apply(req, resp, impl_->cors_cfg);
    return false;
  }
  it->second(impl_, req, resp);
  return true;
}

std::string AgentdApi::db_path() const { return impl_ ? impl_->options.cfg.db_path : ""; }
std::string AgentdApi::listen_host() const { return impl_ ? impl_->options.cfg.listen_host : ""; }
uint16_t AgentdApi::listen_port() const { return impl_ ? impl_->options.cfg.listen_port : 0; }

}  // namespace agentd
