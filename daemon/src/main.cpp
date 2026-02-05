#include "http_server.h"

#include "cors.h"
#include "daemon_auth.h"
#include "daemon_config.h"
#include "config_endpoint.h"
#include "avm_endpoints.h"
#include "file_endpoint.h"
#include "sandbox_policy.h"
#include "string_util.h"
#include "openrouter_models_endpoint.h"
#include "job_stream_endpoint.h"
#include "tools_endpoint.h"
#include "tool_plugins.h"
#include "tool_servers.h"
#include "tool_extension_mux.h"
#include "session_endpoints.h"
#include "job_endpoints.h"
#include "orchestrate_endpoints.h"
#include "memory_endpoints.h"
#include "memory_consolidator.h"
#include "run_endpoints.h"
#include "trace_endpoints.h"
#include "workflow_endpoints.h"
#include "workflow_engine.h"
#include "workflow_stream_endpoint.h"
#include "db_query_endpoints.h"
#include "edge_interop_endpoints.h"
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

#include <filesystem>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <cctype>
#include <signal.h>

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

static void parse_csv_tokens_best_effort(const std::string& csv, std::vector<std::string>* out) {
  if (!out) return;
  out->clear();
  size_t i = 0;
  while (i < csv.size()) {
    size_t j = csv.find(',', i);
    if (j == std::string::npos) j = csv.size();
    std::string tok = trim_copy(csv.substr(i, j - i));
    if (!tok.empty()) out->push_back(tok);
    i = j + 1;
  }
}

