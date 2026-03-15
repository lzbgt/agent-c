#include "http_server.h"

#include "cors.h"
#include "daemon_auth.h"
#include "diagnostics_endpoints.h"
#include "daemon_cli.h"
#include "daemon_config.h"
#include "config_endpoint.h"
#include "caps_endpoint.h"
#include "client_prefs_endpoints.h"
#include "approval_queue_endpoints.h"
#include "avm_endpoints.h"
#include "blob_endpoints.h"
#include "file_endpoint.h"
#include "http_util.h"
#include "sandbox_policy.h"
#include "string_util.h"
#include "openrouter_models_endpoint.h"
#include "job_stream_endpoint.h"
#include "tools_endpoint.h"
#include "sandbox_endpoints.h"
#include "tool_plugins.h"
#include "tool_servers.h"
#include "tool_extension_mux.h"
#include "session_endpoints.h"
#include "session_voice_runtime.h"
#include "moderator_endpoints.h"
#include "job_endpoints.h"
#include "orchestrate_endpoints.h"
#include "memory_endpoints.h"
#include "memory_consolidator.h"
#include "memory_recaps.h"
#include "memory_retention.h"
#include "ota_endpoints.h"
#include "run_endpoints.h"
#include "run_replay_endpoint.h"
#include "trace_endpoints.h"
#include "health_endpoint.h"
#include "workflow_endpoints.h"
#include "workflow_engine.h"
#include "workflow_schedule_endpoints.h"
#include "workflow_schedule_engine.h"
#include "workflow_stream_endpoint.h"
#include "db_query_endpoints.h"
#include "edge_interop_endpoints.h"
#include "edge_runtime_endpoints.h"
#include "edge_deadline_sweeper.h"
#include "edge_workflow_engine.h"
#include "secrets_file.h"
#include "config_store.h"
#include "runtime_config.h"
#include "provider_util.h"

#include "agent_db.h"

#include "openai_client.h"

#include "job_manager.h"
#include "job_engine.h"
#include "openrouter_util.h"

#include <json/json.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <cctype>
#include <signal.h>
#include <memory>
#include <cstring>
#if !defined(_WIN32)
#include <pwd.h>
#include <unistd.h>
#endif

using namespace agentd;

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
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
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

