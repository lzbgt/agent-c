#include "http_server.h"

#include "cors.h"
#include "daemon_auth.h"
#include "daemon_config.h"
#include "config_endpoint.h"
#include "file_endpoint.h"
#include "sandbox_policy.h"
#include "string_util.h"
#include "openrouter_models_endpoint.h"
#include "job_stream_endpoint.h"
#include "tools_endpoint.h"
#include "session_endpoints.h"
#include "job_endpoints.h"
#include "run_endpoints.h"
#include "db_query_endpoints.h"

#include "agent_db.h"

#include "openai_client.h"

#include "job_manager.h"
#include "openrouter_util.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <signal.h>

using namespace agentd;

static const char* getenv_s(const char* k) {
  const char* v = std::getenv(k);
  return (v && v[0]) ? v : nullptr;
}

static std::string home_dir_best_effort() {
  if (const char* h = getenv_s("HOME")) {
    return h;
  }
  return std::filesystem::current_path().string();
}

static std::string state_dir_best_effort() {
  return (std::filesystem::path(home_dir_best_effort()) / ".agent").string();
}

static bool host_is_loopback(std::string host) {
  host = lower_copy(std::move(host));
  if (host == "localhost") return true;
  if (host == "::1" || host == "[::1]") return true;
  if (host.rfind("127.", 0) == 0) return true;
  if (host == "127.0.0.1") return true;
  return false;
}

static std::string trim_copy(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

static bool parse_tool_call_limit_spec(const std::string& spec, std::string* out_tool, size_t* out_max_calls) {
  if (out_tool) out_tool->clear();
  if (out_max_calls) *out_max_calls = 0;
  const std::string s = trim_copy(spec);
  const size_t eq = s.find('=');
  if (eq == std::string::npos) return false;
  const std::string tool = trim_copy(s.substr(0, eq));
  const std::string num = trim_copy(s.substr(eq + 1));
  if (tool.empty() || num.empty()) return false;
  try {
    const size_t v = (size_t)std::stoull(num);
    if (out_tool) *out_tool = tool;
    if (out_max_calls) *out_max_calls = v;
    return true;
  } catch (...) {
    return false;
  }
}

static void upsert_tool_call_limit(std::vector<std::pair<std::string, size_t>>* limits, std::string tool, size_t max_calls) {
  if (!limits) return;
  if (tool.empty()) return;
  for (auto& p : *limits) {
    if (p.first == tool) {
      p.second = max_calls;
      return;
    }
  }
  limits->push_back({std::move(tool), max_calls});
}

static bool parse_tool_call_limits_csv(
  const std::string& csv,
  std::vector<std::pair<std::string, size_t>>* out_limits,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out_limits) return false;
  out_limits->clear();

  size_t i = 0;
  while (i < csv.size()) {
    size_t j = csv.find(',', i);
    if (j == std::string::npos) j = csv.size();
    std::string tok = trim_copy(csv.substr(i, j - i));
    if (!tok.empty()) {
      std::string tool;
      size_t n = 0;
      if (!parse_tool_call_limit_spec(tok, &tool, &n)) {
        if (out_error) *out_error = std::string("invalid tool call limit spec: '") + tok + "'";
        return false;
      }
      upsert_tool_call_limit(out_limits, tool, n);
    }
    i = j + 1;
  }
  return true;
}

