#include "agentd/api.h"

#include "agent_db.h"
#include "approval_queue_endpoints.h"
#include "blob_endpoints.h"
#include "caps_endpoint.h"
#include "client_prefs_endpoints.h"
#include "config_endpoint.h"
#include "config_store.h"
#include "cors.h"
#include "daemon_auth.h"
#include "diagnostics_endpoints.h"
#include "db_query_endpoints.h"
#include "edge_interop_endpoints.h"
#include "edge_runtime_endpoints.h"
#include "file_endpoint.h"
#include "health_endpoint.h"
#include "http_util.h"
#include "job_manager.h"
#include "job_endpoints.h"
#include "memory_endpoints.h"
#include "moderator_endpoints.h"
#include "orchestrate_endpoints.h"
#include "ota_endpoints.h"
#include "openrouter_models_endpoint.h"
#include "openrouter_util.h"
#include "provider_util.h"
#include "run_endpoints.h"
#include "run_replay_endpoint.h"
#include "trace_endpoints.h"
#include "workflow_endpoints.h"
#include "workflow_schedule_endpoints.h"
#include "runtime_config.h"
#include "secrets_file.h"
#include "session_endpoints.h"
#include "session_voice_runtime.h"
#include "string_util.h"
#include "tools_endpoint.h"

#include "openai_client.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <signal.h>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

