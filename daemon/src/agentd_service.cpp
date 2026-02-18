#include "agentd/service.h"

#include "http_server.h"

#include "agent_db.h"
#include "avm_endpoints.h"
#include "blob_endpoints.h"
#include "caps_endpoint.h"
#include "config_endpoint.h"
#include "config_store.h"
#include "cors.h"
#include "daemon_auth.h"
#include "diagnostics_endpoints.h"
#include "db_query_endpoints.h"
#include "edge_interop_endpoints.h"
#include "edge_deadline_sweeper.h"
#include "edge_workflow_engine.h"
#include "file_endpoint.h"
#include "job_endpoints.h"
#include "job_engine.h"
#include "job_manager.h"
#include "job_stream_endpoint.h"
#include "health_endpoint.h"
#include "memory_endpoints.h"
#include "memory_consolidator.h"
#include "orchestrate_endpoints.h"
#include "ota_endpoints.h"
#include "openrouter_models_endpoint.h"
#include "openrouter_util.h"
#include "provider_util.h"
#include "run_endpoints.h"
#include "run_replay_endpoint.h"
#include "trace_endpoints.h"
#include "workflow_endpoints.h"
#include "workflow_engine.h"
#include "workflow_stream_endpoint.h"
#include "runtime_config.h"
#include "sandbox_policy.h"
#include "secrets_file.h"
#include "session_endpoints.h"
#include "string_util.h"
#include "tools_endpoint.h"

#include "openai_client.h"

#include <chrono>
#include <cctype>
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