int main(int argc, char** argv) {
  // On macOS (and many POSIX systems), writing to a closed socket can raise SIGPIPE,
  // which terminates the process by default. Our HTTP server uses plain ::write(),
  // so we must ignore SIGPIPE to avoid daemon exits that look like "hangs" to clients.
  (void)::signal(SIGPIPE, SIG_IGN);

  DaemonConfig cfg;
  cfg.host_scope_root = std::filesystem::current_path().string();
  // Minimal flag parsing (daemon is host-only; core remains argv/env-free).
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    auto take = [&](std::string* out) -> bool {
      if (i + 1 >= argc) return false;
      *out = argv[++i];
      return true;
    };
    if (a == "--host") {
      if (!take(&cfg.listen_host)) {
        std::cerr << "Missing value for --host\n";
        return 2;
      }
    } else if (a == "--auth-token") {
      if (!take(&cfg.auth_token)) {
        std::cerr << "Missing value for --auth-token\n";
        return 2;
      }
    } else if (a == "--allow-unauth") {
      cfg.allow_unauthenticated_non_loopback = true;
    } else if (a == "--port") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --port\n";
        return 2;
      }
      try {
        const unsigned long p = std::stoul(v);
        cfg.listen_port = (uint16_t)p;
      } catch (...) {
        std::cerr << "Invalid --port\n";
        return 2;
      }
    } else if (a == "--model") {
      if (!take(&cfg.model)) {
        std::cerr << "Missing value for --model\n";
        return 2;
      }
    } else if (a == "--state-dir") {
      if (!take(&cfg.state_dir)) {
        std::cerr << "Missing value for --state-dir\n";
        return 2;
      }
    } else if (a == "--sessions-root") {
      if (!take(&cfg.sessions_root_dir)) {
        std::cerr << "Missing value for --sessions-root\n";
        return 2;
      }
    } else if (a == "--summary-model") {
      if (!take(&cfg.summary_model)) {
        std::cerr << "Missing value for --summary-model\n";
        return 2;
      }
    } else if (a == "--summary-max-chars") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --summary-max-chars\n";
        return 2;
      }
      try {
        cfg.summary_max_chars = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --summary-max-chars\n";
        return 2;
      }
    } else if (a == "--base-url") {
      if (!take(&cfg.base_url)) {
        std::cerr << "Missing value for --base-url\n";
        return 2;
      }
    } else if (a == "--api-key") {
      if (!take(&cfg.api_key)) {
        std::cerr << "Missing value for --api-key\n";
        return 2;
      }
    } else if (a == "--proxy") {
      if (!take(&cfg.proxy_url)) {
        std::cerr << "Missing value for --proxy\n";
        return 2;
      }
    } else if (a == "--db-path") {
      if (!take(&cfg.db_path)) {
        std::cerr << "Missing value for --db-path\n";
        return 2;
      }
    } else if (a == "--no-db") {
      cfg.db_path.clear();
      cfg.db_disabled = true;
    } else if (a == "--timeout-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --timeout-ms\n";
        return 2;
      }
      try {
        cfg.timeout_ms = (long)std::stoll(v);
      } catch (...) {
        std::cerr << "Invalid --timeout-ms\n";
        return 2;
      }
    } else if (a == "--job-ttl-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --job-ttl-ms\n";
        return 2;
      }
      try {
        cfg.job_ttl_ms = (int64_t)std::stoll(v);
        if (cfg.job_ttl_ms < 0) cfg.job_ttl_ms = 0;
      } catch (...) {
        std::cerr << "Invalid --job-ttl-ms\n";
        return 2;
      }
    } else if (a == "--max-jobs") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-jobs\n";
        return 2;
      }
      try {
        cfg.max_jobs = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-jobs\n";
        return 2;
      }
    } else if (a == "--max-steps-default") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-steps-default\n";
        return 2;
      }
      try {
        cfg.max_steps_default = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-steps-default\n";
        return 2;
      }
    } else if (a == "--max-tool-calls-total-default") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-tool-calls-total-default\n";
        return 2;
      }
      try {
        cfg.max_tool_calls_total_default = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-tool-calls-total-default\n";
        return 2;
      }
    } else if (a == "--max-tool-calls-per-tool-default") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-tool-calls-per-tool-default\n";
        return 2;
      }
      try {
        cfg.max_tool_calls_per_tool_default = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-tool-calls-per-tool-default\n";
        return 2;
      }
    } else if (a == "--tool-call-limit") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --tool-call-limit\n";
        return 2;
      }
      std::string tool;
      size_t max_calls = 0;
      if (!parse_tool_call_limit_spec(v, &tool, &max_calls)) {
        std::cerr << "Invalid --tool-call-limit (expected: tool=max_calls)\n";
        return 2;
      }
      upsert_tool_call_limit(&cfg.tool_call_limits_default, std::move(tool), max_calls);
    } else if (a == "--tools") {
      if (!take(&cfg.tools)) {
        std::cerr << "Missing value for --tools\n";
        return 2;
      }
    } else if (a == "--tools-root") {
      if (!take(&cfg.tools_root)) {
        std::cerr << "Missing value for --tools-root\n";
        return 2;
      }
    } else if (a == "--host-policy") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --host-policy\n";
        return 2;
      }
      if (v == "full") {
        cfg.host_policy = HostToolsetPolicyMode::Full;
      } else if (v == "readonly") {
        cfg.host_policy = HostToolsetPolicyMode::ReadOnly;
      } else {
        std::cerr << "Invalid --host-policy (expected: full|readonly)\n";
        return 2;
      }
    } else if (a == "--host-scope") {
      if (!take(&cfg.host_scope_root)) {
        std::cerr << "Missing value for --host-scope\n";
        return 2;
      }
    } else if (a == "--yolo") {
      cfg.yolo_default = true;
    } else if (a == "--no-yolo") {
      cfg.yolo_default = false;
    } else if (a == "--no-default-system") {
      cfg.no_default_system = true;
    } else if (a == "--cors-origin") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --cors-origin\n";
        return 2;
      }
      cfg.cors_origins_set = true;
      cfg.cors_origins.push_back(v);
    } else if (a == "--cors-allow-headers") {
      if (!take(&cfg.cors_allow_headers)) {
        std::cerr << "Missing value for --cors-allow-headers\n";
        return 2;
      }
    } else if (a == "--cors-allow-methods") {
      if (!take(&cfg.cors_allow_methods)) {
        std::cerr << "Missing value for --cors-allow-methods\n";
        return 2;
      }
    } else if (a == "--cors-max-age") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --cors-max-age\n";
        return 2;
      }
      try {
        cfg.cors_max_age_seconds = std::max(0, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --cors-max-age\n";
        return 2;
      }
    } else if (a == "--no-cors") {
      cfg.cors_disabled = true;
      cfg.cors_origins_set = true;
      cfg.cors_origins.clear();
    } else if (a == "--help" || a == "-h") {
      std::cerr
        << "Usage: agentd [options]\n"
        << "  --host <ip>          Listen host (default: 127.0.0.1)\n"
        << "  --auth-token <tok>   Require Authorization: Bearer <tok> (default: disabled)\n"
        << "  --allow-unauth       Allow non-loopback without auth (INSECURE)\n"
        << "  --port <n>           Listen port (default: 8123)\n"
        << "  --cors-origin <origin|*>   Allowed browser Origin (repeatable; default: '*' on loopback, else disabled)\n"
        << "  --cors-allow-headers <csv> Allow headers (default includes Authorization, X-OpenRouter-Key)\n"
        << "  --cors-allow-methods <csv> Allow methods (default: GET, POST, DELETE, OPTIONS)\n"
        << "  --cors-max-age <n>         Preflight cache max-age seconds (default: 600)\n"
        << "  --no-cors                  Disable CORS headers entirely\n"
        << "  --model <name>       Default model\n"
        << "  --summary-model <name>   Optional model for compaction summaries (tools=none)\n"
        << "  --summary-max-chars <n>  Max chars for inserted summary (default: 1200)\n"
        << "  --base-url <url>     Default base url\n"
        << "  --api-key <key>      Default API key (else env)\n"
        << "  --proxy <url>        Optional HTTP proxy override (else env HTTPS_PROXY/http_proxy)\n"
        << "  --state-dir <dir>    Base state dir (default: ~/.agent)\n"
        << "  --sessions-root <dir> Session store root (default: <state-dir>/sessions)\n"
        << "  --db-path <path>     Optional SQLite DB path (troubleshooting mirror; default: disabled)\n"
        << "  --no-db              Disable DB mirror even if AGENTD_DB_PATH is set\n"
        << "  --timeout-ms <n>     Provider HTTP timeout in ms (default: 60000)\n"
        << "  --job-ttl-ms <n>     GC finished jobs older than n ms (default: 1800000)\n"
        << "  --max-jobs <n>       Keep at most n jobs in memory (default: 256)\n"
        << "  --max-steps-default <n> Default tool-loop max steps when requests omit it (default: 32; 0 means unlimited)\n"
        << "  --max-tool-calls-total-default <n> Default tool-loop total tool calls cap when requests omit it (default: 128; 0 means unlimited)\n"
        << "  --max-tool-calls-per-tool-default <n> Default tool-loop per-tool call cap when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --tool-call-limit <tool>=<n> Default per-tool call limit (repeatable; 0 means unlimited for that tool)\n"
        << "  --tools host|basic|none   Default toolset (default: host)\n"
        << "  --tools-root <path>  Root/working dir for file edits (default: unrestricted)\n"
        << "  --host-policy full|readonly  Host tool safety policy (default: full)\n"
        << "  --host-scope <path>  Host scope root for tools_root=\"@host\" (default: current dir)\n"
        << "  --yolo / --no-yolo   Default unrestricted mode (default: yolo)\n"
        << "  --no-default-system  Disable default host system hint (host tools only)\n";
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 2;
    }
  }

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
    } else {
      if (const char* k = getenv_s("OPENAI_API_KEY")) cfg.api_key = k;
      else if (const char* k2 = getenv_s("OPENROUTER_API_KEY")) cfg.api_key = k2;
      else if (const char* k3 = getenv_s("DEEPSEEK_API_KEY")) cfg.api_key = k3;
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
  if (cfg.auth_token.empty()) {
    if (const char* t = getenv_s("AGENTD_AUTH_TOKEN")) {
      cfg.auth_token = t;
    }
  }
  if (cfg.db_path.empty()) {
    if (!cfg.db_disabled) {
      if (const char* p = getenv_s("AGENTD_DB_PATH")) {
      cfg.db_path = p;
      }
    }
  }

  if (cfg.state_dir.empty()) {
    if (const char* d = getenv_s("AGENTD_STATE_DIR")) {
      cfg.state_dir = d;
    }
  }
  if (cfg.sessions_root_dir.empty()) {
    if (const char* d = getenv_s("AGENTD_SESSIONS_ROOT")) {
      cfg.sessions_root_dir = d;
    }
  }

  // Make the effective state/session roots explicit (so /api/v1/config can report them).
  if (cfg.state_dir.empty()) {
    cfg.state_dir = state_dir_best_effort();
  }
  if (cfg.sessions_root_dir.empty()) {
    cfg.sessions_root_dir = (std::filesystem::path(cfg.state_dir) / "sessions").string();
  }

  OpenAIClientConfig ocfg;
  ocfg.base_url = cfg.base_url;
  ocfg.api_key = cfg.api_key;
  ocfg.model = cfg.model;
  ocfg.proxy_url = cfg.proxy_url;
  ocfg.timeout_ms = cfg.timeout_ms;
  if (const char* r = getenv_s("OPENROUTER_HTTP_REFERER")) {
    ocfg.openrouter_http_referer = r;
  }
  if (const char* t = getenv_s("OPENROUTER_X_TITLE")) {
    ocfg.openrouter_x_title = t;
  }

  AgentDb db;
  AgentDb* db_or_null = nullptr;
  if (!cfg.db_path.empty()) {
    std::string db_err;
    if (!db.open(cfg.db_path, &db_err)) {
      std::cerr << "Failed to open agentd DB: " << db_err << "\n";
      std::cerr << "db_path=" << cfg.db_path << "\n";
      return 1;
    }
    db_or_null = &db;
  }

  HttpServer server;
  server.set_default_headers({
    {"Server", "agentd/0.1"},
  });
  server.set_options_handler([&](const HttpRequest& req, HttpResponse* resp) {
    resp->status = 204;
    resp->body.clear();
    cors_apply(req, resp, cors_cfg);
  });

  // Background GC for finished jobs (keeps daemon memory bounded for long-running usage).
  if (cfg.job_ttl_ms > 0 || cfg.max_jobs > 0) {
    const int64_t ttl_ms = cfg.job_ttl_ms;
    const size_t max_jobs = cfg.max_jobs;
    std::thread([ttl_ms, max_jobs]() {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        job_gc(ttl_ms, max_jobs);
      }
    }).detach();
  }

  server.handle("GET", "/api/v1/health", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":true,"service":"agentd","version":"0.1"})";
  });

  server.handle("GET", "/api/v1/config", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_config_endpoint(cfg, cors_cfg, req, resp);
  });

  server.handle("GET", "/api/v1/tools", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_tools_endpoint(cfg, cors_cfg, req, resp);
  });

  server.handle("GET", "/api/v1/openrouter/models", [&](const HttpRequest& req, HttpResponse* resp) {
    cors_apply(req, resp, cors_cfg);
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    if (!daemon_require_auth(cfg, req, resp)) return;
    handle_openrouter_models_endpoint(ocfg, !cfg.auth_token.empty(), req, resp);
    return;
  });

  server.handle("GET", "/api/v1/file", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_file_endpoint(cfg, cors_cfg, req, resp);
  });

  const std::string sessions_root_dir = cfg.sessions_root_dir;

  server.handle("GET", "/api/v1/sessions", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_sessions_endpoint(cfg, cors_cfg, sessions_root_dir, req, resp);
  });

  server.handle("POST", "/api/v1/session/new", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_session_new_endpoint(cfg, cors_cfg, sessions_root_dir, req, resp);
  });

  server.handle("GET", "/api/v1/session", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_session_get_endpoint(cfg, cors_cfg, sessions_root_dir, req, resp);
  });

  server.handle("GET", "/api/v1/session/audit", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_session_audit_endpoint(cfg, cors_cfg, sessions_root_dir, req, resp);
  });

  server.handle("GET", "/api/v1/session/artifacts", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_session_artifacts_endpoint(cfg, cors_cfg, sessions_root_dir, req, resp);
  });

  server.handle("DELETE", "/api/v1/session", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_session_delete_endpoint(cfg, cors_cfg, db_or_null, sessions_root_dir, req, resp);
  });

  // Optional: read-only troubleshooting DB queries (enabled only when DB is enabled at startup).
  server.handle("GET", "/api/v1/db/runs", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_db_runs_endpoint(cfg, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/run", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_db_run_endpoint(cfg, cors_cfg, db_or_null, req, resp);
  });
  server.handle("GET", "/api/v1/db/artifacts", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_db_artifacts_endpoint(cfg, cors_cfg, db_or_null, req, resp);
  });

  server.handle("POST", "/api/v1/run", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_run_endpoint(cfg, ocfg, cors_cfg, db_or_null, sessions_root_dir, req, resp);
  });

  // Async run: returns a job id immediately and completes in the background.
  server.handle("POST", "/api/v1/run_async", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_run_async_endpoint(cfg, ocfg, cors_cfg, db_or_null, sessions_root_dir, req, resp);
  });

  server.handle("GET", "/api/v1/job", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_job_get_endpoint(cfg, cors_cfg, req, resp);
  });

  server.handle("POST", "/api/v1/job/cancel", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_job_cancel_endpoint(cfg, cors_cfg, req, resp);
  });

  // Server-Sent Events stream for job progress (preferred UI path vs polling).
  // This endpoint streams `agent_event` events (same object shape as entries in the `events` array) and ends with `job_done`.
  server.handle_stream("GET", "/api/v1/job/stream", [&](const HttpRequest& req, int client_fd) {
    handle_job_stream_endpoint(cfg.auth_token, cors_cfg, req, client_fd);
  });

  server.handle("DELETE", "/api/v1/job", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_job_delete_endpoint(cfg, cors_cfg, req, resp);
  });

  std::string err;
  if (!host_is_loopback(cfg.listen_host) && cfg.auth_token.empty() && !cfg.allow_unauthenticated_non_loopback) {
    std::cerr << "Refusing to bind agentd to non-loopback host without auth.\n";
    std::cerr << "Provide --auth-token <token> (recommended) or pass --allow-unauth to override (insecure).\n";
    std::cerr << "host=" << cfg.listen_host << "\n";
    return 2;
  }
  std::cerr << "agentd listening on http://" << cfg.listen_host << ":" << cfg.listen_port << "\n";
  if (!server.serve(cfg.listen_host, cfg.listen_port, &err)) {
    std::cerr << "agentd failed: " << err << "\n";
    return 1;
  }
  return 0;
}
