#include "ota_endpoints.h"

#include "agent_db.h"
#include "daemon_auth.h"
#include "drain_state.h"
#include "json_util.h"
#include "string_util.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace agentd {
namespace {

struct OtaPlan {
  std::string ota_id;
  std::string version;
  std::string url;
  std::string sha256;
  std::string reason;
  std::string trace_id;
  std::string status; // queued | running | done | error
  std::string last_error;
  std::string plan_path;
  int64_t requested_unix_ms = 0;
  int64_t updated_unix_ms = 0;
  int64_t drain_timeout_ms = 0;
  int64_t command_timeout_ms = 0;
  int64_t agentd_pid = 0;
  std::string state_dir;
  std::string db_path;
};

struct OtaState {
  std::mutex mu;
  OtaPlan plan;
};

OtaState g_ota;
std::mutex g_env_mu;

int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
           .count();
}

int64_t current_pid() {
#if defined(_WIN32)
  return (int64_t)_getpid();
#else
  return (int64_t)getpid();
#endif
}

std::string plan_path_for_cfg(const DaemonConfig& cfg) {
  if (cfg.state_dir.empty()) return "";
  const std::filesystem::path p = std::filesystem::path(cfg.state_dir) / "ota" / "pending.json";
  return p.string();
}

bool is_hex_sha256(const std::string& s) {
  if (s.size() != 64) return false;
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') ||
      (c >= 'a' && c <= 'f') ||
      (c >= 'A' && c <= 'F');
    if (!ok) return false;
  }
  return true;
}

bool is_safe_url(const std::string& s_in) {
  const std::string s = trim_copy(s_in);
  if (s.empty() || s.size() > 4096) return false;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return false;
  }
  return true;
}

std::string new_ota_id() {
  const int64_t now = unix_ms_now();
  static std::atomic<uint64_t> counter{0};
  const uint64_t n = ++counter;
  return "ota_" + std::to_string((long long)now) + "_" + std::to_string((long long)n);
}

Json::Value plan_to_json(const OtaPlan& p) {
  Json::Value out(Json::objectValue);
  out["ota_id"] = p.ota_id;
  if (!p.version.empty()) out["version"] = p.version;
  out["url"] = p.url;
  if (!p.sha256.empty()) out["sha256"] = p.sha256;
  if (!p.reason.empty()) out["reason"] = p.reason;
  if (!p.trace_id.empty()) out["trace_id"] = p.trace_id;
  out["status"] = p.status;
  if (!p.last_error.empty()) out["last_error"] = p.last_error;
  out["requested_unix_ms"] = (Json::Int64)p.requested_unix_ms;
  out["updated_unix_ms"] = (Json::Int64)p.updated_unix_ms;
  out["drain_timeout_ms"] = (Json::Int64)p.drain_timeout_ms;
  out["command_timeout_ms"] = (Json::Int64)p.command_timeout_ms;
  if (p.agentd_pid > 0) out["agentd_pid"] = (Json::Int64)p.agentd_pid;
  if (!p.state_dir.empty()) out["state_dir"] = p.state_dir;
  if (!p.db_path.empty()) out["db_path"] = p.db_path;
  if (!p.plan_path.empty()) out["plan_path"] = p.plan_path;
  return out;
}

bool write_plan_file(const std::string& path, const OtaPlan& plan, std::string* out_err) {
  if (out_err) out_err->clear();
  if (path.empty()) {
    if (out_err) *out_err = "missing plan path";
    return false;
  }
  std::filesystem::path p(path);
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  if (ec) {
    if (out_err) *out_err = "failed to create ota directory";
    return false;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (out_err) *out_err = "failed to open plan file";
    return false;
  }
  const std::string body = json_stringify(plan_to_json(plan)) + "\n";
  out.write(body.data(), (std::streamsize)body.size());
  if (!out.good()) {
    if (out_err) *out_err = "failed to write plan file";
    return false;
  }
  return true;
}