int main(int argc, char** argv) {
  // On macOS (and many POSIX systems), writing to a closed socket can raise SIGPIPE,
  // which terminates the process by default. Our HTTP server uses plain ::write(),
  // so we must ignore SIGPIPE to avoid daemon exits that look like "hangs" to clients.
  (void)::signal(SIGPIPE, SIG_IGN);

  DaemonConfig cfg;
  bool system_profile_set = false;
  bool workflow_enable_http_tasks_set = false;
  std::vector<std::string> tool_plugin_paths;
  std::vector<ToolServerSpec> tool_server_specs;
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
    } else if (a == "--job-concurrency") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --job-concurrency\n";
        return 2;
      }
      try {
        cfg.job_engine_max_concurrency = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --job-concurrency\n";
        return 2;
      }
    } else if (a == "--job-poll-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --job-poll-ms\n";
        return 2;
      }
      try {
        cfg.job_engine_poll_ms = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --job-poll-ms\n";
        return 2;
      }
    } else if (a == "--workflow-concurrency") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-concurrency\n";
        return 2;
      }
      try {
        cfg.workflow_engine_max_concurrency = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --workflow-concurrency\n";
        return 2;
      }
    } else if (a == "--workflow-poll-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-poll-ms\n";
        return 2;
      }
      try {
        cfg.workflow_engine_poll_ms = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --workflow-poll-ms\n";
        return 2;
      }
    } else if (a == "--workflow-max-inflight-per-workflow") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-max-inflight-per-workflow\n";
        return 2;
      }
      try {
        cfg.workflow_engine_max_inflight_per_workflow = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --workflow-max-inflight-per-workflow\n";
        return 2;
      }
    } else if (a == "--workflow-max-inflight-per-session") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-max-inflight-per-session\n";
        return 2;
      }
      try {
        const int n = std::stoi(v);
        cfg.workflow_engine_max_inflight_per_session = std::max(0, n);
      } catch (...) {
        std::cerr << "Invalid --workflow-max-inflight-per-session\n";
        return 2;
      }
    } else if (a == "--workflow-fair-queue-policy") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-fair-queue-policy\n";
        return 2;
      }
      cfg.workflow_engine_fair_queue_policy = v;
    } else if (a == "--workflow-fair-queue-max-session-weight") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-fair-queue-max-session-weight\n";
        return 2;
      }
      try {
        const int n = std::stoi(v);
        cfg.workflow_engine_fair_queue_max_session_weight = std::max(1, n);
      } catch (...) {
        std::cerr << "Invalid --workflow-fair-queue-max-session-weight\n";
        return 2;
      }
    } else if (a == "--workflow-fair-queue-max-schedule-len") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-fair-queue-max-schedule-len\n";
        return 2;
      }
      try {
        const int n = std::stoi(v);
        cfg.workflow_engine_fair_queue_max_schedule_len = std::max(16, n);
      } catch (...) {
        std::cerr << "Invalid --workflow-fair-queue-max-schedule-len\n";
        return 2;
      }
    } else if (a == "--workflow-drr-cost-model") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-drr-cost-model\n";
        return 2;
      }
      cfg.workflow_engine_drr_cost_model = v;
    } else if (a == "--workflow-admit-max-inflight-tasks-per-session") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-admit-max-inflight-tasks-per-session\n";
        return 2;
      }
      try {
        const int n = std::stoi(v);
        cfg.workflow_admit_max_inflight_tasks_per_session = std::max(0, n);
      } catch (...) {
        std::cerr << "Invalid --workflow-admit-max-inflight-tasks-per-session\n";
        return 2;
      }
    } else if (a == "--workflow-admit-max-inflight-tasks-total") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-admit-max-inflight-tasks-total\n";
        return 2;
      }
      try {
        const int n = std::stoi(v);
        cfg.workflow_admit_max_inflight_tasks_total = std::max(0, n);
      } catch (...) {
        std::cerr << "Invalid --workflow-admit-max-inflight-tasks-total\n";
        return 2;
      }
    } else if (a == "--workflow-enable-http-tasks") {
      cfg.workflow_enable_http_tasks = true;
      workflow_enable_http_tasks_set = true;
    } else if (a == "--workflow-http-allow-host") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-http-allow-host\n";
        return 2;
      }
      v = trim_copy(v);
      if (!v.empty()) cfg.workflow_http_allow_hosts.push_back(v);
    } else if (a == "--workflow-http-allow-cidr") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-http-allow-cidr\n";
        return 2;
      }
      v = trim_copy(v);
      if (!v.empty()) cfg.workflow_http_allow_cidrs.push_back(v);
    } else if (a == "--workflow-http-deny-private") {
      cfg.workflow_http_deny_private_addrs = true;
    } else if (a == "--memory-consolidate-interval-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-consolidate-interval-ms\n";
        return 2;
      }
      try {
        cfg.memory_consolidate_interval_ms = (int64_t)std::stoll(v);
        if (cfg.memory_consolidate_interval_ms < 0) cfg.memory_consolidate_interval_ms = 0;
      } catch (...) {
        std::cerr << "Invalid --memory-consolidate-interval-ms\n";
        return 2;
      }
    } else if (a == "--memory-consolidate-daily-days") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-consolidate-daily-days\n";
        return 2;
      }
      try {
        cfg.memory_consolidate_daily_days = (int)std::stol(v);
        if (cfg.memory_consolidate_daily_days < 0) cfg.memory_consolidate_daily_days = 0;
      } catch (...) {
        std::cerr << "Invalid --memory-consolidate-daily-days\n";
        return 2;
      }
    } else if (a == "--memory-consolidate-keep-checkpoints") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-consolidate-keep-checkpoints\n";
        return 2;
      }
      try {
        cfg.memory_consolidate_keep_checkpoints = (int)std::stol(v);
        if (cfg.memory_consolidate_keep_checkpoints < 1) cfg.memory_consolidate_keep_checkpoints = 1;
      } catch (...) {
        std::cerr << "Invalid --memory-consolidate-keep-checkpoints\n";
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
    } else if (a == "--yolo") {
      cfg.yolo_default = true;
    } else if (a == "--no-yolo") {
      cfg.yolo_default = false;
    } else if (a == "--no-default-system") {
      cfg.no_default_system = true;
    } else if (a == "--system-profile") {
      if (!take(&cfg.system_profile) || cfg.system_profile.empty()) {
        std::cerr << "Missing value for --system-profile\n";
        return 2;
      }
      cfg.system_profile = trim_copy(cfg.system_profile);
      if (!(cfg.system_profile == "default" || cfg.system_profile == "jules_codex")) {
        std::cerr << "Invalid --system-profile (expected: default|jules_codex)\n";
        return 2;
      }
      system_profile_set = true;
    } else if (a == "--tool-plugin") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-plugin\n";
        return 2;
      }
      tool_plugin_paths.push_back(v);
    } else if (a == "--tool-server-cmd") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-server-cmd\n";
        return 2;
      }
      ToolServerSpec s;
      s.cmd = v;
      tool_server_specs.push_back(std::move(s));
    } else if (a == "--tool-server-timeout-ms") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-server-timeout-ms\n";
        return 2;
      }
      if (tool_server_specs.empty()) {
        std::cerr << "--tool-server-timeout-ms must follow --tool-server-cmd\n";
        return 2;
      }
      try {
        int n = std::stoi(v);
        if (n < 1) n = 1;
        if (n > 300000) n = 300000;
        tool_server_specs.back().timeout_ms = n;
      } catch (...) {
        std::cerr << "Invalid --tool-server-timeout-ms\n";
        return 2;
      }
    } else if (a == "--tool-server-max-line-bytes") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-server-max-line-bytes\n";
        return 2;
      }
      if (tool_server_specs.empty()) {
        std::cerr << "--tool-server-max-line-bytes must follow --tool-server-cmd\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 1024) n = 1024;
        if (n > 64LL * 1024LL * 1024LL) n = 64LL * 1024LL * 1024LL;
        tool_server_specs.back().max_line_bytes = (size_t)n;
      } catch (...) {
        std::cerr << "Invalid --tool-server-max-line-bytes\n";
        return 2;
      }
    } else if (a == "--tool-server-ping-interval-ms") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-server-ping-interval-ms\n";
        return 2;
      }
      if (tool_server_specs.empty()) {
        std::cerr << "--tool-server-ping-interval-ms must follow --tool-server-cmd\n";
        return 2;
      }
      try {
        int n = std::stoi(v);
        if (n < 0) n = 0;
        if (n > 300000) n = 300000;
        tool_server_specs.back().ping_interval_ms = n;
      } catch (...) {
        std::cerr << "Invalid --tool-server-ping-interval-ms\n";
        return 2;
      }
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
        << "  --state-dir <dir>    Base state dir (default: daemon startup working directory; or env AGENT_WD)\n"
        << "  --sessions-root <dir> Session store root (default: <state-dir>)\n"
        << "  --db-path <path>     SQLite DB path (default: <state-dir>/agentd.db)\n"
        << "  --timeout-ms <n>     Provider HTTP timeout in ms (default: 60000)\n"
        << "  --job-ttl-ms <n>     GC finished jobs older than n ms (default: 1800000)\n"
        << "  --max-jobs <n>       Keep at most n jobs in memory (default: 256)\n"
        << "  --job-concurrency <n>       Max concurrent async jobs (default: 2)\n"
        << "  --job-poll-ms <n>           Job engine idle poll sleep ms (default: 200)\n"
        << "  --workflow-concurrency <n>  Max concurrent workflow tasks (default: 4)\n"
        << "  --workflow-poll-ms <n>      Workflow engine idle poll sleep ms (default: 200)\n"
        << "  --workflow-max-inflight-per-workflow <n>  Workflow fairness cap (default: 2)\n"
        << "  --workflow-max-inflight-per-session <n>   Optional multi-tenant cap; 0 disables (default: 0)\n"
        << "  --workflow-fair-queue-policy <scan_rr|wrr|drr>  Scheduler policy (default: wrr)\n"
        << "  --workflow-fair-queue-max-session-weight <n>  Clamp per-session weight (default: 16)\n"
        << "  --workflow-fair-queue-max-schedule-len <n>    Bound expanded WRR schedule length (default: 1024)\n"
        << "  --workflow-admit-max-inflight-tasks-per-session <n>  Admission control cap (queued|running tasks per session); 0 disables (default: 0)\n"
        << "  --workflow-admit-max-inflight-tasks-total <n>        Admission control cap (queued|running tasks total); 0 disables (default: 0)\n"
        << "  --workflow-enable-http-tasks   Enable deterministic workflow kind:\"http_json\" outbound HTTP (INSECURE by default; SSRF risk)\n"
        << "  --workflow-http-allow-host <host[:port]>   Optional allowlist for workflow outbound HTTP (repeatable; default: allow any host when enabled)\n"
        << "  --workflow-http-allow-cidr <cidr>   Optional CIDR allowlist for workflow outbound HTTP (repeatable; e.g. 10.0.0.0/8)\n"
        << "  --workflow-http-deny-private   Reject private/loopback/link-local outbound HTTP targets unless explicitly allowed (defense-in-depth)\n"
        << "  --memory-consolidate-interval-ms <n>   Run memory consolidation every n ms (default: 0=disabled)\n"
        << "  --memory-consolidate-daily-days <n>    Scan last n daily memory files for @mem markers (default: 14)\n"
        << "  --memory-consolidate-keep-checkpoints <n>  Retain at most n structured checkpoints (default: 100)\n"
        << "  --max-steps-default <n> Default tool-loop max steps when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --max-tool-calls-total-default <n> Default tool-loop total tool calls cap when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --max-tool-calls-per-tool-default <n> Default tool-loop per-tool call cap when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --tool-call-limit <tool>=<n> Default per-tool call limit (repeatable; 0 means unlimited for that tool)\n"
        << "  --tools host|basic|none   Default toolset (default: host)\n"
        << "  --host-policy full|readonly  Host tool safety policy (default: full)\n"
        << "  --yolo / --no-yolo   Default unrestricted mode (default: yolo)\n"
        << "  --no-default-system  Disable default host system hint (host tools only)\n"
        << "  --system-profile <name> Host system prompt profile (default: default)\n"
        << "  --tool-plugin <path> Load a tool plugin (repeatable)\n"
        << "  --tool-server-cmd <cmd> Start a stdio tool server (repeatable)\n"
        << "    --tool-server-timeout-ms <n>    Per-server RPC timeout (must follow --tool-server-cmd; default 30000; clamp 1..300000)\n"
        << "    --tool-server-max-line-bytes <n>  Per-server stdout line cap (must follow --tool-server-cmd; default 4MiB; clamp 1024..64MiB)\n"
        << "    --tool-server-ping-interval-ms <n>  Per-server idle ping interval (must follow --tool-server-cmd; default 0=disabled; clamp 0..300000)\n";
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
  if (!system_profile_set) {
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
  if (cfg.db_path.empty()) {
    if (const char* p = getenv_s("AGENTD_DB_PATH")) {
      cfg.db_path = p;
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
  if (!workflow_enable_http_tasks_set) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_ENABLE_HTTP_TASKS")) {
      cfg.workflow_enable_http_tasks = env_truthy(s);
    }
  }
  if (cfg.workflow_http_allow_hosts.empty()) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS")) {
      std::vector<std::string> toks;
      parse_csv_tokens_best_effort(s, &toks);
      for (auto& t : toks) {
        t = trim_copy(t);
        if (!t.empty()) cfg.workflow_http_allow_hosts.push_back(t);
      }
    }
  }
  if (cfg.workflow_http_allow_cidrs.empty()) {
    if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS")) {
      std::vector<std::string> toks;
      parse_csv_tokens_best_effort(s, &toks);
      for (auto& t : toks) {
        t = trim_copy(t);
        if (!t.empty()) cfg.workflow_http_allow_cidrs.push_back(t);
      }
    }
  }
  if (const char* s = getenv_s("AGENTD_WORKFLOW_HTTP_DENY_PRIVATE")) {
    cfg.workflow_http_deny_private_addrs = env_truthy(s);
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
    if (!load_runtime_config_best_effort(db, &cfg, &err)) {
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

  if (!tool_plugin_paths.empty()) {
    std::vector<ToolPluginSpec> specs;
    specs.reserve(tool_plugin_paths.size());
    for (const auto& p : tool_plugin_paths) {
      ToolPluginSpec s;
      s.path = p;
      specs.push_back(std::move(s));
    }
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

  if (!tool_server_specs.empty()) {
    std::vector<ToolServerSpec> specs = tool_server_specs;
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
    resp->body = R"({"ok":true,"service":"agentd","version":"0.1"})";
  });

  server.handle("GET", "/api/v1/config", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_config_endpoint(cur, cors_cfg, req, resp);
  });

  server.handle("POST", "/api/v1/config/update", [&](const HttpRequest& req, HttpResponse* resp) {
    handle_config_update_endpoint(&cfg_store, db_or_null, cors_cfg, req, resp);
  });

  server.handle("GET", "/api/v1/tools", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_tools_endpoint(cur, cors_cfg, cur.sessions_root_dir, tool_ext_or_null, req, resp);
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

  server.handle("POST", "/api/v1/memory/consolidate", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_consolidate_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/checkpoints", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_checkpoints_endpoint(cur, cors_cfg, req, resp);
  });
  server.handle("GET", "/api/v1/memory/correlate", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_memory_correlate_endpoint(cur, cors_cfg, req, resp);
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

  server.handle("POST", "/api/v1/run", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    const OpenAIClientConfig ocfg = ocfg_from_cfg(cur);
    handle_run_endpoint(cur, ocfg, cors_cfg, db_or_null, tool_ext_or_null, sessions_root_dir, req, resp);
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

  server.handle("GET", "/api/v1/workflow/events", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_workflow_events_endpoint(cur, cors_cfg, db_or_null, req, resp);
  });

  // Server-Sent Events stream for durable workflow progress. Streams `workflow_event` records and ends with `workflow_done`.
  server.handle_stream("GET", "/api/v1/workflow/stream", [&](const HttpRequest& req, int client_fd) {
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
  server.handle("GET", "/api/v1/edge/node/caps", [&](const HttpRequest& req, HttpResponse* resp) {
    const DaemonConfig cur = cfg_store.snapshot();
    handle_edge_node_caps_endpoint(cur, cors_cfg, db_or_null, req, resp);
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
  server.handle_stream("GET", "/api/v1/edge/workflow/stream", [&](const HttpRequest& req, int client_fd) {
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
  server.handle_stream("GET", "/api/v1/job/stream", [&](const HttpRequest& req, int client_fd) {
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
    wf_engine.stop();
    job_engine.stop();
    return 1;
  }
  edge_wf_engine.stop();
  edge_deadline_engine.stop();
  mem_engine.stop();
  wf_engine.stop();
  job_engine.stop();
  return 0;
}