static bool env_truthy(const char* s) {
  if (!s) return false;
  std::string v = s;
  for (char& c : v) c = (char)std::tolower((unsigned char)c);
  return v == "1" || v == "true" || v == "yes" || v == "on";
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

  // base_url
  if (cfg->base_url.empty()) {
    if (const char* b = getenv_s("OPENAI_API_BASE")) cfg->base_url = b;
    else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) cfg->base_url = b2;
    else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) cfg->base_url = b3;
    else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) cfg->base_url = b4;
    else if (const char* b5 = getenv_s("MOONSHOT_API_BASE")) cfg->base_url = b5;
  }

  // api_key selection: choose a key that matches base_url best-effort.
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
  if (cfg->auth_cookie_name.empty()) {
    if (const char* t = getenv_s("AGENTD_AUTH_COOKIE")) cfg->auth_cookie_name = t;
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

  if (const char* v = getenv_s("AGENTD_UPLOAD_MAX_BYTES")) {
    try {
      unsigned long long n = std::stoull(v);
      const unsigned long long kMax = 512ull * 1024ull * 1024ull;
      if (n > kMax) n = kMax;
      cfg->upload_max_bytes = (size_t)n;
    } catch (...) {
    }
  }

  if (const char* ms = getenv_s("AGENTD_MAX_STEPS_DEFAULT")) {
    try { cfg->max_steps_default = (size_t)std::stoull(ms); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_CALLS_TOTAL_DEFAULT")) {
    try { cfg->max_tool_calls_total_default = (size_t)std::stoull(ms); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_CALLS_PER_TOOL_DEFAULT")) {
    try { cfg->max_tool_calls_per_tool_default = (size_t)std::stoull(ms); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_CALL_ARGS_CHARS_DEFAULT")) {
    try { cfg->max_tool_call_args_chars_default = (size_t)std::stoull(ms); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_JOB_CONCURRENCY")) {
    try { cfg->job_engine_max_concurrency = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_JOB_POLL_MS")) {
    try { cfg->job_engine_poll_ms = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_CONCURRENCY")) {
    try { cfg->workflow_engine_max_concurrency = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_POLL_MS")) {
    try { cfg->workflow_engine_poll_ms = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_MAX_INFLIGHT_PER_WORKFLOW")) {
    try { cfg->workflow_engine_max_inflight_per_workflow = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_MAX_INFLIGHT_PER_SESSION")) {
    try { cfg->workflow_engine_max_inflight_per_session = std::max(0, std::stoi(ms)); } catch (...) {}
  }
  if (cfg->workflow_engine_fair_queue_policy.empty()) {
    if (const char* ms = getenv_s("AGENTD_WORKFLOW_FAIR_QUEUE_POLICY")) cfg->workflow_engine_fair_queue_policy = ms;
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_FAIR_QUEUE_MAX_SESSION_WEIGHT")) {
    try { cfg->workflow_engine_fair_queue_max_session_weight = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_FAIR_QUEUE_MAX_SCHEDULE_LEN")) {
    try { cfg->workflow_engine_fair_queue_max_schedule_len = std::max(16, std::stoi(ms)); } catch (...) {}
  }
  if (cfg->workflow_engine_drr_cost_model.empty()) {
    if (const char* ms = getenv_s("AGENTD_WORKFLOW_DRR_COST_MODEL")) cfg->workflow_engine_drr_cost_model = ms;
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
    // Make db_path absolute for clearer /api/v1/config output.
    std::error_code ec;
    const std::filesystem::path p(cfg->db_path);
    if (p.is_relative()) {
      cfg->db_path = (std::filesystem::path(cfg->state_dir) / p).lexically_normal().string();
    }
    (void)ec;
  }
}

}  // namespace

struct AgentdService::Impl {
  explicit Impl(Options options) : options(std::move(options)) {}

  Options options;
  CorsConfig cors_cfg{};
  AgentDb db;
  std::unique_ptr<DaemonConfigStore> cfg_store;
  std::unique_ptr<JobEngine> job_engine;
  std::unique_ptr<WorkflowEngine> wf_engine;
  std::unique_ptr<MemoryConsolidatorEngine> mem_engine;
  std::unique_ptr<EdgeDeadlineSweeperEngine> edge_deadline_engine;
  std::unique_ptr<EdgeWorkflowEngine> edge_wf_engine;
  HttpServer server;
  const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

  std::thread server_thread;
  bool initialized = false;
  std::string init_error;

  const ToolExtension* tool_ext_or_null() const {
    if (!options.enable_tool_extension) return nullptr;
    return &options.tool_extension;
  }

  OpenAIClientConfig ocfg_from_cfg(const DaemonConfig& c) const {
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

  bool init(std::string* out_error) {
    if (out_error) out_error->clear();
    if (initialized) return true;

    // Avoid daemon termination on closed sockets.
    (void)::signal(SIGPIPE, SIG_IGN);

    DaemonConfig cfg = options.cfg;
    fill_env_defaults(&cfg);
    fill_path_defaults(&cfg);
    cors_cfg = cors_cfg_from_config(cfg);

    // Open DB.
    {
      std::string db_err;
      if (!db.open(cfg.db_path, &db_err)) {
        init_error = std::string("failed to open agentd DB: ") + db_err;
        if (out_error) *out_error = init_error;
        return false;
      }
    }

    // Load runtime-configured daemon defaults from the DB (keeps all daemon state in agentd.db).
    {
      std::string err;
      RuntimeConfigLoadOptions opt;
      if (!load_runtime_config_best_effort(db, &cfg, &err, opt)) {
        std::cerr << "Warning: failed to load runtime config from DB: " << err << "\n";
      }
    }

    // Job durability: recover inflight jobs to queued so they can resume after restart.
    {
      const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
      std::string err;
      if (!db.recover_inflight_jobs_resumable(now_ms, &err)) {
        std::cerr << "Warning: failed to recover inflight jobs: " << err << "\n";
      }
    }

    // Store final cfg back into options and create config store.
    options.cfg = cfg;
    cfg_store = std::make_unique<DaemonConfigStore>(cfg);

    // Server headers + OPTIONS handler (CORS preflight).
    server.set_default_headers({
      {"Server", "agentd/0.1"},
    });
    server.set_options_handler([this](const HttpRequest& req, HttpResponse* resp) {
      resp->status = 204;
      resp->body.clear();
      cors_apply(req, resp, cors_cfg);
    });

    // Background GC for finished jobs.
    {
      const DaemonConfig cfg0 = cfg_store->snapshot();
      if (cfg0.job_ttl_ms > 0 || cfg0.max_jobs > 0) {
        const int64_t ttl_ms = cfg0.job_ttl_ms;
        const size_t max_jobs = cfg0.max_jobs;
        std::thread([ttl_ms, max_jobs]() {
          for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            job_gc(ttl_ms, max_jobs);
          }
        }).detach();
      }
    }

    // Resumable async job scheduler (background).
    if (!job_engine) {
      const DaemonConfig cfg0 = cfg_store->snapshot();
      JobEngine::Options opt;
      opt.max_concurrency = std::max(1, cfg0.job_engine_max_concurrency);
      opt.poll_ms = std::max(1, cfg0.job_engine_poll_ms);
      job_engine = std::make_unique<JobEngine>(
        &db,
        [this]() { return cfg_store->snapshot(); },
        [this](const DaemonConfig& c) { return ocfg_from_cfg(c); },
        tool_ext_or_null(),
        cfg0.sessions_root_dir,
        opt
      );
      std::string jerr;
      if (!job_engine->start(&jerr)) {
        std::cerr << "Warning: failed to start job engine: " << jerr << "\n";
      }
    }

    // Durable workflow scheduler (background).
    if (!wf_engine) {
      const DaemonConfig cfg0 = cfg_store->snapshot();
      WorkflowEngine::Options opt;
      opt.max_concurrency = std::max(1, cfg0.workflow_engine_max_concurrency);
      opt.poll_ms = std::max(1, cfg0.workflow_engine_poll_ms);
      opt.max_inflight_per_workflow = std::max(1, cfg0.workflow_engine_max_inflight_per_workflow);
      opt.max_inflight_per_session = std::max(0, cfg0.workflow_engine_max_inflight_per_session);
      opt.fair_queue_policy = cfg0.workflow_engine_fair_queue_policy;
      opt.fair_queue_max_session_weight = std::max(1, cfg0.workflow_engine_fair_queue_max_session_weight);
      opt.fair_queue_max_schedule_len = std::max(16, cfg0.workflow_engine_fair_queue_max_schedule_len);
      opt.drr_cost_model = cfg0.workflow_engine_drr_cost_model;
      wf_engine = std::make_unique<WorkflowEngine>(
        &db,
        [this]() { return cfg_store->snapshot(); },
        [this](const DaemonConfig& c) { return ocfg_from_cfg(c); },
        tool_ext_or_null(),
        cfg0.sessions_root_dir,
        opt
      );
      std::string werr;
      if (!wf_engine->start(&werr)) {
        std::cerr << "Warning: failed to start workflow engine: " << werr << "\n";
      }
    }

    // Memory consolidation (background; disabled by default).
    if (!mem_engine) {
      mem_engine = std::make_unique<MemoryConsolidatorEngine>(
        [this]() { return cfg_store->snapshot(); },
        MemoryConsolidatorEngine::Options{}
      );
      std::string merr;
      if (!mem_engine->start(&merr)) {
        std::cerr << "Warning: failed to start memory consolidator engine: " << merr << "\n";
      }
    }

    // Edge task deadline sweeper (UM‑EEM deadlines/timeouts; platform-side best-effort).
    if (!edge_deadline_engine) {
      EdgeDeadlineSweeperEngine::Options opt;
      opt.poll_ms = 500;
      opt.max_scan_rows = 128;
      edge_deadline_engine = std::make_unique<EdgeDeadlineSweeperEngine>(
        &db,
        [this]() { return cfg_store->snapshot(); },
        opt
      );
      std::string derr;
      if (!edge_deadline_engine->start(&derr)) {
        std::cerr << "Warning: failed to start edge deadline sweeper: " << derr << "\n";
      }
    }

    // Durable edge workflow runner (UM‑WF executed over UM‑BMP TASK_ASSIGN).
    if (!edge_wf_engine) {
      EdgeWorkflowEngine::Options opt;
      opt.poll_ms = 200;
      opt.max_scan_workflows = 64;
      opt.max_dispatch_per_tick = 64;
      edge_wf_engine = std::make_unique<EdgeWorkflowEngine>(
        &db,
        [this]() { return cfg_store->snapshot(); },
        opt
      );
      std::string werr;
      if (!edge_wf_engine->start(&werr)) {
        std::cerr << "Warning: failed to start edge workflow engine: " << werr << "\n";
      }
    }

    // Core routes.
    server.handle("GET", "/api/v1/health", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = build_health_body(start_time);
    });
    server.handle("GET", "/api/v1/ready", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      resp->body = build_ready_body(start_time, &db);
    });
    server.handle("GET", "/metrics", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8";
      resp->body = build_metrics_body(start_time, &db);
    });

    server.handle("GET", "/api/v1/config", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_config_endpoint(cur, cors_cfg, req, resp);
    });

    server.handle("GET", "/api/v1/caps", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_caps_endpoint(cur, cors_cfg, start_time, req, resp);
    });

    server.handle("POST", "/api/v1/config/update", [this](const HttpRequest& req, HttpResponse* resp) {
      handle_config_update_endpoint(cfg_store.get(), &db, cors_cfg, req, resp);
    });

    server.handle("POST", "/api/v1/ota/update", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_ota_update_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("GET", "/api/v1/ota/status", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_ota_status_endpoint(cur, cors_cfg, req, resp);
    });

    server.handle("GET", "/api/v1/diagnostics", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      const DaemonConfig cur = cfg_store->snapshot();
      handle_diagnostics_endpoint(cur, &db, start_time, req, resp);
    });
    server.handle("GET", "/api/v1/diagnostics/providers", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      const DaemonConfig cur = cfg_store->snapshot();
      handle_diagnostics_providers_endpoint(cur, start_time, req, resp);
    });
    server.handle("POST", "/api/v1/diagnostics/provider_test", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      const DaemonConfig cur = cfg_store->snapshot();
      const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
      handle_diagnostics_provider_test_endpoint(cur, ocfg, &db, tool_ext_or_null(), cur.sessions_root_dir, req, resp);
    });

    server.handle("GET", "/api/v1/tools", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_tools_endpoint(cur, cors_cfg, cur.sessions_root_dir, tool_ext_or_null(), req, resp);
    });

    server.handle("POST", "/api/v1/avm/job_scan", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_avm_job_scan_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("POST", "/api/v1/avm/policy_scan", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_avm_policy_scan_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("POST", "/api/v1/avm/inspect", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_avm_inspect_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("POST", "/api/v1/avm/verify_strict", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_avm_verify_strict_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("POST", "/api/v1/avm/trace_hash", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_avm_trace_hash_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("POST", "/api/v1/avm/capsule_run", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_avm_capsule_run_endpoint(cur, cors_cfg, req, resp);
    });

    server.handle("GET", "/api/v1/openrouter/models", [this](const HttpRequest& req, HttpResponse* resp) {
      cors_apply(req, resp, cors_cfg);
      resp->headers["Content-Type"] = "application/json; charset=utf-8";
      const DaemonConfig cur = cfg_store->snapshot();
      if (!daemon_require_auth(cur, req, resp)) return;
      const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
      handle_openrouter_models_endpoint(ocfg, !cur.auth_token.empty(), req, resp);
      return;
    });

    server.handle("GET", "/api/v1/file", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_file_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("POST", "/api/v1/blob/upload", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_blob_upload_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/blob", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_blob_get_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/blob/meta", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_blob_meta_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/blob/retain", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_blob_retain_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/blob/gc", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_blob_gc_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/blob/tier/enforce", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_blob_tier_enforce_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("POST", "/api/v1/memory/consolidate", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_memory_consolidate_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("GET", "/api/v1/memory/checkpoints", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_memory_checkpoints_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("GET", "/api/v1/memory/correlate", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_memory_correlate_endpoint(cur, cors_cfg, req, resp);
    });
    server.handle("GET", "/api/v1/memory/query", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_memory_query_endpoint(cur, cors_cfg, req, resp);
    });

    server.handle("GET", "/api/v1/sessions", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_sessions_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("POST", "/api/v1/session/new", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_new_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("GET", "/api/v1/session", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_get_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("GET", "/api/v1/session/audit", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_audit_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("GET", "/api/v1/session/client_events", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_client_events_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("GET", "/api/v1/session/clients", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_clients_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("GET", "/api/v1/session/artifacts", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_artifacts_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("GET", "/api/v1/session/scene", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_scene_get_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("POST", "/api/v1/session/scene/apply", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_scene_apply_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("POST", "/api/v1/session/ui_event", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_ui_event_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("POST", "/api/v1/session/client_event", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_ui_event_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("POST", "/api/v1/session/upload", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_upload_endpoint(cur, cors_cfg, &db, req, resp);
    });

    server.handle("DELETE", "/api/v1/session", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_session_delete_endpoint(cur, cors_cfg, &db, cur.sessions_root_dir, req, resp);
    });

    // DB query endpoints.
    server.handle("GET", "/api/v1/db/runs", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_runs_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/run", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_run_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/artifacts", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_artifacts_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/ui_actions", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_ui_actions_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/sessions", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_sessions_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/messages", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_messages_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/client_events", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_client_events_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/workflows", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_workflows_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/workflow", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_workflow_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/workflow_tasks", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_workflow_tasks_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/workflow_events", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_workflow_events_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/edge_workflows", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_edge_workflows_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/edge_workflow", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_edge_workflow_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/edge_workflow_steps", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_edge_workflow_steps_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/edge_workflow_events", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_edge_workflow_events_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/analytics/workflows", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_workflow_analytics_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/analytics/workflows/export", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_workflow_analytics_export_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/analytics/edge", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_edge_analytics_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/db/analytics/edge/export", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_db_edge_analytics_export_endpoint(cur, cors_cfg, &db, req, resp);
    });

    // Run endpoints (sync + async).
    server.handle("POST", "/api/v1/run", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
      handle_run_endpoint(cur, ocfg, cors_cfg, &db, tool_ext_or_null(), cur.sessions_root_dir, req, resp);
    });
    server.handle("GET", "/api/v1/run/replay", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_run_replay_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/run_async", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
      handle_run_async_endpoint(cur, ocfg, cors_cfg, &db, tool_ext_or_null(), cur.sessions_root_dir, req, resp);
    });
    server.handle("POST", "/api/v1/orchestrate", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
      handle_orchestrate_endpoint(cur, ocfg, cors_cfg, &db, tool_ext_or_null(), cur.sessions_root_dir, req, resp);
    });

    server.handle("GET", "/api/v1/trace", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_trace_lookup_endpoint(cur, cors_cfg, &db, req, resp);
    });

    // Workflow endpoints.
    server.handle("POST", "/api/v1/workflow/submit", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_submit_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/workflow", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_get_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/workflow/stats", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_stats_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/workflows", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_list_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/workflow/cancel", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_cancel_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/workflow/events", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_events_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle_stream("GET", "/api/v1/workflow/stream", [this](const HttpRequest& req, socket_t client_fd) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_workflow_stream_endpoint(cur.auth_token, cors_cfg, &db, req, client_fd);
    });

    // Edge interop endpoints (UM‑EAIS / UM‑BMP transport mapping).
    server.handle("POST", "/api/v1/edge/message", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_message_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/outbox", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_outbox_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/nodes", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_nodes_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/node", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_node_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/node/caps", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_node_caps_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/edge/task/assign", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_task_assign_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/task", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_task_get_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/edge/rule/upsert", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_rule_upsert_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/rules", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_rules_list_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("DELETE", "/api/v1/edge/rule", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_rule_delete_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/edge/workflow/submit", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_workflow_submit_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/edge/workflow/cancel", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_workflow_cancel_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/workflow", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_workflow_get_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("GET", "/api/v1/edge/workflow/events", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_workflow_events_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle_stream("GET", "/api/v1/edge/workflow/stream", [this](const HttpRequest& req, socket_t client_fd) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_workflow_stream_endpoint(cur.auth_token, cors_cfg, &db, req, client_fd);
    });
    server.handle("GET", "/api/v1/edge/workflows", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_edge_workflow_list_endpoint(cur, cors_cfg, &db, req, resp);
    });

    // Job endpoints.
    server.handle("GET", "/api/v1/job", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_job_get_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle("POST", "/api/v1/job/cancel", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_job_cancel_endpoint(cur, cors_cfg, &db, req, resp);
    });
    server.handle_stream("GET", "/api/v1/job/stream", [this](const HttpRequest& req, socket_t client_fd) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_job_stream_endpoint(cur.auth_token, cors_cfg, &db, req, client_fd);
    });
    server.handle("DELETE", "/api/v1/job", [this](const HttpRequest& req, HttpResponse* resp) {
      const DaemonConfig cur = cfg_store->snapshot();
      handle_job_delete_endpoint(cur, cors_cfg, &db, req, resp);
    });

    initialized = true;
    return true;
  }

  bool serve_blocking(std::string* out_error) {
    if (out_error) out_error->clear();
    if (!init(out_error)) return false;

    std::string err;
    const DaemonConfig cfg_final = cfg_store->snapshot();
    if (!host_is_loopback(cfg_final.listen_host) && cfg_final.auth_token.empty() && !cfg_final.allow_unauthenticated_non_loopback) {
      if (out_error) {
        *out_error = "refusing to bind to non-loopback host without auth";
      }
      return false;
    }
    std::cerr << "agentd listening on http://" << cfg_final.listen_host << ":" << cfg_final.listen_port << "\n";
    if (!server.serve(cfg_final.listen_host, cfg_final.listen_port, &err)) {
      if (out_error) *out_error = err;
      return false;
    }
    return true;
  }

  bool start_background(std::string* out_error) {
    if (out_error) out_error->clear();
    if (server_thread.joinable()) return true;
    std::string init_err;
    if (!init(&init_err)) {
      if (out_error) *out_error = init_err;
      return false;
    }
    server_thread = std::thread([this]() {
      std::string err;
      (void)serve_blocking(&err);
    });
    return true;
  }

  void stop() {
    if (job_engine) job_engine->stop();
    if (wf_engine) wf_engine->stop();
    if (mem_engine) mem_engine->stop();
    if (edge_deadline_engine) edge_deadline_engine->stop();
    if (edge_wf_engine) edge_wf_engine->stop();
    server.stop();
    if (server_thread.joinable()) {
      server_thread.join();
    }
  }
};

AgentdService::AgentdService(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
AgentdService::~AgentdService() = default;

bool AgentdService::init(std::string* out_error) { return impl_->init(out_error); }
bool AgentdService::serve_blocking(std::string* out_error) { return impl_->serve_blocking(out_error); }
bool AgentdService::start_background(std::string* out_error) { return impl_->start_background(out_error); }
void AgentdService::stop() { impl_->stop(); }

std::string AgentdService::listen_host() const { return impl_->options.cfg.listen_host; }
uint16_t AgentdService::listen_port() const { return impl_->options.cfg.listen_port; }
std::string AgentdService::db_path() const { return impl_->options.cfg.db_path; }

}  // namespace agentd