bool read_plan_file(const std::string& path, OtaPlan* out_plan, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out_plan) return false;
  *out_plan = OtaPlan{};
  if (path.empty()) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::stringstream ss;
  ss << in.rdbuf();
  std::string content = ss.str();
  Json::Value v;
  std::string perr;
  if (!json_parse_object(content, &v, &perr)) {
    if (out_err) *out_err = perr;
    return false;
  }
  if (v.isMember("ota_id") && v["ota_id"].isString()) out_plan->ota_id = v["ota_id"].asString();
  if (v.isMember("version") && v["version"].isString()) out_plan->version = v["version"].asString();
  if (v.isMember("url") && v["url"].isString()) out_plan->url = v["url"].asString();
  if (v.isMember("sha256") && v["sha256"].isString()) out_plan->sha256 = v["sha256"].asString();
  if (v.isMember("reason") && v["reason"].isString()) out_plan->reason = v["reason"].asString();
  if (v.isMember("trace_id") && v["trace_id"].isString()) out_plan->trace_id = v["trace_id"].asString();
  if (v.isMember("status") && v["status"].isString()) out_plan->status = v["status"].asString();
  if (v.isMember("last_error") && v["last_error"].isString()) out_plan->last_error = v["last_error"].asString();
  if (v.isMember("requested_unix_ms") && v["requested_unix_ms"].isInt64()) out_plan->requested_unix_ms = v["requested_unix_ms"].asInt64();
  if (v.isMember("updated_unix_ms") && v["updated_unix_ms"].isInt64()) out_plan->updated_unix_ms = v["updated_unix_ms"].asInt64();
  if (v.isMember("drain_timeout_ms") && v["drain_timeout_ms"].isInt64()) out_plan->drain_timeout_ms = v["drain_timeout_ms"].asInt64();
  if (v.isMember("command_timeout_ms") && v["command_timeout_ms"].isInt64()) out_plan->command_timeout_ms = v["command_timeout_ms"].asInt64();
  if (v.isMember("agentd_pid") && v["agentd_pid"].isInt64()) out_plan->agentd_pid = v["agentd_pid"].asInt64();
  if (v.isMember("state_dir") && v["state_dir"].isString()) out_plan->state_dir = v["state_dir"].asString();
  if (v.isMember("db_path") && v["db_path"].isString()) out_plan->db_path = v["db_path"].asString();
  if (v.isMember("plan_path") && v["plan_path"].isString()) out_plan->plan_path = v["plan_path"].asString();
  return true;
}

struct EnvBackup {
  std::string key;
  std::string prev;
  bool had = false;
};

bool set_env_var(const std::string& key, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(key.c_str(), value.c_str()) == 0;
#else
  return setenv(key.c_str(), value.c_str(), 1) == 0;
#endif
}

void unset_env_var(const std::string& key) {
#if defined(_WIN32)
  _putenv_s(key.c_str(), "");
#else
  unsetenv(key.c_str());
#endif
}

class ScopedEnv {
 public:
  explicit ScopedEnv(const std::vector<std::pair<std::string, std::string>>& vars) {
    g_env_mu.lock();
    backups_.reserve(vars.size());
    for (const auto& kv : vars) {
      EnvBackup b;
      b.key = kv.first;
      const char* cur = std::getenv(kv.first.c_str());
      if (cur) {
        b.had = true;
        b.prev = cur;
      }
      backups_.push_back(b);
      set_env_var(kv.first, kv.second);
    }
  }
  ~ScopedEnv() {
    for (const auto& b : backups_) {
      if (b.had) set_env_var(b.key, b.prev);
      else unset_env_var(b.key);
    }
    g_env_mu.unlock();
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::vector<EnvBackup> backups_;
};

struct OtaInflightCounts {
  int64_t jobs_running = 0;
  int64_t jobs_queued = 0;
  int64_t workflows_running = 0;
  int64_t workflow_tasks_running = 0;
  int64_t workflow_tasks_queued = 0;
};

int64_t map_value(const std::map<std::string, int64_t>& m, const char* key) {
  auto it = m.find(key ? key : "");
  if (it == m.end()) return 0;
  return it->second;
}

bool load_inflight_counts(AgentDb* db_or_null, OtaInflightCounts* out) {
  if (!out) return false;
  *out = OtaInflightCounts{};
  if (!db_or_null || !db_or_null->is_open()) return false;
  AgentDb::JobStatusCounts jobs;
  std::string jerr;
  if (!db_or_null->get_job_status_counts(&jobs, &jerr)) return false;
  AgentDb::WorkflowSchedulerStats wf;
  std::string werr;
  if (!db_or_null->get_workflow_scheduler_stats(unix_ms_now(), &wf, &werr)) return false;
  out->jobs_running = map_value(jobs.by_status, "running");
  out->jobs_queued = map_value(jobs.by_status, "queued");
  out->workflows_running = map_value(wf.workflows_by_status, "running");
  out->workflow_tasks_running = map_value(wf.tasks_by_status, "running");
  out->workflow_tasks_queued = map_value(wf.tasks_by_status, "queued");
  return true;
}

void wait_for_inflight_or_timeout(AgentDb* db_or_null, int64_t drain_timeout_ms) {
  const int64_t cap = std::max<int64_t>(0, std::min<int64_t>(60LL * 60 * 1000, drain_timeout_ms));
  if (cap <= 0) return;
  if (!db_or_null || !db_or_null->is_open()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(cap));
    return;
  }
  const int64_t deadline = unix_ms_now() + cap;
  for (;;) {
    OtaInflightCounts counts;
    if (!load_inflight_counts(db_or_null, &counts)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(cap));
      return;
    }
    const int64_t running_total = counts.jobs_running + counts.workflow_tasks_running;
    if (running_total <= 0) break;
    const int64_t now = unix_ms_now();
    if (now >= deadline) break;
    const int64_t sleep_ms = std::min<int64_t>(250, deadline - now);
    if (sleep_ms <= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
}

bool run_ota_command(
  const std::string& cmd,
  const std::vector<std::pair<std::string, std::string>>& envs,
  int64_t timeout_ms,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (cmd.empty()) {
    if (out_err) *out_err = "missing ota command";
    return false;
  }

  auto prom = std::make_shared<std::promise<int>>();
  std::future<int> fut = prom->get_future();
  std::thread t([cmd, envs, prom]() {
    int rc = -1;
    try {
      ScopedEnv env(envs);
      rc = std::system(cmd.c_str());
    } catch (...) {
      rc = -1;
    }
    try {
      prom->set_value(rc);
    } catch (...) {
    }
  });

  if (timeout_ms > 0) {
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::timeout) {
      t.detach();
      if (out_err) *out_err = "ota command timed out";
      return false;
    }
  }
  int rc = -1;
  try {
    rc = fut.get();
  } catch (...) {
    rc = -1;
  }
  if (t.joinable()) t.join();
  if (rc != 0) {
    if (out_err) *out_err = "ota command failed";
    return false;
  }
  return true;
}