namespace agentd {
namespace {

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static bool env_truthy(const char* s) {
  if (!s) return false;
  std::string v = s;
  for (char& c : v) c = (char)std::tolower((unsigned char)c);
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) return h;
#if !defined(_WIN32)
  if (struct passwd* pw = getpwuid(getuid())) {
    if (pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
  }
#endif
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
    ? std::string("Content-Type, Authorization, X-OpenRouter-Key, X-Request-Id, X-Trace-Id")
    : cfg.cors_allow_headers;
  cors_cfg.allow_methods = cfg.cors_allow_methods.empty()
    ? std::string("GET, POST, DELETE, OPTIONS")
    : cfg.cors_allow_methods;
  cors_cfg.allow_credentials = cfg.cors_allow_credentials;
  cors_cfg.routes.clear();
  cors_cfg.routes.reserve(cfg.cors_routes.size());
  for (const auto& r : cfg.cors_routes) {
    CorsRoute cr;
    cr.path_prefix = r.path_prefix;
    cr.origins = r.origins;
    cors_cfg.routes.push_back(std::move(cr));
  }
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
  cors_compile(&cors_cfg);
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
  if (cfg->run_attest_hmac_kid.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_HMAC_KID")) cfg->run_attest_hmac_kid = t;
  }
  if (cfg->run_attest_hmac_key.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_HMAC_KEY")) cfg->run_attest_hmac_key = t;
  }
  if (cfg->run_attest_ed25519_kid.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_ED25519_KID")) cfg->run_attest_ed25519_kid = t;
  }
  if (cfg->run_attest_ed25519_seed.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_ED25519_SEED")) cfg->run_attest_ed25519_seed = t;
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
  if (cfg->audio_webrtc_peer_tool_path.empty()) {
    if (const char* p = getenv_s("AGENTD_AUDIO_WEBRTC_PEER_TOOL")) cfg->audio_webrtc_peer_tool_path = p;
  }
  if (cfg->audio_webrtc_peer_node_bin.empty()) {
    if (const char* p = getenv_s("AGENTD_AUDIO_WEBRTC_PEER_NODE_BIN")) cfg->audio_webrtc_peer_node_bin = p;
  }
  if (cfg->edge_consensus_node_tool_path.empty()) {
    if (const char* p = getenv_s("AGENTD_EDGE_CONSENSUS_NODE_TOOL")) cfg->edge_consensus_node_tool_path = p;
  }

  if (const char* v = getenv_s("AGENTD_UPLOAD_MAX_BYTES")) {
    try {
      unsigned long long n = std::stoull(v);
      const unsigned long long kMax = 512ull * 1024ull * 1024ull;
      if (n > kMax) n = kMax;
      cfg->upload_max_bytes = (size_t)n;
    } catch (...) {
    }
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_CONSOLIDATE_INTERVAL_MS")) {
    try {
      cfg->memory_consolidate_interval_ms = (int64_t)std::stoll(ms);
      if (cfg->memory_consolidate_interval_ms < 0) cfg->memory_consolidate_interval_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_CONSOLIDATE_DAILY_DAYS")) {
    try {
      cfg->memory_consolidate_daily_days = (int)std::stol(ms);
      if (cfg->memory_consolidate_daily_days < 0) cfg->memory_consolidate_daily_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_CONSOLIDATE_KEEP_CHECKPOINTS")) {
    try {
      cfg->memory_consolidate_keep_checkpoints = (int)std::stol(ms);
      if (cfg->memory_consolidate_keep_checkpoints < 1) cfg->memory_consolidate_keep_checkpoints = 1;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_DAILY_INTERVAL_MS")) {
    try {
      cfg->memory_recap_daily_interval_ms = (int64_t)std::stoll(ms);
      if (cfg->memory_recap_daily_interval_ms < 0) cfg->memory_recap_daily_interval_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_WEEKLY_INTERVAL_MS")) {
    try {
      cfg->memory_recap_weekly_interval_ms = (int64_t)std::stoll(ms);
      if (cfg->memory_recap_weekly_interval_ms < 0) cfg->memory_recap_weekly_interval_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_DAILY_DAYS")) {
    try {
      cfg->memory_recap_daily_days = (int)std::stol(ms);
      if (cfg->memory_recap_daily_days < 0) cfg->memory_recap_daily_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_WEEKLY_DAYS")) {
    try {
      cfg->memory_recap_weekly_days = (int)std::stol(ms);
      if (cfg->memory_recap_weekly_days < 0) cfg->memory_recap_weekly_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_INTERVAL_MS")) {
    try {
      cfg->memory_retention_interval_ms = (int64_t)std::stoll(ms);
      if (cfg->memory_retention_interval_ms < 0) cfg->memory_retention_interval_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_DAILY_MAX_DAYS")) {
    try {
      cfg->memory_retention_daily_max_days = (int)std::stol(ms);
      if (cfg->memory_retention_daily_max_days < 0) cfg->memory_retention_daily_max_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_DAILY_MAX_BYTES")) {
    try {
      cfg->memory_retention_daily_max_bytes = (int64_t)std::stoll(ms);
      if (cfg->memory_retention_daily_max_bytes < 0) cfg->memory_retention_daily_max_bytes = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_CHECKPOINT_MAX_DAYS")) {
    try {
      cfg->memory_retention_checkpoint_max_days = (int)std::stol(ms);
      if (cfg->memory_retention_checkpoint_max_days < 0) cfg->memory_retention_checkpoint_max_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_CHECKPOINT_MAX_COUNT")) {
    try {
      cfg->memory_retention_checkpoint_max_count = (int)std::stol(ms);
      if (cfg->memory_retention_checkpoint_max_count < 0) cfg->memory_retention_checkpoint_max_count = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_STRUCTURED_DEPRECATE_DAYS")) {
    try {
      cfg->memory_retention_structured_deprecate_days = (int)std::stol(ms);
      if (cfg->memory_retention_structured_deprecate_days < 0) cfg->memory_retention_structured_deprecate_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_STRUCTURED_DEPRECATE_MAX_ENTRIES")) {
    try {
      cfg->memory_retention_structured_deprecate_max_entries = (int)std::stol(ms);
      if (cfg->memory_retention_structured_deprecate_max_entries < 0) {
        cfg->memory_retention_structured_deprecate_max_entries = 0;
      }
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_DAILY_DAYS")) {
    try {
      cfg->memory_salience_daily_days = (int)std::stol(ms);
      if (cfg->memory_salience_daily_days < 0) cfg->memory_salience_daily_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_MAX_ITEMS")) {
    try {
      cfg->memory_salience_max_items = (int)std::stol(ms);
      if (cfg->memory_salience_max_items < 1) cfg->memory_salience_max_items = 1;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_STRUCTURED_MAX_ITEMS")) {
    try {
      cfg->memory_salience_structured_max_items = (int)std::stol(ms);
      if (cfg->memory_salience_structured_max_items < 0) cfg->memory_salience_structured_max_items = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_DAILY_MAX_ITEMS")) {
    try {
      cfg->memory_salience_daily_max_items = (int)std::stol(ms);
      if (cfg->memory_salience_daily_max_items < 0) cfg->memory_salience_daily_max_items = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_HALF_LIFE_DAYS")) {
    try {
      cfg->memory_salience_half_life_days = std::stod(ms);
      if (cfg->memory_salience_half_life_days < 0) cfg->memory_salience_half_life_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_IMPORTANCE_WEIGHT")) {
    try {
      cfg->memory_salience_importance_weight = std::stod(ms);
      if (cfg->memory_salience_importance_weight < 0) cfg->memory_salience_importance_weight = 0;
    } catch (...) {}
  }
  if (const char* s = getenv_s("AGENTD_OTA_ENABLE")) {
    cfg->ota_enable = env_truthy(s);
  }
  if (const char* s = getenv_s("AGENTD_OTA_COMMAND")) {
    cfg->ota_command = s;
  }
  if (const char* ms = getenv_s("AGENTD_OTA_COMMAND_TIMEOUT_MS")) {
    try {
      cfg->ota_command_timeout_ms = (int64_t)std::stoll(ms);
      if (cfg->ota_command_timeout_ms < 0) cfg->ota_command_timeout_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_OTA_DRAIN_TIMEOUT_MS")) {
    try {
      cfg->ota_drain_timeout_ms = (int64_t)std::stoll(ms);
      if (cfg->ota_drain_timeout_ms < 0) cfg->ota_drain_timeout_ms = 0;
    } catch (...) {}
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
  const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
  std::atomic<int64_t> last_job_gc_ms{0};

  struct PrefixRoute {
    std::string method;
    std::string prefix;
    Handler handler = nullptr;
  };

  std::map<RouteKey, Handler> routes;
  std::vector<PrefixRoute> prefix_routes;

  const ToolExtension* tool_ext_or_null() const {
    if (!options.enable_tool_extension) return nullptr;
    return &options.tool_extension;
  }

  void route(const std::string& method, const std::string& path, Handler h) {
    routes[RouteKey{method, path}] = h;
  }

  void route_prefix(const std::string& method, const std::string& prefix, Handler h) {
    prefix_routes.push_back(PrefixRoute{method, prefix, h});
  }

  void maybe_job_gc() {
    if (!cfg_store) return;
    const DaemonConfig cfg = cfg_store->snapshot();
    if (cfg.job_ttl_ms <= 0 && cfg.max_jobs == 0) return;
    const int64_t now = now_unix_ms();
    int64_t last = last_job_gc_ms.load(std::memory_order_relaxed);
    if (now - last < 2000) return;
    if (!last_job_gc_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    job_gc(cfg.job_ttl_ms, cfg.max_jobs);
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
    RuntimeConfigLoadOptions opt;
    if (!load_runtime_config_best_effort(impl_->db, &cfg, &err, opt)) {
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
    resp->body = build_health_body(self->start_time);
  });
  impl_->route("GET", "/api/v1/ready", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = build_ready_body(self->start_time, &self->db);
  });
  impl_->route("GET", "/metrics", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8";
    resp->body = build_metrics_body(self->start_time, &self->db);
  });

  impl_->route("GET", "/api/v1/config", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_config_endpoint(cur, self->cors_cfg, req, resp);
  });

  impl_->route("GET", "/api/v1/caps", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_caps_endpoint(cur, self->cors_cfg, self->start_time, req, resp);
  });

  impl_->route("POST", "/api/v1/config/update", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_config_update_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/auth/trust_roots", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_trust_roots_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/trust_roots/rotate", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_edge_auth_trust_roots_rotate_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/trust_roots/send", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_trust_roots_send_endpoint(cur, &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/auth/cert_roots", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_cert_roots_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/cert_roots/rotate", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_edge_auth_cert_roots_rotate_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/cert_roots/send", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_cert_roots_send_endpoint(cur, &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/cert_roots/verify_chain", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_cert_roots_verify_chain_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/auth/node_binding", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_node_binding_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/provision_node", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_edge_auth_provision_node_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/auth/revocations", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_revocations_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/revocations/update", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_edge_auth_revocations_update_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/auth/revocations/send", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_auth_revocations_send_endpoint(cur, &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/consensus/membership", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_consensus_membership_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/consensus/membership/rotate", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    handle_edge_consensus_membership_rotate_endpoint(self->cfg_store.get(), &self->db, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/consensus/membership/send", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_consensus_membership_send_endpoint(cur, &self->db, self->cors_cfg, req, resp);
  });

  impl_->route("GET", "/api/v1/client/prefs", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_client_prefs_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/client/prefs", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_client_prefs_post_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  impl_->route("POST", "/api/v1/ota/update", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_ota_update_endpoint(cur, self->cors_cfg, req, resp, &self->db);
  });
  impl_->route("GET", "/api/v1/ota/status", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_ota_status_endpoint(cur, self->cors_cfg, req, resp, &self->db);
  });

  impl_->route("GET", "/api/v1/diagnostics", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_diagnostics_endpoint(cur, &self->db, self->start_time, req, resp);
  });
  impl_->route("GET", "/api/v1/diagnostics/providers", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_diagnostics_providers_endpoint(cur, self->start_time, req, resp);
  });
  impl_->route("POST", "/api/v1/diagnostics/provider_test", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    cors_apply(req, resp, self->cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_diagnostics_provider_test_endpoint(cur, ocfg, &self->db, self->tool_ext_or_null(), cur.sessions_root_dir, req, resp);
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
  impl_->route("POST", "/api/v1/blob/upload", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_upload_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/blob", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/blob/meta", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_meta_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/blob/retain", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_retain_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/blob/gc", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_gc_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/blob/tier/enforce", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_tier_enforce_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/blob/archive", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_archive_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/blob/restore", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_blob_restore_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  // Memory endpoints (durable + deterministic checkpoint correlation).
  impl_->route("POST", "/api/v1/memory/consolidate", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_consolidate_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/memory/retention/enforce", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_retention_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/memory/checkpoints", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_checkpoints_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/memory/correlate", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_correlate_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/memory/correlation/index", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_correlation_index_build_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/memory/query", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_query_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/memory/index", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_index_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/memory/salience", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_memory_salience_endpoint(cur, self->cors_cfg, req, resp);
  });
  impl_->route("GET", "/api/v1/memory/recaps", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_memory_recaps_endpoint(cur, ocfg, self->cors_cfg, req, resp);
  });
  impl_->route("POST", "/api/v1/memory/recaps", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_memory_recaps_endpoint(cur, ocfg, self->cors_cfg, req, resp);
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
  impl_->route("POST", "/api/v1/session/voice_control", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_voice_control_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/voice_stats", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_voice_stats_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/session/voice_webrtc_peer", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_voice_webrtc_peer_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/session/voice_webrtc_peer", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_session_voice_webrtc_peer_status_endpoint(cur, self->cors_cfg, &self->db, req, resp);
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
  impl_->route("POST", "/api/v1/moderator/directive", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_moderator_directive_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/moderator/task", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_moderator_task_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/moderator/events", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_moderator_events_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  impl_->route("GET", "/api/v1/approvals", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_approvals_list_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route_prefix("GET", "/api/v1/approvals/", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_approvals_prefix_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route_prefix("POST", "/api/v1/approvals/", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_approvals_prefix_endpoint(cur, self->cors_cfg, &self->db, req, resp);
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
  impl_->route("GET", "/api/v1/db/blobs", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_blobs_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/blob", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_blob_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/analytics/blobs", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_blob_analytics_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/workflows", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_workflows_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/workflow", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_workflow_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/workflow_tasks", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_workflow_tasks_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/workflow_events", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_workflow_events_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/edge_workflows", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_edge_workflows_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/edge_workflow", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_edge_workflow_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/edge_workflow_steps", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_edge_workflow_steps_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/edge_workflow_events", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_edge_workflow_events_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/analytics/workflows", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_workflow_analytics_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/analytics/workflows/export", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_workflow_analytics_export_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/analytics/edge", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_edge_analytics_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/db/analytics/edge/export", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_db_edge_analytics_export_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  // Run endpoints.
  impl_->route("POST", "/api/v1/run", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_run_endpoint(cur, ocfg, self->cors_cfg, &self->db, self->tool_ext_or_null(), cur.sessions_root_dir, req, resp);
  });
  impl_->route("GET", "/api/v1/run/replay", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_run_replay_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/run/attestation", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_run_attestation_endpoint(cur, self->cors_cfg, &self->db, req, resp);
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

  // Workflow endpoints (durable graphs; execution requires the workflow engine, which is owned by AgentdService/agentd).
  impl_->route("POST", "/api/v1/workflow/submit", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_submit_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/workflow", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/workflow/stats", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_stats_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/workflows", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_list_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/workflow/cancel", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_cancel_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/workflow_schedules", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_create_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/workflow_schedules", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_list_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/workflow_schedule", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("DELETE", "/api/v1/workflow_schedule", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_delete_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/workflow_schedule/pause", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_pause_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/workflow_schedule/resume", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_resume_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/workflow_schedule/runs", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_workflow_schedule_runs_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });

  // Edge interop endpoints (UM‑EAIS / UM‑BMP transport mapping).
  impl_->route("POST", "/api/v1/edge/message", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_message_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/outbox", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_outbox_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/nodes", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_nodes_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/node", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_node_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/node/consensus_runtime", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_node_consensus_runtime_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/node/consensus_runtime", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_node_consensus_runtime_status_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/node/caps", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_node_caps_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/node/manifest_bundle", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_node_manifest_bundle_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/node/manifest_bundle/send", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_node_manifest_bundle_send_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("POST", "/api/v1/edge/task/assign", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_task_assign_endpoint(cur, self->cors_cfg, &self->db, req, resp);
  });
  impl_->route("GET", "/api/v1/edge/task", +[](void* ctx, const HttpRequest& req, HttpResponse* resp) {
    auto* self = static_cast<Impl*>(ctx);
    const DaemonConfig cur = self->cfg_store->snapshot();
    handle_edge_task_get_endpoint(cur, self->cors_cfg, &self->db, req, resp);
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
    resp->body = json_error_body("agentd api not initialized");
    return true;
  }

  impl_->maybe_job_gc();

  // Minimal OPTIONS behavior (used by HTTP transport CORS preflight).
  if (req.method == "OPTIONS") {
    resp->status = 204;
    resp->body.clear();
    cors_apply(req, resp, impl_->cors_cfg);
    return true;
  }

  const auto it = impl_->routes.find(RouteKey{req.method, req.path});
  if (it != impl_->routes.end()) {
    it->second(impl_, req, resp);
    return true;
  }

  const Handler* prefix_handler = nullptr;
  size_t prefix_len = 0;
  for (const auto& entry : impl_->prefix_routes) {
    if (entry.method != req.method) continue;
    if (req.path.rfind(entry.prefix, 0) != 0) continue;
    if (entry.prefix.size() > prefix_len) {
      prefix_handler = &entry.handler;
      prefix_len = entry.prefix.size();
    }
  }
  if (prefix_handler && *prefix_handler) {
    (*prefix_handler)(impl_, req, resp);
    return true;
  }

  resp->status = 404;
  resp->body = json_error_body("not found");
  cors_apply(req, resp, impl_->cors_cfg);
  return false;
}

std::string AgentdApi::db_path() const { return impl_ ? impl_->options.cfg.db_path : ""; }
std::string AgentdApi::listen_host() const { return impl_ ? impl_->options.cfg.listen_host : ""; }
uint16_t AgentdApi::listen_port() const { return impl_ ? impl_->options.cfg.listen_port : 0; }

}  // namespace agentd
