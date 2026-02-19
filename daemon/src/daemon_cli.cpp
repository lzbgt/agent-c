#include "daemon_cli.h"

#include "cors.h"
#include "daemon_config.h"
#include "http_util.h"
#include "policy_hooks.h"
#include "sandbox_policy.h"
#include "string_util.h"
#include "tool_plugins.h"
#include "tool_servers.h"

#include <json/json.h>

#include <iostream>
#include <memory>
#include <string>

namespace agentd {
namespace {

bool parse_tool_call_limit_spec(const std::string& spec, std::string* out_tool, size_t* out_max_calls) {
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

bool is_safe_tool_name(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 128) return false;
  for (const char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

void upsert_tool_call_limit(std::vector<std::pair<std::string, size_t>>* limits, std::string tool, size_t max_calls) {
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

}  // namespace

bool host_is_loopback(std::string host) {
  host = lower_copy(std::move(host));
  if (host == "localhost") return true;
  if (host == "::1" || host == "[::1]") return true;
  if (host.rfind("127.", 0) == 0) return true;
  if (host == "127.0.0.1") return true;
  return false;
}

bool parse_tool_call_limits_csv(
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

void parse_csv_tokens_best_effort(const std::string& csv, std::vector<std::string>* out) {
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

bool parse_cors_route_value(const Json::Value& root, CorsRouteConfig* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  if (!root.isObject()) {
    if (out_error) *out_error = "cors route must be a JSON object";
    return false;
  }
  CorsRouteConfig route;
  if (root.isMember("path_prefix")) {
    if (!root["path_prefix"].isString()) {
      if (out_error) *out_error = "cors route path_prefix must be a string";
      return false;
    }
    route.path_prefix = trim_copy(root["path_prefix"].asString());
  }
  if (!root.isMember("origins")) {
    if (out_error) *out_error = "cors route missing origins";
    return false;
  }
  const auto& origins = root["origins"];
  if (origins.isString()) {
    const std::string v = trim_copy(origins.asString());
    if (!v.empty()) route.origins.push_back(v);
  } else if (origins.isArray()) {
    for (const auto& o : origins) {
      if (!o.isString()) {
        if (out_error) *out_error = "cors route origins must be strings";
        return false;
      }
      const std::string v = trim_copy(o.asString());
      if (!v.empty()) route.origins.push_back(v);
    }
  } else {
    if (out_error) *out_error = "cors route origins must be string or array";
    return false;
  }
  *out = std::move(route);
  return true;
}

bool parse_cors_route_json(const std::string& raw, CorsRouteConfig* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  Json::CharReaderBuilder b;
  b["collectComments"] = false;
  std::string errs;
  Json::Value root;
  const auto* begin = raw.c_str();
  const auto* end = begin + raw.size();
  std::unique_ptr<Json::CharReader> reader(b.newCharReader());
  if (!reader->parse(begin, end, &root, &errs)) {
    if (out_error) *out_error = errs;
    return false;
  }
  return parse_cors_route_value(root, out, out_error);
}

int parse_daemon_cli(int argc, char** argv, DaemonConfig* cfg, DaemonCliOverrides* out) {
  if (!cfg || !out) return 2;
  *out = DaemonCliOverrides{};

  // Minimal flag parsing (daemon is host-only; core remains argv/env-free).
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    auto take = [&](std::string* outv) -> bool {
      if (i + 1 >= argc) return false;
      *outv = argv[++i];
      return true;
    };
    if (a == "--host") {
      if (!take(&cfg->listen_host)) {
        std::cerr << "Missing value for --host\n";
        return 2;
      }
    } else if (a == "--auth-token") {
      if (!take(&cfg->auth_token)) {
        std::cerr << "Missing value for --auth-token\n";
        return 2;
      }
    } else if (a == "--auth-cookie") {
      if (!take(&cfg->auth_cookie_name)) {
        std::cerr << "Missing value for --auth-cookie\n";
        return 2;
      }
    } else if (a == "--allow-unauth") {
      cfg->allow_unauthenticated_non_loopback = true;
    } else if (a == "--port") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --port\n";
        return 2;
      }
      try {
        const unsigned long p = std::stoul(v);
        cfg->listen_port = (uint16_t)p;
      } catch (...) {
        std::cerr << "Invalid --port\n";
        return 2;
      }
    } else if (a == "--model") {
      if (!take(&cfg->model)) {
        std::cerr << "Missing value for --model\n";
        return 2;
      }
    } else if (a == "--summary-model") {
      if (!take(&cfg->summary_model)) {
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
        const long n = std::stol(v);
        cfg->summary_max_chars = n < 0 ? 0 : (size_t)n;
      } catch (...) {
        std::cerr << "Invalid --summary-max-chars\n";
        return 2;
      }
    } else if (a == "--base-url") {
      if (!take(&cfg->base_url)) {
        std::cerr << "Missing value for --base-url\n";
        return 2;
      }
    } else if (a == "--api-key") {
      if (!take(&cfg->api_key)) {
        std::cerr << "Missing value for --api-key\n";
        return 2;
      }
    } else if (a == "--proxy") {
      if (!take(&cfg->proxy_url)) {
        std::cerr << "Missing value for --proxy\n";
        return 2;
      }
    } else if (a == "--state-dir") {
      if (!take(&cfg->state_dir)) {
        std::cerr << "Missing value for --state-dir\n";
        return 2;
      }
    } else if (a == "--sessions-root") {
      if (!take(&cfg->sessions_root_dir)) {
        std::cerr << "Missing value for --sessions-root\n";
        return 2;
      }
    } else if (a == "--file-session-realpath-strict") {
      cfg->file_session_realpath_strict = true;
    } else if (a == "--no-file-session-realpath-strict") {
      cfg->file_session_realpath_strict = false;
    } else if (a == "--upload-max-bytes") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --upload-max-bytes\n";
        return 2;
      }
      try {
        unsigned long long n = std::stoull(v);
        const unsigned long long kMax = 512ull * 1024ull * 1024ull;
        if (n > kMax) n = kMax;
        cfg->upload_max_bytes = (size_t)n;
        out->upload_max_bytes_set = true;
      } catch (...) {
        std::cerr << "Invalid --upload-max-bytes\n";
        return 2;
      }
    } else if (a == "--blob-store-mode") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-mode\n";
        return 2;
      }
      if (v != "local" && v != "object") {
        std::cerr << "Invalid --blob-store-mode (expected local|object)\n";
        return 2;
      }
      cfg->blob_store_mode = v;
      out->blob_store_set = true;
    } else if (a == "--blob-store-endpoint") {
      if (!take(&cfg->blob_store_endpoint)) {
        std::cerr << "Missing value for --blob-store-endpoint\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-store-region") {
      if (!take(&cfg->blob_store_region)) {
        std::cerr << "Missing value for --blob-store-region\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-store-bucket") {
      if (!take(&cfg->blob_store_bucket)) {
        std::cerr << "Missing value for --blob-store-bucket\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-store-prefix") {
      if (!take(&cfg->blob_store_prefix)) {
        std::cerr << "Missing value for --blob-store-prefix\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-store-path-style") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-path-style\n";
        return 2;
      }
      cfg->blob_store_path_style = string_to_bool(v);
      out->blob_store_set = true;
    } else if (a == "--blob-store-read-mode") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-read-mode\n";
        return 2;
      }
      if (v != "redirect" && v != "proxy") {
        std::cerr << "Invalid --blob-store-read-mode (expected redirect|proxy)\n";
        return 2;
      }
      cfg->blob_store_read_mode = v;
      out->blob_store_set = true;
    } else if (a == "--blob-store-cache-mode") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-cache-mode\n";
        return 2;
      }
      if (v != "none" && v != "read-through") {
        std::cerr << "Invalid --blob-store-cache-mode (expected none|read-through)\n";
        return 2;
      }
      cfg->blob_store_cache_mode = v;
      out->blob_store_set = true;
    } else if (a == "--blob-store-cache-max-bytes") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-cache-max-bytes\n";
        return 2;
      }
      try {
        unsigned long long n = std::stoull(v);
        const unsigned long long kMax = 512ull * 1024ull * 1024ull;
        if (n > kMax) n = kMax;
        cfg->blob_store_cache_max_bytes = (size_t)n;
        out->blob_store_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-store-cache-max-bytes\n";
        return 2;
      }
    } else if (a == "--blob-store-presign-ttl-sec") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-presign-ttl-sec\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 1) n = 1;
        if (n > 604800) n = 604800;
        cfg->blob_store_presign_ttl_sec = (int64_t)n;
        out->blob_store_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-store-presign-ttl-sec\n";
        return 2;
      }
    } else if (a == "--blob-store-timeout-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-store-timeout-ms\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        if (n > 30LL * 60 * 1000) n = 30LL * 60 * 1000;
        cfg->blob_store_timeout_ms = (int64_t)n;
        out->blob_store_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-store-timeout-ms\n";
        return 2;
      }
    } else if (a == "--blob-store-access-key") {
      if (!take(&cfg->blob_store_access_key)) {
        std::cerr << "Missing value for --blob-store-access-key\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-store-secret-key") {
      if (!take(&cfg->blob_store_secret_key)) {
        std::cerr << "Missing value for --blob-store-secret-key\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-store-session-token") {
      if (!take(&cfg->blob_store_session_token)) {
        std::cerr << "Missing value for --blob-store-session-token\n";
        return 2;
      }
      out->blob_store_set = true;
    } else if (a == "--blob-tier-local-max-bytes") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-tier-local-max-bytes\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        cfg->blob_tier_local_max_bytes = (int64_t)n;
        out->blob_tier_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-tier-local-max-bytes\n";
        return 2;
      }
    } else if (a == "--blob-tier-local-max-age-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-tier-local-max-age-ms\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        cfg->blob_tier_local_max_age_ms = (int64_t)n;
        out->blob_tier_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-tier-local-max-age-ms\n";
        return 2;
      }
    } else if (a == "--blob-tier-promote-after-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-tier-promote-after-ms\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        cfg->blob_tier_promote_after_ms = (int64_t)n;
        out->blob_tier_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-tier-promote-after-ms\n";
        return 2;
      }
    } else if (a == "--blob-tier-promote-max-bytes") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --blob-tier-promote-max-bytes\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        cfg->blob_tier_promote_max_bytes = (int64_t)n;
        out->blob_tier_set = true;
      } catch (...) {
        std::cerr << "Invalid --blob-tier-promote-max-bytes\n";
        return 2;
      }
    } else if (a == "--db-path") {
      if (!take(&cfg->db_path)) {
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
        cfg->timeout_ms = (long)std::stol(v);
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
        cfg->job_ttl_ms = (int64_t)std::stoll(v);
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
        cfg->max_jobs = (size_t)std::stoull(v);
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
        cfg->job_engine_max_concurrency = std::max(1, std::stoi(v));
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
        cfg->job_engine_poll_ms = std::max(1, std::stoi(v));
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
        cfg->workflow_engine_max_concurrency = std::max(1, std::stoi(v));
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
        cfg->workflow_engine_poll_ms = std::max(1, std::stoi(v));
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
        cfg->workflow_engine_max_inflight_per_workflow = std::max(1, std::stoi(v));
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
        cfg->workflow_engine_max_inflight_per_session = std::max(0, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --workflow-max-inflight-per-session\n";
        return 2;
      }
    } else if (a == "--workflow-fair-queue-policy") {
      if (!take(&cfg->workflow_engine_fair_queue_policy)) {
        std::cerr << "Missing value for --workflow-fair-queue-policy\n";
        return 2;
      }
    } else if (a == "--workflow-drr-cost-model") {
      if (!take(&cfg->workflow_engine_drr_cost_model)) {
        std::cerr << "Missing value for --workflow-drr-cost-model\n";
        return 2;
      }
    } else if (a == "--workflow-fair-queue-max-session-weight") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-fair-queue-max-session-weight\n";
        return 2;
      }
      try {
        cfg->workflow_engine_fair_queue_max_session_weight = std::max(1, std::stoi(v));
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
        cfg->workflow_engine_fair_queue_max_schedule_len = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --workflow-fair-queue-max-schedule-len\n";
        return 2;
      }
    } else if (a == "--workflow-admit-max-inflight-tasks-per-session") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-admit-max-inflight-tasks-per-session\n";
        return 2;
      }
      try {
        cfg->workflow_admit_max_inflight_tasks_per_session = std::max(0, std::stoi(v));
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
        cfg->workflow_admit_max_inflight_tasks_total = std::max(0, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --workflow-admit-max-inflight-tasks-total\n";
        return 2;
      }
    } else if (a == "--workflow-enable-http-tasks") {
      cfg->workflow_enable_http_tasks = true;
      out->workflow_enable_http_tasks_set = true;
    } else if (a == "--workflow-http-allow-host") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-http-allow-host\n";
        return 2;
      }
      if (!v.empty()) cfg->workflow_http_allow_hosts.push_back(v);
      out->workflow_http_allow_hosts_set = true;
    } else if (a == "--workflow-http-allow-cidr") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-http-allow-cidr\n";
        return 2;
      }
      if (!v.empty()) cfg->workflow_http_allow_cidrs.push_back(v);
      out->workflow_http_allow_cidrs_set = true;
    } else if (a == "--workflow-http-deny-cidr") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --workflow-http-deny-cidr\n";
        return 2;
      }
      if (!v.empty()) cfg->workflow_http_deny_cidrs.push_back(v);
      out->workflow_http_deny_cidrs_set = true;
    } else if (a == "--workflow-http-deny-private") {
      cfg->workflow_http_deny_private_addrs = true;
      out->workflow_http_deny_private_set = true;
    } else if (a == "--workflow-http-dns-pin") {
      cfg->workflow_http_dns_pin = true;
      out->workflow_http_dns_pin_set = true;
    } else if (a == "--memory-consolidate-interval-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-consolidate-interval-ms\n";
        return 2;
      }
      try {
        cfg->memory_consolidate_interval_ms = (int64_t)std::stoll(v);
        if (cfg->memory_consolidate_interval_ms < 0) cfg->memory_consolidate_interval_ms = 0;
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
        cfg->memory_consolidate_daily_days = std::max(0, std::stoi(v));
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
        cfg->memory_consolidate_keep_checkpoints = std::max(1, std::stoi(v));
      } catch (...) {
        std::cerr << "Invalid --memory-consolidate-keep-checkpoints\n";
        return 2;
      }
    } else if (a == "--memory-retention-interval-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-interval-ms\n";
        return 2;
      }
      try {
        cfg->memory_retention_interval_ms = (int64_t)std::stoll(v);
        if (cfg->memory_retention_interval_ms < 0) cfg->memory_retention_interval_ms = 0;
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-interval-ms\n";
        return 2;
      }
    } else if (a == "--memory-retention-daily-max-days") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-daily-max-days\n";
        return 2;
      }
      try {
        cfg->memory_retention_daily_max_days = std::max(0, std::stoi(v));
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-daily-max-days\n";
        return 2;
      }
    } else if (a == "--memory-retention-daily-max-bytes") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-daily-max-bytes\n";
        return 2;
      }
      try {
        cfg->memory_retention_daily_max_bytes = (int64_t)std::stoll(v);
        if (cfg->memory_retention_daily_max_bytes < 0) cfg->memory_retention_daily_max_bytes = 0;
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-daily-max-bytes\n";
        return 2;
      }
    } else if (a == "--memory-retention-checkpoint-max-days") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-checkpoint-max-days\n";
        return 2;
      }
      try {
        cfg->memory_retention_checkpoint_max_days = std::max(0, std::stoi(v));
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-checkpoint-max-days\n";
        return 2;
      }
    } else if (a == "--memory-retention-checkpoint-max-count") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-checkpoint-max-count\n";
        return 2;
      }
      try {
        cfg->memory_retention_checkpoint_max_count = std::max(0, std::stoi(v));
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-checkpoint-max-count\n";
        return 2;
      }
    } else if (a == "--memory-retention-structured-deprecate-days") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-structured-deprecate-days\n";
        return 2;
      }
      try {
        cfg->memory_retention_structured_deprecate_days = std::max(0, std::stoi(v));
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-structured-deprecate-days\n";
        return 2;
      }
    } else if (a == "--memory-retention-structured-deprecate-max-entries") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-retention-structured-deprecate-max-entries\n";
        return 2;
      }
      try {
        cfg->memory_retention_structured_deprecate_max_entries = std::max(0, std::stoi(v));
        out->memory_retention_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-retention-structured-deprecate-max-entries\n";
        return 2;
      }
    } else if (a == "--memory-salience-daily-days") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-salience-daily-days\n";
        return 2;
      }
      try {
        cfg->memory_salience_daily_days = std::max(0, std::stoi(v));
        out->memory_salience_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-salience-daily-days\n";
        return 2;
      }
    } else if (a == "--memory-salience-max-items") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-salience-max-items\n";
        return 2;
      }
      try {
        cfg->memory_salience_max_items = std::max(1, std::stoi(v));
        out->memory_salience_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-salience-max-items\n";
        return 2;
      }
    } else if (a == "--memory-salience-structured-max-items") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-salience-structured-max-items\n";
        return 2;
      }
      try {
        cfg->memory_salience_structured_max_items = std::max(0, std::stoi(v));
        out->memory_salience_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-salience-structured-max-items\n";
        return 2;
      }
    } else if (a == "--memory-salience-daily-max-items") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-salience-daily-max-items\n";
        return 2;
      }
      try {
        cfg->memory_salience_daily_max_items = std::max(0, std::stoi(v));
        out->memory_salience_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-salience-daily-max-items\n";
        return 2;
      }
    } else if (a == "--memory-salience-half-life-days") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-salience-half-life-days\n";
        return 2;
      }
      try {
        cfg->memory_salience_half_life_days = std::stod(v);
        if (cfg->memory_salience_half_life_days < 0) cfg->memory_salience_half_life_days = 0;
        out->memory_salience_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-salience-half-life-days\n";
        return 2;
      }
    } else if (a == "--memory-salience-importance-weight") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --memory-salience-importance-weight\n";
        return 2;
      }
      try {
        cfg->memory_salience_importance_weight = std::stod(v);
        if (cfg->memory_salience_importance_weight < 0) cfg->memory_salience_importance_weight = 0;
        out->memory_salience_set = true;
      } catch (...) {
        std::cerr << "Invalid --memory-salience-importance-weight\n";
        return 2;
      }
    } else if (a == "--ota-enable") {
      cfg->ota_enable = true;
    } else if (a == "--ota-command") {
      if (!take(&cfg->ota_command)) {
        std::cerr << "Missing value for --ota-command\n";
        return 2;
      }
    } else if (a == "--ota-command-timeout-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --ota-command-timeout-ms\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        cfg->ota_command_timeout_ms = (int64_t)n;
      } catch (...) {
        std::cerr << "Invalid --ota-command-timeout-ms\n";
        return 2;
      }
    } else if (a == "--ota-drain-timeout-ms") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --ota-drain-timeout-ms\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 0) n = 0;
        cfg->ota_drain_timeout_ms = (int64_t)n;
      } catch (...) {
        std::cerr << "Invalid --ota-drain-timeout-ms\n";
        return 2;
      }
    } else if (a == "--max-steps-default") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-steps-default\n";
        return 2;
      }
      try {
        cfg->max_steps_default = (size_t)std::stoull(v);
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
        cfg->max_tool_calls_total_default = (size_t)std::stoull(v);
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
        cfg->max_tool_calls_per_tool_default = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-tool-calls-per-tool-default\n";
        return 2;
      }
    } else if (a == "--max-tool-call-args-chars-default") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-tool-call-args-chars-default\n";
        return 2;
      }
      try {
        cfg->max_tool_call_args_chars_default = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-tool-call-args-chars-default\n";
        return 2;
      }
    } else if (a == "--max-tool-result-chars-default") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --max-tool-result-chars-default\n";
        return 2;
      }
      try {
        cfg->max_tool_result_chars_default = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --max-tool-result-chars-default\n";
        return 2;
      }
    } else if (a == "--policy-mode") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-mode\n";
        return 2;
      }
      PolicyMode pm = PolicyMode::Off;
      if (!policy_mode_from_string(v, &pm)) {
        std::cerr << "Invalid --policy-mode (expected: off|audit|enforce)\n";
        return 2;
      }
      cfg->policy_mode = policy_mode_to_string(pm);
    } else if (a == "--policy-tool-allow") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-tool-allow\n";
        return 2;
      }
      std::vector<std::string> items;
      parse_csv_tokens_best_effort(v, &items);
      for (const auto& item : items) {
        if (!is_safe_tool_name(item)) {
          std::cerr << "Invalid --policy-tool-allow (bad tool name: " << item << ")\n";
          return 2;
        }
        cfg->policy_tool_allowlist.push_back(item);
      }
    } else if (a == "--policy-tool-deny") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-tool-deny\n";
        return 2;
      }
      std::vector<std::string> items;
      parse_csv_tokens_best_effort(v, &items);
      for (const auto& item : items) {
        if (!is_safe_tool_name(item)) {
          std::cerr << "Invalid --policy-tool-deny (bad tool name: " << item << ")\n";
          return 2;
        }
        cfg->policy_tool_denylist.push_back(item);
      }
    } else if (a == "--policy-max-steps") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-max-steps\n";
        return 2;
      }
      try {
        cfg->policy_max_steps = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --policy-max-steps\n";
        return 2;
      }
    } else if (a == "--policy-max-tool-calls-total") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-max-tool-calls-total\n";
        return 2;
      }
      try {
        cfg->policy_max_tool_calls_total = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --policy-max-tool-calls-total\n";
        return 2;
      }
    } else if (a == "--policy-max-tool-calls-per-tool") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-max-tool-calls-per-tool\n";
        return 2;
      }
      try {
        cfg->policy_max_tool_calls_per_tool = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --policy-max-tool-calls-per-tool\n";
        return 2;
      }
    } else if (a == "--policy-max-tool-call-args-chars") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-max-tool-call-args-chars\n";
        return 2;
      }
      try {
        cfg->policy_max_tool_call_args_chars = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --policy-max-tool-call-args-chars\n";
        return 2;
      }
    } else if (a == "--policy-max-tool-result-chars") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --policy-max-tool-result-chars\n";
        return 2;
      }
      try {
        cfg->policy_max_tool_result_chars = (size_t)std::stoull(v);
      } catch (...) {
        std::cerr << "Invalid --policy-max-tool-result-chars\n";
        return 2;
      }
    } else if (a == "--tool-call-limit") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --tool-call-limit\n";
        return 2;
      }
      std::vector<std::pair<std::string, size_t>> limits;
      std::string err;
      if (!parse_tool_call_limits_csv(v, &limits, &err)) {
        std::cerr << "Invalid --tool-call-limit: " << err << "\n";
        return 2;
      }
      for (auto& p : limits) {
        upsert_tool_call_limit(&cfg->tool_call_limits_default, p.first, p.second);
      }
    } else if (a == "--tools") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --tools\n";
        return 2;
      }
      if (v == "none" || v == "basic" || v == "host") {
        cfg->tools = v;
      } else {
        std::cerr << "Invalid --tools (expected: host|basic|none)\n";
        return 2;
      }
    } else if (a == "--host-policy") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --host-policy\n";
        return 2;
      }
      HostToolsetPolicyMode p{};
      if (!host_policy_from_string(v, &p)) {
        std::cerr << "Invalid --host-policy (expected: full|readonly)\n";
        return 2;
      }
      cfg->host_policy = p;
    } else if (a == "--yolo") {
      cfg->yolo_default = true;
    } else if (a == "--no-yolo") {
      cfg->yolo_default = false;
    } else if (a == "--no-default-system") {
      cfg->no_default_system = true;
    } else if (a == "--system-profile") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --system-profile\n";
        return 2;
      }
      const std::string s = trim_copy(v);
      if (!(s == "default" || s == "jules_codex")) {
        std::cerr << "Invalid --system-profile (expected: default|jules_codex)\n";
        return 2;
      }
      cfg->system_profile = s;
      out->system_profile_set = true;
    } else if (a == "--tool-plugin") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-plugin\n";
        return 2;
      }
      ToolPluginSpec spec;
      spec.path = v;
      out->tool_plugin_specs.push_back(std::move(spec));
    } else if (a == "--tool-plugin-config") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-plugin-config\n";
        return 2;
      }
      if (out->tool_plugin_specs.empty()) {
        std::cerr << "--tool-plugin-config must follow --tool-plugin\n";
        return 2;
      }
      out->tool_plugin_specs.back().config_json = v;
    } else if (a == "--tool-server-cmd") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-server-cmd\n";
        return 2;
      }
      ToolServerSpec spec;
      spec.cmd = v;
      out->tool_server_specs.push_back(std::move(spec));
    } else if (a == "--tool-server-timeout-ms") {
      std::string v;
      if (!take(&v) || v.empty()) {
        std::cerr << "Missing value for --tool-server-timeout-ms\n";
        return 2;
      }
      if (out->tool_server_specs.empty()) {
        std::cerr << "--tool-server-timeout-ms must follow --tool-server-cmd\n";
        return 2;
      }
      try {
        int n = std::stoi(v);
        if (n < 1) n = 1;
        if (n > 300000) n = 300000;
        out->tool_server_specs.back().timeout_ms = n;
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
      if (out->tool_server_specs.empty()) {
        std::cerr << "--tool-server-max-line-bytes must follow --tool-server-cmd\n";
        return 2;
      }
      try {
        long long n = std::stoll(v);
        if (n < 1024) n = 1024;
        if (n > 64LL * 1024LL * 1024LL) n = 64LL * 1024LL * 1024LL;
        out->tool_server_specs.back().max_line_bytes = (size_t)n;
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
      if (out->tool_server_specs.empty()) {
        std::cerr << "--tool-server-ping-interval-ms must follow --tool-server-cmd\n";
        return 2;
      }
      try {
        int n = std::stoi(v);
        if (n < 0) n = 0;
        if (n > 300000) n = 300000;
        out->tool_server_specs.back().ping_interval_ms = n;
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
      cfg->cors_origins_set = true;
      cfg->cors_origins.push_back(v);
    } else if (a == "--cors-allow-headers") {
      if (!take(&cfg->cors_allow_headers)) {
        std::cerr << "Missing value for --cors-allow-headers\n";
        return 2;
      }
    } else if (a == "--cors-allow-methods") {
      if (!take(&cfg->cors_allow_methods)) {
        std::cerr << "Missing value for --cors-allow-methods\n";
        return 2;
      }
    } else if (a == "--cors-allow-credentials") {
      cfg->cors_allow_credentials = true;
      out->cors_allow_credentials_set = true;
    } else if (a == "--cors-max-age") {
      std::string v;
      if (!take(&v)) {
        std::cerr << "Missing value for --cors-max-age\n";
        return 2;
      }
      try {
        cfg->cors_max_age_seconds = std::max(0, std::stoi(v));
        out->cors_max_age_set = true;
      } catch (...) {
        std::cerr << "Invalid --cors-max-age\n";
        return 2;
      }
    } else if (a == "--no-cors") {
      cfg->cors_disabled = true;
      cfg->cors_origins_set = true;
      cfg->cors_origins.clear();
    } else if (a == "--cors-route") {
      std::string raw;
      if (!take(&raw) || raw.empty()) {
        std::cerr << "Missing value for --cors-route\n";
        return 2;
      }
      CorsRouteConfig route;
      std::string perr;
      if (!parse_cors_route_json(raw, &route, &perr)) {
        std::cerr << "Invalid --cors-route JSON: " << perr << "\n";
        return 2;
      }
      cfg->cors_routes.push_back(std::move(route));
      out->cors_routes_set = true;
    } else if (a == "--help" || a == "-h") {
      std::cerr
        << "Usage: agentd [options]\n"
        << "  --host <ip>          Listen host (default: 127.0.0.1)\n"
        << "  --auth-token <tok>   Require Authorization: Bearer <tok> (default: disabled)\n"
        << "  --auth-cookie <name> Accept auth token from cookie name (optional)\n"
        << "  --allow-unauth       Allow non-loopback without auth (INSECURE)\n"
        << "  --port <n>           Listen port (default: 8123)\n"
        << "  --cors-origin <origin|*>   Allowed browser Origin (repeatable; supports re:<regex>)\n"
        << "  --cors-allow-headers <csv> Allow headers (default includes Authorization, X-OpenRouter-Key)\n"
        << "  --cors-allow-methods <csv> Allow methods (default: GET, POST, DELETE, OPTIONS)\n"
        << "  --cors-allow-credentials    Allow cookies/credentials (default: false)\n"
        << "  --cors-max-age <n>         Preflight cache max-age seconds (default: 600)\n"
        << "  --cors-route <json>    Per-route CORS origin policy (repeatable; JSON {path_prefix, origins})\n"
        << "  --no-cors                  Disable CORS headers entirely\n"
        << "  --model <name>       Default model\n"
        << "  --summary-model <name>   Optional model for compaction summaries (tools=none)\n"
        << "  --summary-max-chars <n>  Max chars for inserted summary (default: 1200)\n"
        << "  --base-url <url>     Default base url\n"
        << "  --api-key <key>      Default API key (else env)\n"
        << "  --proxy <url>        Optional HTTP proxy override (else env HTTPS_PROXY/http_proxy)\n"
        << "  --state-dir <dir>    Base state dir (default: daemon startup working directory; or env AGENT_WD)\n"
        << "  --sessions-root <dir> Session store root (default: <state-dir>)\n"
        << "  --file-session-realpath-strict  Enforce session realpath confinement for /api/v1/file\n"
        << "  --no-file-session-realpath-strict  Allow symlink escapes for /api/v1/file\n"
        << "  --upload-max-bytes <n>  Per-file session upload limit (decoded bytes; 0 disables; default: 33554432)\n"
        << "  --blob-store-mode <local|object>  Blob storage mode (default: local)\n"
        << "  --blob-store-endpoint <url>  Object store endpoint (S3/MinIO)\n"
        << "  --blob-store-region <region>  Object store region (default: us-east-1)\n"
        << "  --blob-store-bucket <name>  Object store bucket name\n"
        << "  --blob-store-prefix <path>  Object store key prefix (default: blobs/sha256)\n"
        << "  --blob-store-path-style <bool>  Use path-style bucket URLs (default: true)\n"
        << "  --blob-store-read-mode <redirect|proxy>  Read mode for object tier (default: redirect)\n"
        << "  --blob-store-cache-mode <none|read-through>  Cache mode for object tier (default: read-through)\n"
        << "  --blob-store-cache-max-bytes <n>  Max bytes for proxy/read-through fetches (default: 33554432)\n"
        << "  --blob-store-presign-ttl-sec <n>  Presigned URL TTL seconds (default: 900)\n"
        << "  --blob-store-timeout-ms <n>  Object store HTTP timeout (default: 60000)\n"
        << "  --blob-store-access-key <key>  Object store access key (secret)\n"
        << "  --blob-store-secret-key <key>  Object store secret key (secret)\n"
        << "  --blob-store-session-token <token>  Optional session token (secret)\n"
        << "  --blob-tier-local-max-bytes <n>  Max local cache bytes for object tier (0 disables)\n"
        << "  --blob-tier-local-max-age-ms <n>  Evict object-tier cache older than this age (0 disables)\n"
        << "  --blob-tier-promote-after-ms <n>  Promote local blobs after this age (0 disables)\n"
        << "  --blob-tier-promote-max-bytes <n>  Per-blob promotion size cap (0 disables)\n"
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
        << "  --workflow-drr-cost-model <unit|simple_v1|telemetry_v1>  DRR cost model (default: unit)\n"
        << "  --workflow-fair-queue-max-session-weight <n>  Clamp per-session weight (default: 16)\n"
        << "  --workflow-fair-queue-max-schedule-len <n>    Bound expanded WRR schedule length (default: 1024)\n"
        << "  --workflow-admit-max-inflight-tasks-per-session <n>  Admission control cap (queued|running tasks per session); 0 disables (default: 0)\n"
        << "  --workflow-admit-max-inflight-tasks-total <n>        Admission control cap (queued|running tasks total); 0 disables (default: 0)\n"
        << "  --workflow-enable-http-tasks   Enable deterministic workflow kind:\"http_json\" outbound HTTP (INSECURE by default; SSRF risk)\n"
        << "  --workflow-http-allow-host <host[:port]>   Optional allowlist for workflow outbound HTTP (repeatable; default: allow any host when enabled)\n"
        << "  --workflow-http-allow-cidr <cidr>   Optional CIDR allowlist for workflow outbound HTTP (repeatable; e.g. 10.0.0.0/8)\n"
        << "  --workflow-http-deny-cidr <cidr>    Optional CIDR denylist for workflow outbound HTTP (repeatable; checked in addition to allowlist)\n"
        << "  --workflow-http-deny-private   Reject private/loopback/link-local outbound HTTP targets unless explicitly allowed (defense-in-depth)\n"
        << "  --workflow-http-dns-pin        Pin hostname DNS results per request (defense-in-depth vs DNS rebinding; may reduce DNS load balancing)\n"
        << "  --memory-consolidate-interval-ms <n>   Run memory consolidation every n ms (default: 0=disabled)\n"
        << "  --memory-consolidate-daily-days <n>    Scan last n daily memory files for @mem markers (default: 14)\n"
        << "  --memory-consolidate-keep-checkpoints <n>  Retain at most n structured checkpoints (default: 100)\n"
        << "  --memory-retention-interval-ms <n>    Run memory retention every n ms (default: 0=disabled)\n"
        << "  --memory-retention-daily-max-days <n> Keep at most n daily memory files (default: 0=disabled)\n"
        << "  --memory-retention-daily-max-bytes <n>  Cap daily memory bytes (default: 0=disabled)\n"
        << "  --memory-retention-checkpoint-max-days <n>  Delete checkpoints older than n days (default: 0=disabled)\n"
        << "  --memory-retention-checkpoint-max-count <n>  Keep at most n checkpoints (default: 0=disabled)\n"
        << "  --memory-retention-structured-deprecate-days <n>  Deprecate structured entries older than n days (default: 0=disabled)\n"
        << "  --memory-retention-structured-deprecate-max-entries <n>  Max entries to deprecate per run (default: 50)\n"
        << "  --memory-salience-daily-days <n>  Daily files to scan for salience (default: 7)\n"
        << "  --memory-salience-max-items <n>   Max total salience items (default: 12)\n"
        << "  --memory-salience-structured-max-items <n>  Max structured salience items (default: 6)\n"
        << "  --memory-salience-daily-max-items <n>  Max daily salience items (default: 6)\n"
        << "  --memory-salience-half-life-days <n>  Salience decay half-life in days (default: 14)\n"
        << "  --memory-salience-importance-weight <n>  Weight for @obs importance (default: 0.35)\n"
        << "  --ota-enable            Enable OTA update endpoint (default: disabled)\n"
        << "  --ota-command <cmd>     Command to apply OTA plan (see docs/spec/ota/agentd_ota_v0.md)\n"
        << "  --ota-command-timeout-ms <n>  OTA command timeout in ms (default: 300000; 0 disables)\n"
        << "  --ota-drain-timeout-ms <n>    Grace window before restart (default: 15000)\n"
        << "  --max-steps-default <n> Default tool-loop max steps when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --max-tool-calls-total-default <n> Default tool-loop total tool calls cap when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --max-tool-calls-per-tool-default <n> Default tool-loop per-tool call cap when requests omit it (default: 0; 0 means unlimited)\n"
        << "  --max-tool-call-args-chars-default <n> Default tool-loop tool call args JSON cap (default: 0; 0 means unlimited)\n"
        << "  --max-tool-result-chars-default <n> Default tool-loop tool result cap (default: 12000; 0 means unlimited)\n"
        << "  --policy-mode <off|audit|enforce> Policy hook mode (default: off)\n"
        << "  --policy-tool-allow <csv> Policy allowlist tool names (comma-separated, repeatable)\n"
        << "  --policy-tool-deny <csv> Policy denylist tool names (comma-separated, repeatable)\n"
        << "  --policy-max-steps <n> Policy max steps cap (default: 0; 0 means unlimited)\n"
        << "  --policy-max-tool-calls-total <n> Policy max total tool calls (default: 0; 0 means unlimited)\n"
        << "  --policy-max-tool-calls-per-tool <n> Policy max tool calls per tool (default: 0; 0 means unlimited)\n"
        << "  --policy-max-tool-call-args-chars <n> Policy max tool call args chars (default: 0; 0 means unlimited)\n"
        << "  --policy-max-tool-result-chars <n> Policy max tool result chars (default: 0; 0 means unlimited)\n"
        << "  --tool-call-limit <tool>=<n>[,<tool>=<n>...] Default per-tool call limit (repeatable; 0 means unlimited for that tool)\n"
        << "  --tools host|basic|none   Default toolset (default: host)\n"
        << "  --host-policy full|readonly  Host tool safety policy (default: full)\n"
        << "  --yolo / --no-yolo   Default unrestricted mode (default: yolo)\n"
        << "  --no-default-system  Disable default host system hint (host tools only)\n"
        << "  --system-profile <name> Host system prompt profile (default: default)\n"
        << "  --tool-plugin <path> Load a tool plugin (repeatable)\n"
        << "  --tool-plugin-config <json> Set config JSON for the most recent --tool-plugin\n"
        << "  --tool-server-cmd <cmd> Start a stdio tool server (repeatable)\n"
        << "    --tool-server-timeout-ms <n>    Per-server RPC timeout (must follow --tool-server-cmd; default 30000; clamp 1..300000)\n"
        << "    --tool-server-max-line-bytes <n>  Per-server stdout line cap (must follow --tool-server-cmd; default 4MiB; clamp 1024..64MiB)\n"
        << "    --tool-server-ping-interval-ms <n>  Per-server idle ping interval (must follow --tool-server-cmd; default 0=disabled; clamp 0..300000)\n";
      out->help_requested = true;
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      return 2;
    }
  }

  return 0;
}

}  // namespace agentd