void run_ota_async(OtaPlan plan, DaemonConfig cfg, AgentDb* db_or_null) {
  const std::string plan_path = plan.plan_path;
  const int64_t now_ms = unix_ms_now();
  const int64_t drain_until = plan.drain_timeout_ms > 0 ? now_ms + plan.drain_timeout_ms : now_ms;
  const std::string drain_reason = plan.reason.empty() ? std::string("ota") : plan.reason;
  drain_begin(drain_until, drain_reason);
  {
    std::lock_guard<std::mutex> lk(g_ota.mu);
    g_ota.plan.status = "running";
    g_ota.plan.updated_unix_ms = unix_ms_now();
    plan.status = g_ota.plan.status;
    plan.updated_unix_ms = g_ota.plan.updated_unix_ms;
    (void)write_plan_file(plan_path, g_ota.plan, nullptr);
  }

  wait_for_inflight_or_timeout(db_or_null, plan.drain_timeout_ms);

  std::vector<std::pair<std::string, std::string>> envs;
  envs.push_back({"AGENTD_OTA_PLAN_PATH", plan_path});
  envs.push_back({"AGENTD_OTA_VERSION", plan.version});
  envs.push_back({"AGENTD_OTA_URL", plan.url});
  envs.push_back({"AGENTD_OTA_SHA256", plan.sha256});
  envs.push_back({"AGENTD_OTA_STATE_DIR", plan.state_dir});
  envs.push_back({"AGENTD_OTA_DB_PATH", plan.db_path});
  envs.push_back({"AGENTD_OTA_PID", std::to_string((long long)plan.agentd_pid)});

  std::string err;
  const bool ok = run_ota_command(cfg.ota_command, envs, plan.command_timeout_ms, &err);

  {
    std::lock_guard<std::mutex> lk(g_ota.mu);
    g_ota.plan.status = ok ? "done" : "error";
    g_ota.plan.updated_unix_ms = unix_ms_now();
    g_ota.plan.last_error = ok ? "" : (err.empty() ? "ota command failed" : err);
    (void)write_plan_file(plan_path, g_ota.plan, nullptr);
  }
  drain_end();
}

void write_json_error(HttpResponse* resp, int status, const std::string& msg) {
  if (!resp) return;
  resp->status = status;
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["error"] = msg;
  resp->body = json_stringify(o);
}

}  // namespace