int main(int argc, char** argv) {
#if !defined(_WIN32)
  // On macOS (and many POSIX systems), writing to a closed socket can raise SIGPIPE,
  // which terminates the process by default. Our HTTP server uses send/write(),
  // so we must ignore SIGPIPE to avoid daemon exits that look like "hangs" to clients.
  (void)::signal(SIGPIPE, SIG_IGN);
#endif
  {
    std::string net_err;
    if (!net_init(&net_err)) {
      std::cerr << "Network init failed: " << net_err << "\n";
      return 2;
    }
  }

  DaemonConfig cfg;
  DaemonCliOverrides cli;
  const int cli_rc = parse_daemon_cli(argc, argv, &cfg, &cli);
  if (cli_rc != 0) return cli_rc;
  if (cli.help_requested) return 0;

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

  // Fill from env only when not provided by flags.
  // Important: pick the API key that matches the configured base URL.
  // Otherwise a host environment that exports multiple keys can accidentally send the wrong key to the wrong provider.
  if (cfg.base_url.empty()) {
    if (const char* b = getenv_s("OPENAI_API_BASE")) {
      cfg.base_url = b;
    } else if (const char* b2 = getenv_s("OPENAI_BASE_URL")) {
      cfg.base_url = b2;
    } else if (const char* b3 = getenv_s("OPENROUTER_API_BASE")) {
      cfg.base_url = b3;
    } else if (const char* b4 = getenv_s("DEEPSEEK_API_BASE")) {
      cfg.base_url = b4;
    } else if (const char* b5 = getenv_s("MOONSHOT_API_BASE")) {
      cfg.base_url = b5;
    }
  }
  if (cfg.api_key.empty()) {
    if (url_contains_ci(cfg.base_url, "deepseek")) {
      if (const char* k = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k3;
    } else if (url_contains_ci(cfg.base_url, "openrouter")) {
      if (const char* k = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENAI_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k3;
    } else if (url_contains_ci(cfg.base_url, "moonshot")) {
      // Moonshot/Kimi: prefer CN key when present.
      if (const char* k = getenv_s("KIMI_API_KEY_CN")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("MOONSHOT_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("OPENAI_API_KEY")) cfg.api_key = k3;
    } else {
      if (const char* k = getenv_s("OPENAI_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k3;
    }
  }
  if (cfg.api_key.empty()) {
    // Best-effort local secret file discovery to keep provider keys out of browser storage.
    // Preferred: .not_in_repo; fallback: project.local.md
    const std::string provider = provider_from_base_url(cfg.base_url);
    if (auto k = load_provider_key_best_effort(provider)) {
      cfg.api_key = *k;
    }
  }

  if (const char* ms = getenv_s("AGENTD_MAX_STEPS_DEFAULT")) {
    try {
      cfg.max_steps_default = (size_t)std::stoull(ms);
    } catch (...) {
      std::cerr << "Invalid AGENTD_MAX_STEPS_DEFAULT; ignoring\n";
    }
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_CALLS_TOTAL_DEFAULT")) {
    try {
      cfg.max_tool_calls_total_default = (size_t)std::stoull(ms);
    } catch (...) {
      std::cerr << "Invalid AGENTD_MAX_TOOL_CALLS_TOTAL_DEFAULT; ignoring\n";
    }
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_CALLS_PER_TOOL_DEFAULT")) {
    try {
      cfg.max_tool_calls_per_tool_default = (size_t)std::stoull(ms);
    } catch (...) {
      std::cerr << "Invalid AGENTD_MAX_TOOL_CALLS_PER_TOOL_DEFAULT; ignoring\n";
    }
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_CALL_ARGS_CHARS_DEFAULT")) {
    try {
      cfg.max_tool_call_args_chars_default = (size_t)std::stoull(ms);
    } catch (...) {
      std::cerr << "Invalid AGENTD_MAX_TOOL_CALL_ARGS_CHARS_DEFAULT; ignoring\n";
    }
  }
  if (const char* ms = getenv_s("AGENTD_MAX_TOOL_RESULT_CHARS_DEFAULT")) {
    try {
      cfg.max_tool_result_chars_default = (size_t)std::stoull(ms);
    } catch (...) {
      std::cerr << "Invalid AGENTD_MAX_TOOL_RESULT_CHARS_DEFAULT; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_TOOL_CALL_LIMITS_DEFAULT")) {
    std::vector<std::pair<std::string, size_t>> parsed;
    std::string perr;
    if (parse_tool_call_limits_csv(s, &parsed, &perr)) {
      cfg.tool_call_limits_default = std::move(parsed);
    } else {
      std::cerr << "Invalid AGENTD_TOOL_CALL_LIMITS_DEFAULT; ignoring: " << perr << "\n";
    }
  }
  if (cfg.model.empty()) {
    if (const char* m = getenv_s("AGENT_MODEL")) {
      cfg.model = m;
    }
  }
  if (!cli.system_profile_set) {
    if (const char* p = getenv_s("AGENTD_SYSTEM_PROFILE")) {
      const std::string s = trim_copy(p);
      if (s == "default" || s == "jules_codex") cfg.system_profile = s;
    }
  }
  if (cfg.auth_token.empty()) {
    if (const char* t = getenv_s("AGENTD_AUTH_TOKEN")) {
      cfg.auth_token = t;
    }
  }
  if (cfg.run_attest_hmac_kid.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_HMAC_KID")) {
      cfg.run_attest_hmac_kid = t;
    }
  }
  if (cfg.run_attest_hmac_key.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_HMAC_KEY")) {
      cfg.run_attest_hmac_key = t;
    }
  }
  if (cfg.run_attest_ed25519_kid.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_ED25519_KID")) {
      cfg.run_attest_ed25519_kid = t;
    }
  }
  if (cfg.run_attest_ed25519_seed.empty()) {
    if (const char* t = getenv_s("AGENTD_RUN_ATTEST_ED25519_SEED")) {
      cfg.run_attest_ed25519_seed = t;
    }
  }
  if (cfg.auth_cookie_name.empty()) {
    if (const char* t = getenv_s("AGENTD_AUTH_COOKIE")) {
      cfg.auth_cookie_name = t;
    }
  }
  if (!cfg.cors_origins_set) {
    if (const char* s = getenv_s("AGENTD_CORS_ORIGINS")) {
      std::vector<std::string> toks;
      parse_csv_tokens_best_effort(s, &toks);
      if (!toks.empty()) {
        cfg.cors_origins_set = true;
        cfg.cors_origins = std::move(toks);
      }
    }
  }
  if (cfg.cors_allow_headers.empty()) {
    if (const char* s = getenv_s("AGENTD_CORS_ALLOW_HEADERS")) {
      cfg.cors_allow_headers = s;
    }
  }
  if (cfg.cors_allow_methods.empty()) {
    if (const char* s = getenv_s("AGENTD_CORS_ALLOW_METHODS")) {
      cfg.cors_allow_methods = s;
    }
  }
  if (!cli.cors_allow_credentials_set) {
    if (const char* s = getenv_s("AGENTD_CORS_ALLOW_CREDENTIALS")) {
      cfg.cors_allow_credentials = env_truthy(s);
    }
  }
  if (!cli.cors_max_age_set) {
    if (const char* s = getenv_s("AGENTD_CORS_MAX_AGE_SECONDS")) {
      try {
        cfg.cors_max_age_seconds = std::max(0, std::stoi(s));
      } catch (...) {
        std::cerr << "Invalid AGENTD_CORS_MAX_AGE_SECONDS; ignoring\n";
      }
    }
  }
  if (!cli.cors_routes_set) {
    if (const char* s = getenv_s("AGENTD_CORS_ROUTES")) {
      Json::CharReaderBuilder b;
      b["collectComments"] = false;
      std::string errs;
      Json::Value root;
      const auto* begin = s;
      const auto* end = s + std::strlen(s);
      std::unique_ptr<Json::CharReader> reader(b.newCharReader());
      if (!reader->parse(begin, end, &root, &errs)) {
        std::cerr << "Invalid AGENTD_CORS_ROUTES JSON; ignoring: " << errs << "\n";
      } else if (!root.isArray()) {
        std::cerr << "Invalid AGENTD_CORS_ROUTES JSON; expected array\n";
      } else {
        for (const auto& v : root) {
          CorsRouteConfig route;
          std::string perr;
          if (!parse_cors_route_value(v, &route, &perr)) {
            std::cerr << "Invalid AGENTD_CORS_ROUTES entry; ignoring: " << perr << "\n";
            continue;
          }
          cfg.cors_routes.push_back(std::move(route));
        }
      }
    }
  }
  if (cfg.db_path.empty()) {
    if (const char* p = getenv_s("AGENTD_DB_PATH")) {
      cfg.db_path = p;
    }
  }
  if (!cli.upload_max_bytes_set) {
    if (const char* v = getenv_s("AGENTD_UPLOAD_MAX_BYTES")) {
      try {
        unsigned long long n = std::stoull(v);
        const unsigned long long kMax = 512ull * 1024ull * 1024ull;
        if (n > kMax) n = kMax;
        cfg.upload_max_bytes = (size_t)n;
        cli.upload_max_bytes_set = true;
      } catch (...) {
        std::cerr << "Invalid AGENTD_UPLOAD_MAX_BYTES; ignoring\n";
      }
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_MODE")) {
    const std::string v = trim_copy(s);
    if (v == "local" || v == "object") {
      cfg.blob_store_mode = v;
      cli.blob_store_set = true;
    } else {
      std::cerr << "Invalid AGENTD_BLOB_STORE_MODE; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_ENDPOINT")) {
    cfg.blob_store_endpoint = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_REGION")) {
    cfg.blob_store_region = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_BUCKET")) {
    cfg.blob_store_bucket = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_PREFIX")) {
    cfg.blob_store_prefix = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_PATH_STYLE")) {
    cfg.blob_store_path_style = env_truthy(s);
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_READ_MODE")) {
    const std::string v = trim_copy(s);
    if (v == "redirect" || v == "proxy") {
      cfg.blob_store_read_mode = v;
      cli.blob_store_set = true;
    } else {
      std::cerr << "Invalid AGENTD_BLOB_STORE_READ_MODE; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_CACHE_MODE")) {
    const std::string v = trim_copy(s);
    if (v == "none" || v == "read-through") {
      cfg.blob_store_cache_mode = v;
      cli.blob_store_set = true;
    } else {
      std::cerr << "Invalid AGENTD_BLOB_STORE_CACHE_MODE; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_CACHE_MAX_BYTES")) {
    try {
      unsigned long long n = std::stoull(s);
      const unsigned long long kMax = 512ull * 1024ull * 1024ull;
      if (n > kMax) n = kMax;
      cfg.blob_store_cache_max_bytes = (size_t)n;
      cli.blob_store_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_STORE_CACHE_MAX_BYTES; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_PRESIGN_TTL_SEC")) {
    try {
      long long n = std::stoll(s);
      if (n < 1) n = 1;
      if (n > 604800) n = 604800;
      cfg.blob_store_presign_ttl_sec = (int64_t)n;
      cli.blob_store_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_STORE_PRESIGN_TTL_SEC; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_TIMEOUT_MS")) {
    try {
      long long n = std::stoll(s);
      if (n < 0) n = 0;
      if (n > 30LL * 60 * 1000) n = 30LL * 60 * 1000;
      cfg.blob_store_timeout_ms = (int64_t)n;
      cli.blob_store_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_STORE_TIMEOUT_MS; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_ACCESS_KEY")) {
    cfg.blob_store_access_key = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_SECRET_KEY")) {
    cfg.blob_store_secret_key = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_STORE_SESSION_TOKEN")) {
    cfg.blob_store_session_token = s;
    cli.blob_store_set = true;
  }
  if (const char* s = getenv_s("AGENTD_BLOB_TIER_LOCAL_MAX_BYTES")) {
    try {
      long long n = std::stoll(s);
      if (n < 0) n = 0;
      cfg.blob_tier_local_max_bytes = (int64_t)n;
      cli.blob_tier_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_TIER_LOCAL_MAX_BYTES; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_TIER_LOCAL_MAX_AGE_MS")) {
    try {
      long long n = std::stoll(s);
      if (n < 0) n = 0;
      cfg.blob_tier_local_max_age_ms = (int64_t)n;
      cli.blob_tier_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_TIER_LOCAL_MAX_AGE_MS; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_TIER_PROMOTE_AFTER_MS")) {
    try {
      long long n = std::stoll(s);
      if (n < 0) n = 0;
      cfg.blob_tier_promote_after_ms = (int64_t)n;
      cli.blob_tier_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_TIER_PROMOTE_AFTER_MS; ignoring\n";
    }
  }
  if (const char* s = getenv_s("AGENTD_BLOB_TIER_PROMOTE_MAX_BYTES")) {
    try {
      long long n = std::stoll(s);
      if (n < 0) n = 0;
      cfg.blob_tier_promote_max_bytes = (int64_t)n;
      cli.blob_tier_set = true;
    } catch (...) {
      std::cerr << "Invalid AGENTD_BLOB_TIER_PROMOTE_MAX_BYTES; ignoring\n";
    }
  }
  if (const char* ms = getenv_s("AGENTD_JOB_CONCURRENCY")) {
    try { cfg.job_engine_max_concurrency = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_JOB_POLL_MS")) {
    try { cfg.job_engine_poll_ms = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_CONCURRENCY")) {
    try { cfg.workflow_engine_max_concurrency = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_POLL_MS")) {
    try { cfg.workflow_engine_poll_ms = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_MAX_INFLIGHT_PER_WORKFLOW")) {
    try { cfg.workflow_engine_max_inflight_per_workflow = std::max(1, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_MAX_INFLIGHT_PER_SESSION")) {
    try { cfg.workflow_engine_max_inflight_per_session = std::max(0, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_PER_SESSION")) {
    try { cfg.workflow_admit_max_inflight_tasks_per_session = std::max(0, std::stoi(ms)); } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_TOTAL")) {
    try { cfg.workflow_admit_max_inflight_tasks_total = std::max(0, std::stoi(ms)); } catch (...) {}
  }
  if (!cli.workflow_enable_http_tasks_set) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_ENABLE_HTTP_TASKS")) {
      cfg.workflow_enable_http_tasks = env_truthy(s);
    }
  }
  if (!cli.workflow_http_allow_hosts_set && cfg.workflow_http_allow_hosts.empty()) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS")) {
      std::vector<std::string> toks;
      parse_csv_tokens_best_effort(s, &toks);
      for (auto& t : toks) {
        t = trim_copy(t);
        if (!t.empty()) cfg.workflow_http_allow_hosts.push_back(t);
      }
    }
  }
  if (!cli.workflow_http_allow_cidrs_set && cfg.workflow_http_allow_cidrs.empty()) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS")) {
      std::vector<std::string> toks;
      parse_csv_tokens_best_effort(s, &toks);
      for (auto& t : toks) {
        t = trim_copy(t);
        if (!t.empty()) cfg.workflow_http_allow_cidrs.push_back(t);
      }
    }
  }
  if (!cli.workflow_http_deny_cidrs_set && cfg.workflow_http_deny_cidrs.empty()) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_DENY_CIDRS")) {
      std::vector<std::string> toks;
      parse_csv_tokens_best_effort(s, &toks);
      for (auto& t : toks) {
        t = trim_copy(t);
        if (!t.empty()) cfg.workflow_http_deny_cidrs.push_back(t);
      }
    }
  }
  if (!cli.workflow_http_deny_private_set) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_DENY_PRIVATE")) {
      cfg.workflow_http_deny_private_addrs = env_truthy(s);
    }
  }
  if (!cli.workflow_http_dns_pin_set) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_DNS_PIN")) {
      cfg.workflow_http_dns_pin = env_truthy(s);
    }
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_CONSOLIDATE_INTERVAL_MS")) {
    try {
      cfg.memory_consolidate_interval_ms = (int64_t)std::stoll(ms);
      if (cfg.memory_consolidate_interval_ms < 0) cfg.memory_consolidate_interval_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_CONSOLIDATE_DAILY_DAYS")) {
    try {
      cfg.memory_consolidate_daily_days = (int)std::stol(ms);
      if (cfg.memory_consolidate_daily_days < 0) cfg.memory_consolidate_daily_days = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_CONSOLIDATE_KEEP_CHECKPOINTS")) {
    try {
      cfg.memory_consolidate_keep_checkpoints = (int)std::stol(ms);
      if (cfg.memory_consolidate_keep_checkpoints < 1) cfg.memory_consolidate_keep_checkpoints = 1;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_DAILY_INTERVAL_MS")) {
    try {
      cfg.memory_recap_daily_interval_ms = (int64_t)std::stoll(ms);
      if (cfg.memory_recap_daily_interval_ms < 0) cfg.memory_recap_daily_interval_ms = 0;
      cli.memory_recap_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_WEEKLY_INTERVAL_MS")) {
    try {
      cfg.memory_recap_weekly_interval_ms = (int64_t)std::stoll(ms);
      if (cfg.memory_recap_weekly_interval_ms < 0) cfg.memory_recap_weekly_interval_ms = 0;
      cli.memory_recap_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_DAILY_DAYS")) {
    try {
      cfg.memory_recap_daily_days = (int)std::stol(ms);
      if (cfg.memory_recap_daily_days < 0) cfg.memory_recap_daily_days = 0;
      cli.memory_recap_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RECAP_WEEKLY_DAYS")) {
    try {
      cfg.memory_recap_weekly_days = (int)std::stol(ms);
      if (cfg.memory_recap_weekly_days < 0) cfg.memory_recap_weekly_days = 0;
      cli.memory_recap_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_INTERVAL_MS")) {
    try {
      cfg.memory_retention_interval_ms = (int64_t)std::stoll(ms);
      if (cfg.memory_retention_interval_ms < 0) cfg.memory_retention_interval_ms = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_DAILY_MAX_DAYS")) {
    try {
      cfg.memory_retention_daily_max_days = (int)std::stol(ms);
      if (cfg.memory_retention_daily_max_days < 0) cfg.memory_retention_daily_max_days = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_DAILY_MAX_BYTES")) {
    try {
      cfg.memory_retention_daily_max_bytes = (int64_t)std::stoll(ms);
      if (cfg.memory_retention_daily_max_bytes < 0) cfg.memory_retention_daily_max_bytes = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_CHECKPOINT_MAX_DAYS")) {
    try {
      cfg.memory_retention_checkpoint_max_days = (int)std::stol(ms);
      if (cfg.memory_retention_checkpoint_max_days < 0) cfg.memory_retention_checkpoint_max_days = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_CHECKPOINT_MAX_COUNT")) {
    try {
      cfg.memory_retention_checkpoint_max_count = (int)std::stol(ms);
      if (cfg.memory_retention_checkpoint_max_count < 0) cfg.memory_retention_checkpoint_max_count = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_STRUCTURED_DEPRECATE_DAYS")) {
    try {
      cfg.memory_retention_structured_deprecate_days = (int)std::stol(ms);
      if (cfg.memory_retention_structured_deprecate_days < 0) cfg.memory_retention_structured_deprecate_days = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_RETENTION_STRUCTURED_DEPRECATE_MAX_ENTRIES")) {
    try {
      cfg.memory_retention_structured_deprecate_max_entries = (int)std::stol(ms);
      if (cfg.memory_retention_structured_deprecate_max_entries < 0) cfg.memory_retention_structured_deprecate_max_entries = 0;
      cli.memory_retention_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_DAILY_DAYS")) {
    try {
      cfg.memory_salience_daily_days = (int)std::stol(ms);
      if (cfg.memory_salience_daily_days < 0) cfg.memory_salience_daily_days = 0;
      cli.memory_salience_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_MAX_ITEMS")) {
    try {
      cfg.memory_salience_max_items = (int)std::stol(ms);
      if (cfg.memory_salience_max_items < 1) cfg.memory_salience_max_items = 1;
      cli.memory_salience_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_STRUCTURED_MAX_ITEMS")) {
    try {
      cfg.memory_salience_structured_max_items = (int)std::stol(ms);
      if (cfg.memory_salience_structured_max_items < 0) cfg.memory_salience_structured_max_items = 0;
      cli.memory_salience_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_DAILY_MAX_ITEMS")) {
    try {
      cfg.memory_salience_daily_max_items = (int)std::stol(ms);
      if (cfg.memory_salience_daily_max_items < 0) cfg.memory_salience_daily_max_items = 0;
      cli.memory_salience_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_HALF_LIFE_DAYS")) {
    try {
      cfg.memory_salience_half_life_days = std::stod(ms);
      if (cfg.memory_salience_half_life_days < 0) cfg.memory_salience_half_life_days = 0;
      cli.memory_salience_set = true;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_MEMORY_SALIENCE_IMPORTANCE_WEIGHT")) {
    try {
      cfg.memory_salience_importance_weight = std::stod(ms);
      if (cfg.memory_salience_importance_weight < 0) cfg.memory_salience_importance_weight = 0;
      cli.memory_salience_set = true;
    } catch (...) {}
  }
  if (const char* s = getenv_s("AGENTD_OTA_ENABLE")) {
    cfg.ota_enable = env_truthy(s);
  }
  if (const char* s = getenv_s("AGENTD_OTA_COMMAND")) {
    cfg.ota_command = s;
  }
  if (const char* ms = getenv_s("AGENTD_OTA_COMMAND_TIMEOUT_MS")) {
    try {
      cfg.ota_command_timeout_ms = (int64_t)std::stoll(ms);
      if (cfg.ota_command_timeout_ms < 0) cfg.ota_command_timeout_ms = 0;
    } catch (...) {}
  }
  if (const char* ms = getenv_s("AGENTD_OTA_DRAIN_TIMEOUT_MS")) {
    try {
      cfg.ota_drain_timeout_ms = (int64_t)std::stoll(ms);
      if (cfg.ota_drain_timeout_ms < 0) cfg.ota_drain_timeout_ms = 0;
    } catch (...) {}
  }

  if (cfg.state_dir.empty()) {
    if (const char* d = getenv_s("AGENT_WD")) {
      cfg.state_dir = d;
    }
    if (const char* d = getenv_s("AGENTD_STATE_DIR")) {
      cfg.state_dir = d;
    }
  }
  if (cfg.sessions_root_dir.empty()) {
    if (const char* d = getenv_s("AGENT_WD")) {
      cfg.sessions_root_dir = d;
    }
    if (const char* d = getenv_s("AGENTD_SESSIONS_ROOT")) {
      cfg.sessions_root_dir = d;
    }
  }
  if (cfg.audio_webrtc_peer_tool_path.empty()) {
    if (const char* p = getenv_s("AGENTD_AUDIO_WEBRTC_PEER_TOOL")) {
      cfg.audio_webrtc_peer_tool_path = p;
    }
  }
  if (cfg.audio_webrtc_peer_node_bin.empty()) {
    if (const char* p = getenv_s("AGENTD_AUDIO_WEBRTC_PEER_NODE_BIN")) {
      cfg.audio_webrtc_peer_node_bin = p;
    }
  }
  if (cfg.audio_webrtc_broker_url.empty()) {
    if (const char* p = getenv_s("AGENTD_AUDIO_WEBRTC_BROKER_URL")) {
      cfg.audio_webrtc_broker_url = p;
    }
  }
  if (cfg.audio_webrtc_broker_token.empty()) {
    if (const char* p = getenv_s("AGENTD_AUDIO_WEBRTC_BROKER_TOKEN")) {
      cfg.audio_webrtc_broker_token = p;
    }
  }
  if (cfg.edge_consensus_node_tool_path.empty()) {
    if (const char* p = getenv_s("AGENTD_EDGE_CONSENSUS_NODE_TOOL")) {
      cfg.edge_consensus_node_tool_path = p;
    }
  }

  // Make the effective state/session roots explicit (so /api/v1/config can report them).
  if (cfg.state_dir.empty()) {
    // If sessions_root_dir was explicitly configured (e.g. --sessions-root), use it as the default state_dir too
    // so the daemon DB and session folders live under the same operator-chosen root.
    cfg.state_dir = cfg.sessions_root_dir.empty() ? state_dir_best_effort() : cfg.sessions_root_dir;
  }
  if (cfg.sessions_root_dir.empty()) {
    // Session root directory is `<state_dir>/session_<session_id>/`.
    cfg.sessions_root_dir = cfg.state_dir;
  }

  // DB is mandatory (canonical daemon state store). Default to agentd.db under state_dir.
  if (cfg.db_path.empty()) {
    cfg.db_path = (std::filesystem::path(cfg.state_dir) / "agentd.db").string();
  } else {
    // Make db_path absolute for clearer /api/v1/config output.
    std::error_code ec;
    const std::filesystem::path p(cfg.db_path);
    if (p.is_relative()) {
      cfg.db_path = (std::filesystem::path(cfg.state_dir) / p).lexically_normal().string();
    }
  }

  AgentDb db;
  {
    std::string db_err;
    if (!db.open(cfg.db_path, &db_err)) {
      std::cerr << "Failed to open agentd DB: " << db_err << "\n";
      std::cerr << "db_path=" << cfg.db_path << "\n";
      return 1;
    }
  }

  // Load runtime-configured daemon defaults (model/base_url/proxy/timeout + provider keys) from the DB.
  // This keeps provider keys out of browser storage and ensures all daemon state is in agentd.db.
  {
    std::string err;
    RuntimeConfigLoadOptions opt;
    opt.override_workflow_http_allow_hosts = !cli.workflow_http_allow_hosts_set;
    opt.override_workflow_http_allow_cidrs = !cli.workflow_http_allow_cidrs_set;
    opt.override_workflow_http_deny_cidrs = !cli.workflow_http_deny_cidrs_set;
    opt.override_workflow_http_deny_private_addrs = !cli.workflow_http_deny_private_set;
    opt.override_workflow_http_dns_pin = !cli.workflow_http_dns_pin_set;
    opt.override_upload_max_bytes = !cli.upload_max_bytes_set;
    opt.override_blob_store = !cli.blob_store_set;
    opt.override_blob_tier = !cli.blob_tier_set;
    opt.override_memory_recap = !cli.memory_recap_set;
    opt.override_memory_retention = !cli.memory_retention_set;
    opt.override_memory_salience = !cli.memory_salience_set;
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

  // Runtime-mutable daemon config store (requests are handled concurrently).
  DaemonConfigStore cfg_store(cfg);

  const std::string openrouter_http_referer = getenv_s("OPENROUTER_HTTP_REFERER") ? getenv_s("OPENROUTER_HTTP_REFERER") : "";
  const std::string openrouter_x_title = getenv_s("OPENROUTER_X_TITLE") ? getenv_s("OPENROUTER_X_TITLE") : "";

  auto ocfg_from_cfg = [&](const DaemonConfig& c) -> OpenAIClientConfig {
    OpenAIClientConfig ocfg;
    ocfg.base_url = c.base_url;
    ocfg.api_key = c.api_key;
    ocfg.model = c.model;
    ocfg.proxy_url = c.proxy_url;
    ocfg.timeout_ms = c.timeout_ms;
    if (!openrouter_http_referer.empty()) ocfg.openrouter_http_referer = openrouter_http_referer;
    if (!openrouter_x_title.empty()) ocfg.openrouter_x_title = openrouter_x_title;
    return ocfg;
  };

  AgentDb* db_or_null = &db;

  ToolPluginChain tool_plugins;
  ToolExtension tool_plugin_ext{};
  ToolServerChain tool_servers;
  ToolExtension tool_server_ext{};
  ToolExtensionMux tool_mux;
  ToolExtension tool_mux_ext{};
  const ToolExtension* tool_ext_or_null = nullptr;

  std::vector<ToolExtension> exts;
  std::vector<std::vector<std::string>> names_by_ext;

  if (!cli.tool_plugin_specs.empty()) {
    std::vector<ToolPluginSpec> specs = cli.tool_plugin_specs;
    std::string terr;
    if (!tool_plugins.load(specs, &terr)) {
      std::cerr << "Failed to load tool plugins: " << (terr.empty() ? "unknown error" : terr) << "\n";
      return 2;
    }
    tool_plugin_ext = tool_plugins.as_tool_extension();
    if (tool_plugin_ext.register_tools && tool_plugin_ext.execute_tool) {
      exts.push_back(tool_plugin_ext);
      names_by_ext.push_back(tool_plugins.tool_names());
    }
  }

  if (!cli.tool_server_specs.empty()) {
    std::vector<ToolServerSpec> specs = cli.tool_server_specs;
    std::string terr;
    if (!tool_servers.load(specs, &terr)) {
      std::cerr << "Failed to load tool servers: " << (terr.empty() ? "unknown error" : terr) << "\n";
      return 2;
    }
    tool_server_ext = tool_servers.as_tool_extension();
    if (tool_server_ext.register_tools && tool_server_ext.execute_tool) {
      exts.push_back(tool_server_ext);
      names_by_ext.push_back(tool_servers.tool_names());
    }
  }

  if (!exts.empty()) {
    std::string merr;
    if (!tool_mux.init(exts, names_by_ext, &merr)) {
      std::cerr << "Failed to init tool extension mux: " << (merr.empty() ? "unknown error" : merr) << "\n";
      return 2;
    }
    tool_mux_ext = tool_mux.as_tool_extension();
    tool_ext_or_null = &tool_mux_ext;
  }

  HttpServer server;
  const auto start_time = std::chrono::steady_clock::now();
  server.set_default_headers({
    {"Server", "agentd/0.1"},
  });
  server.set_options_handler([&](const HttpRequest& req, HttpResponse* resp) {
    resp->status = 204;
    resp->body.clear();
    cors_apply(req, resp, cors_cfg);
  });

  // Background GC for finished jobs (keeps daemon memory bounded for long-running usage).
  {
    const DaemonConfig cfg0 = cfg_store.snapshot();
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
  JobEngine job_engine(
    db_or_null,
    [&cfg_store]() { return cfg_store.snapshot(); },
    ocfg_from_cfg,
    tool_ext_or_null,
    cfg.sessions_root_dir,
    JobEngine::Options{
      /*max_concurrency=*/std::max(1, cfg.job_engine_max_concurrency),
      /*poll_ms=*/std::max(1, cfg.job_engine_poll_ms),
      /*max_scan_jobs=*/JobEngine::Options{}.max_scan_jobs,
    }
  );
  {
    std::string jerr;
    if (!job_engine.start(&jerr)) {
      std::cerr << "Warning: failed to start job engine: " << jerr << "\n";
    }
  }

  // Durable workflow scheduler (background).
  WorkflowEngine wf_engine(
    db_or_null,
    [&cfg_store]() { return cfg_store.snapshot(); },
    ocfg_from_cfg,
    tool_ext_or_null,
    cfg.sessions_root_dir,
    WorkflowEngine::Options{
      /*max_concurrency=*/std::max(1, cfg.workflow_engine_max_concurrency),
      /*poll_ms=*/std::max(1, cfg.workflow_engine_poll_ms),
      /*max_scan_workflows=*/WorkflowEngine::Options{}.max_scan_workflows,
      /*max_inflight_per_workflow=*/std::max(1, cfg.workflow_engine_max_inflight_per_workflow),
      /*max_inflight_per_session=*/std::max(0, cfg.workflow_engine_max_inflight_per_session),
    }
  );
  {
    std::string werr;
    if (!wf_engine.start(&werr)) {
      std::cerr << "Warning: failed to start workflow engine: " << werr << "\n";
    }
  }

  // Workflow schedule engine (background).
  WorkflowScheduleEngine wf_schedule_engine(
    db_or_null,
    [&cfg_store]() { return cfg_store.snapshot(); },
    WorkflowScheduleEngine::Options{}
  );
  {
    std::string serr;
    if (!wf_schedule_engine.start(&serr)) {
      std::cerr << "Warning: failed to start workflow schedule engine: " << serr << "\n";
    }
  }

  // Memory consolidation (background; disabled by default).
  MemoryConsolidatorEngine mem_engine(
    [&cfg_store]() { return cfg_store.snapshot(); },
    MemoryConsolidatorEngine::Options{}
  );
  {
    std::string merr;
    if (!mem_engine.start(&merr)) {
      std::cerr << "Warning: failed to start memory consolidator engine: " << merr << "\n";
    }
  }

  // Memory retention (background; disabled by default).
  MemoryRetentionEngine mem_retention_engine(
    [&cfg_store]() { return cfg_store.snapshot(); },
    MemoryRetentionEngine::Options{}
  );
  {
    std::string merr;
    if (!mem_retention_engine.start(&merr)) {
      std::cerr << "Warning: failed to start memory retention engine: " << merr << "\n";
    }
  }

  // Memory recaps (background; disabled by default).
  MemoryRecapEngine mem_recap_engine(
    [&cfg_store]() { return cfg_store.snapshot(); },
    ocfg_from_cfg,
    MemoryRecapEngine::Options{}
  );
  {
    std::string rerr;
    if (!mem_recap_engine.start(&rerr)) {
      std::cerr << "Warning: failed to start memory recap engine: " << rerr << "\n";
    }
  }

  // Edge task deadline sweeper (UM‑EEM deadlines/timeouts; platform-side best-effort).
  EdgeDeadlineSweeperEngine edge_deadline_engine(
    db_or_null,
    [&cfg_store]() { return cfg_store.snapshot(); },
    EdgeDeadlineSweeperEngine::Options{
      /*poll_ms=*/500,
      /*max_scan_rows=*/128,
    }
  );
  {
    std::string derr;
    if (!edge_deadline_engine.start(&derr)) {
      std::cerr << "Warning: failed to start edge deadline sweeper: " << derr << "\n";
    }
  }

  // Durable edge workflow runner (UM‑WF executed over UM‑BMP TASK_ASSIGN).
  EdgeWorkflowEngine edge_wf_engine(
    db_or_null,
    [&cfg_store]() { return cfg_store.snapshot(); },
    EdgeWorkflowEngine::Options{
      /*poll_ms=*/200,
      /*max_scan_workflows=*/64,
      /*max_dispatch_per_tick=*/64,
    }
  );
  {
    std::string werr;
    if (!edge_wf_engine.start(&werr)) {
      std::cerr << "Warning: failed to start edge workflow engine: " << werr << "\n";
    }
  }

  server.handle("GET", "/api/v1/health", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = build_health_body(start_time);
  });
  server.handle("GET", "/api/v1/ready", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = build_ready_body(start_time, db_or_null);
  });
  server.handle("GET", "/metrics", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "text/plain; version=0.0.4; charset=utf-8";
    resp->body = build_metrics_body(start_time, db_or_null);
  });

  server.handle("GET", "/api/v1/config", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_config_endpoint(cur, cors_cfg, req, resp);
  });

  server.handle("GET", "/api/v1/caps", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_caps_endpoint(cur, cors_cfg, start_time, req, resp);
  });

  server.handle("POST", "/api/v1/config/update", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_config_update_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/edge/auth/trust_roots", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_trust_roots_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/trust_roots/rotate", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_edge_auth_trust_roots_rotate_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/trust_roots/send", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_trust_roots_send_endpoint(cur, db_or_null, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/edge/auth/cert_roots", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_cert_roots_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/cert_roots/rotate", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_edge_auth_cert_roots_rotate_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/cert_roots/send", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_cert_roots_send_endpoint(cur, db_or_null, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/cert_roots/verify_chain", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_cert_roots_verify_chain_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/edge/auth/node_binding", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_node_binding_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/provision_node", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_edge_auth_provision_node_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/edge/auth/revocations", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_revocations_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/revocations/update", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_edge_auth_revocations_update_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/auth/revocations/send", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_auth_revocations_send_endpoint(cur, db_or_null, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/edge/consensus/membership", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_consensus_membership_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/consensus/membership/rotate", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_edge_consensus_membership_rotate_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/edge/consensus/membership/send", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_consensus_membership_send_endpoint(cur, db_or_null, cors_cfg, req, resp);
  });

  server.handle("GET", "/api/v1/client/prefs", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_client_prefs_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/client/prefs", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_client_prefs_post_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/ota/update", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_ota_update_endpoint(cur, cors_cfg, req, resp, db_or_null);
  });
  server.handle("GET", "/api/v1/ota/status", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_ota_status_endpoint(cur, cors_cfg, req, resp, db_or_null);
  });

  server.handle("GET", "/api/v1/diagnostics", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = cfg_store.snapshot();
    handle_diagnostics_endpoint(cur, db_or_null, start_time, req, resp);
  });
  server.handle("GET", "/api/v1/diagnostics/providers", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = cfg_store.snapshot();
    handle_diagnostics_providers_endpoint(cur, start_time, req, resp);
  });
  server.handle("POST", "/api/v1/diagnostics/provider_test", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_diagnostics_provider_test_endpoint(cur, ocfg, db_or_null, tool_ext_or_null, cur.sessions_root_dir, req, resp);
  });

  server.handle("GET", "/api/v1/tools", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_tools_endpoint(cur, cors_cfg, cur.sessions_root_dir, tool_ext_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/sandbox/mount_validate", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_sandbox_mount_validate_endpoint(cur, cors_cfg, req, resp);
  });

  server.handle("GET", "/api/v1/openrouter/models", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    const DaemonConfig cur = cfg_store.snapshot();
    if (!daemon_require_auth(cur, req, resp)) return;
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_openrouter_models_endpoint(ocfg, !cur.auth_token.empty(), req, resp);
    return;
  });

  server.handle("GET", "/api/v1/file", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_file_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/blob/upload", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_upload_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/blob", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/blob/meta", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_meta_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/blob/retain", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_retain_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/blob/gc", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_gc_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/blob/tier/enforce", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_tier_enforce_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/blob/archive", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_archive_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/blob/restore", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_blob_restore_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/memory/consolidate", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_consolidate_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/memory/retention/enforce", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_retention_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/checkpoints", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_checkpoints_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/correlate", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_correlate_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/memory/correlation/index", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_correlation_index_build_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/query", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_query_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/index", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_index_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/salience", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_salience_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/recaps", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_memory_recaps_endpoint(cur, ocfg, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/memory/recaps", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_memory_recaps_endpoint(cur, ocfg, cors_cfg, req, resp);
  });

  const std::string sessions_root_dir = cfg_store.snapshot().sessions_root_dir;

  server.handle("GET", "/api/v1/sessions", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_sessions_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/session/new", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_new_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/audit", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_audit_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/client_events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_client_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/session/voice_control", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_voice_control_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/voice_stats", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_voice_stats_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/session/voice_webrtc_peer", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_voice_webrtc_peer_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/voice_webrtc_peer", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_voice_webrtc_peer_status_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/clients", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_clients_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/artifacts", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_artifacts_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/session/scene", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_scene_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/session/scene/apply", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_scene_apply_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/session/ui_event", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_ui_event_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  // Client collaboration protocol (preferred name). This is an alias of /session/ui_event.
  server.handle("POST", "/api/v1/session/client_event", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_ui_event_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/moderator/directive", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_moderator_directive_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/moderator/task", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_moderator_task_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/moderator/events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_moderator_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/approvals", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_approvals_list_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle_prefix("GET", "/api/v1/approvals/", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_approvals_prefix_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle_prefix("POST", "/api/v1/approvals/", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_approvals_prefix_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  // Session-scoped uploads (UI -> daemon). Returns session-relative paths that can be fetched via /api/v1/file.
  server.handle("POST", "/api/v1/session/upload", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_upload_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("DELETE", "/api/v1/session", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_session_delete_endpoint(cur, cors_cfg, db_or_null, sessions_root_dir, req, resp);
  });

  // Optional: read-only troubleshooting DB queries (enabled only when DB is enabled at startup).
  server.handle("GET", "/api/v1/db/runs", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_runs_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/run", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_run_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/artifacts", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_artifacts_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/ui_actions", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_ui_actions_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/sessions", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_sessions_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/messages", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_messages_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/client_events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_client_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/blobs", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_blobs_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/blob", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_blob_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/analytics/blobs", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_blob_analytics_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/workflows", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_workflows_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/workflow", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_workflow_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/workflow_tasks", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_workflow_tasks_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/workflow_events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_workflow_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/edge_workflows", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_edge_workflows_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/edge_workflow", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_edge_workflow_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/edge_workflow_steps", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_edge_workflow_steps_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/edge_workflow_events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_edge_workflow_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/analytics/workflows", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_workflow_analytics_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/analytics/workflows/export", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_workflow_analytics_export_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/analytics/edge", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_edge_analytics_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/analytics/edge/export", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_db_edge_analytics_export_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/run", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_run_endpoint(cur, ocfg, cors_cfg, db_or_null, tool_ext_or_null, sessions_root_dir, req, resp);
  });

  server.handle("GET", "/api/v1/run/replay", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_run_replay_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/run/attestation", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_run_attestation_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  // Async run: returns a job id immediately and completes in the background.
  server.handle("POST", "/api/v1/run_async", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_run_async_endpoint(cur, ocfg, cors_cfg, db_or_null, tool_ext_or_null, sessions_root_dir, req, resp);
  });

  server.handle("POST", "/api/v1/orchestrate", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_orchestrate_endpoint(cur, ocfg, cors_cfg, db_or_null, tool_ext_or_null, sessions_root_dir, req, resp);
  });

  server.handle("POST", "/api/v1/avm/job_scan", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_avm_job_scan_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/avm/policy_scan", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_avm_policy_scan_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/avm/inspect", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_avm_inspect_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/avm/verify_strict", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_avm_verify_strict_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/avm/trace_hash", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_avm_trace_hash_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("POST", "/api/v1/avm/capsule_run", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_avm_capsule_run_endpoint(cur, cors_cfg, req, resp);
  });

  server.handle("POST", "/api/v1/workflow/submit", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_submit_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/workflow", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/workflow/stats", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_stats_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/workflows", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_list_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/workflow/cancel", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_cancel_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/workflow_schedules", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_create_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/workflow_schedules", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_list_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/workflow_schedule", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("DELETE", "/api/v1/workflow_schedule", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_delete_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/workflow_schedule/pause", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_pause_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/workflow_schedule/resume", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_resume_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/workflow_schedule/runs", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_schedule_runs_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/workflow/events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  // Server-Sent Events stream for durable workflow progress. Streams `workflow_event` records and ends with `workflow_done`.
  server.handle_stream("GET", "/api/v1/workflow/stream", [&](const HttpRequest& req, socket_t client_fd) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_stream_endpoint(cur.auth_token, cors_cfg, db_or_null, req, client_fd);
  });

  // Edge interop endpoints (UM‑EAIS / UM‑BMP transport mapping).
  server.handle("POST", "/api/v1/edge/message", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_message_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/outbox", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_outbox_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/nodes", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_nodes_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/node", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/edge/node/consensus_runtime", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_consensus_runtime_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/node/consensus_runtime", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_consensus_runtime_status_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/node/caps", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_caps_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/node/manifest_bundle", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_manifest_bundle_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/edge/node/manifest_bundle/send", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_manifest_bundle_send_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/edge/task/assign", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_task_assign_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/task", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_task_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/edge/rule/upsert", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_rule_upsert_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/rules", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_rules_list_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("DELETE", "/api/v1/edge/rule", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_rule_delete_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/edge/workflow/submit", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_workflow_submit_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("POST", "/api/v1/edge/workflow/cancel", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_workflow_cancel_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/workflow", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_workflow_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/edge/workflow/events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_workflow_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });
  server.handle_stream("GET", "/api/v1/edge/workflow/stream", [&](const HttpRequest& req, socket_t client_fd) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_workflow_stream_endpoint(cur.auth_token, cors_cfg, db_or_null, req, client_fd);
  });
  server.handle("GET", "/api/v1/edge/workflows", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_workflow_list_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/trace", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_trace_lookup_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("GET", "/api/v1/job", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_job_get_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/job/cancel", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_job_cancel_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  // Server-Sent Events stream for job progress (preferred UI path vs polling).
  // This endpoint streams `agent_event` events (same object shape as entries in the `events` array) and ends with `job_done`.
  server.handle_stream("GET", "/api/v1/job/stream", [&](const HttpRequest& req, socket_t client_fd) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_job_stream_endpoint(cur.auth_token, cors_cfg, db_or_null, req, client_fd);
  });

  server.handle("DELETE", "/api/v1/job", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_job_delete_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  std::string err;
  const DaemonConfig cfg_final = cfg_store.snapshot();
  if (!host_is_loopback(cfg_final.listen_host) && cfg_final.auth_token.empty() && !cfg_final.allow_unauthenticated_non_loopback) {
    std::cerr << "Refusing to bind agentd to non-loopback host without auth.\n";
    std::cerr << "Provide --auth-token <token> (recommended) or pass --allow-unauth to override (insecure).\n";
    std::cerr << "host=" << cfg_final.listen_host << "\n";
    edge_wf_engine.stop();
    edge_deadline_engine.stop();
    mem_engine.stop();
    wf_schedule_engine.stop();
    wf_engine.stop();
    job_engine.stop();
    return 2;
  }
  std::cerr << "agentd listening on http://" << cfg_final.listen_host << ":" << cfg_final.listen_port << "\n";
  if (!server.serve(cfg_final.listen_host, cfg_final.listen_port, &err)) {
    std::cerr << "agentd failed: " << err << "\n";
    edge_wf_engine.stop();
    edge_deadline_engine.stop();
    mem_engine.stop();
    wf_schedule_engine.stop();
    wf_engine.stop();
    job_engine.stop();
    return 1;
  }
  edge_wf_engine.stop();
  edge_deadline_engine.stop();
  mem_engine.stop();
  wf_schedule_engine.stop();
  wf_engine.stop();
  job_engine.stop();
  return 0;
}