void handle_ota_update_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp,
  AgentDb* db_or_null
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!cfg.ota_enable) {
    write_json_error(resp, 403, "ota disabled");
    return;
  }
  if (trim_copy(cfg.ota_command).empty()) {
    write_json_error(resp, 400, "ota command not configured");
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    write_json_error(resp, 400, std::string("invalid JSON: ") + perr);
    return;
  }

  const std::string url = args.isMember("url") && args["url"].isString() ? trim_copy(args["url"].asString()) : "";
  if (!is_safe_url(url)) {
    write_json_error(resp, 400, "missing/invalid url");
    return;
  }
  const std::string sha256 = args.isMember("sha256") && args["sha256"].isString() ? trim_copy(args["sha256"].asString()) : "";
  if (!sha256.empty() && !is_hex_sha256(sha256)) {
    write_json_error(resp, 400, "invalid sha256");
    return;
  }

  const std::string version = args.isMember("version") && args["version"].isString() ? trim_copy(args["version"].asString()) : "";
  const std::string reason = args.isMember("reason") && args["reason"].isString() ? trim_copy(args["reason"].asString()) : "";
  const std::string trace_id = args.isMember("trace_id") && args["trace_id"].isString() ? trim_copy(args["trace_id"].asString()) : "";

  int64_t drain_ms = cfg.ota_drain_timeout_ms;
  if (args.isMember("drain_timeout_ms") && args["drain_timeout_ms"].isInt64()) {
    drain_ms = args["drain_timeout_ms"].asInt64();
  } else if (args.isMember("drain_timeout_ms") && args["drain_timeout_ms"].isUInt64()) {
    drain_ms = (int64_t)args["drain_timeout_ms"].asUInt64();
  }
  if (drain_ms < 0) drain_ms = 0;
  if (drain_ms > 60LL * 60 * 1000) drain_ms = 60LL * 60 * 1000;

  const std::string plan_path = plan_path_for_cfg(cfg);
  if (plan_path.empty()) {
    write_json_error(resp, 500, "missing state_dir");
    return;
  }

  {
    std::lock_guard<std::mutex> lk(g_ota.mu);
    if (g_ota.plan.status == "queued" || g_ota.plan.status == "running") {
      write_json_error(resp, 409, "ota already running");
      return;
    }
  }

  OtaPlan plan;
  plan.ota_id = new_ota_id();
  plan.version = version;
  plan.url = url;
  plan.sha256 = sha256;
  plan.reason = reason;
  plan.trace_id = trace_id;
  plan.status = "queued";
  plan.requested_unix_ms = unix_ms_now();
  plan.updated_unix_ms = plan.requested_unix_ms;
  plan.drain_timeout_ms = drain_ms;
  plan.command_timeout_ms = cfg.ota_command_timeout_ms;
  plan.plan_path = plan_path;
  plan.state_dir = cfg.state_dir;
  plan.db_path = cfg.db_path;
  plan.agentd_pid = current_pid();

  std::string werr;
  if (!write_plan_file(plan_path, plan, &werr)) {
    write_json_error(resp, 500, werr.empty() ? "failed to write ota plan" : werr);
    return;
  }

  {
    std::lock_guard<std::mutex> lk(g_ota.mu);
    g_ota.plan = plan;
  }

  std::thread([plan, cfg, db_or_null]() mutable {
    run_ota_async(plan, cfg, db_or_null);
  }).detach();

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["ota_id"] = plan.ota_id;
  out["status"] = plan.status;
  resp->body = json_stringify(out);
}

void handle_ota_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp,
  AgentDb* db_or_null
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const std::string plan_path = plan_path_for_cfg(cfg);
  OtaPlan plan;
  std::string err;
  bool ok = read_plan_file(plan_path, &plan, &err);
  if (!ok) {
    plan = OtaPlan{};
    plan.status = "none";
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["status"] = plan.status.empty() ? "none" : plan.status;
  if (!plan.ota_id.empty()) out["ota_id"] = plan.ota_id;
  if (plan.updated_unix_ms > 0) out["updated_unix_ms"] = (Json::Int64)plan.updated_unix_ms;
  if (!plan.last_error.empty()) out["last_error"] = plan.last_error;
  if (!plan.plan_path.empty()) out["plan_path"] = plan.plan_path;
  out["drain_active"] = drain_is_active();
  const int64_t drain_until = drain_until_unix_ms();
  if (drain_until > 0) out["drain_until_unix_ms"] = (Json::Int64)drain_until;
  const std::string drain_reason_str = drain_reason();
  if (!drain_reason_str.empty()) out["drain_reason"] = drain_reason_str;
  {
    OtaInflightCounts counts;
    if (load_inflight_counts(db_or_null, &counts)) {
      out["jobs_running"] = (Json::Int64)counts.jobs_running;
      out["jobs_queued"] = (Json::Int64)counts.jobs_queued;
      out["workflows_running"] = (Json::Int64)counts.workflows_running;
      out["workflow_tasks_running"] = (Json::Int64)counts.workflow_tasks_running;
      out["workflow_tasks_queued"] = (Json::Int64)counts.workflow_tasks_queued;
    }
  }
  resp->body = json_stringify(out);
}

}  // namespace agentd
