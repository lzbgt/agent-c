#include "job_engine.h"

#include "drain_state.h"
#include "job_manager.h"
#include "json_util.h"
#include "run_endpoints.h"
#include "string_util.h"

#include <json/json.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <unordered_set>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool is_terminal_status(const std::string& s) {
  return s == "done" || s == "error" || s == "cancelled" || s == "interrupted";
}

static std::string sanitize_job_request_for_resume(const std::string& raw_json) {
  if (raw_json.empty()) return "{}";
  Json::Value v;
  std::string perr;
  if (!json_parse_object(raw_json, &v, &perr) || !v.isObject()) {
    return "{}";
  }

  // Do not persist inline secrets. Resumed jobs should use daemon-configured keys.
  if (v.isMember("api_key")) v.removeMember("api_key");
  if (v.isMember("Authorization")) v.removeMember("Authorization");
  if (v.isMember("auth_token")) v.removeMember("auth_token");
  // Trace buffers can be large.
  if (v.isMember("trace_text")) v.removeMember("trace_text");
  if (v.isMember("http_body")) v.removeMember("http_body");

  return json_stringify_compact(v);
}

static std::string extract_trace_id_best_effort(const std::string& request_json) {
  Json::Value v;
  std::string perr;
  if (!json_parse_object(request_json, &v, &perr) || !v.isObject()) return "";
  if (v.isMember("trace_id") && v["trace_id"].isString()) return trim_copy(v["trace_id"].asString());
  return "";
}

}  // namespace

JobEngine::JobEngine(
  AgentDb* db,
  std::function<DaemonConfig()> cfg_snapshot,
  std::function<OpenAIClientConfig(const DaemonConfig&)> ocfg_from_cfg,
  const ToolExtension* tool_ext_or_null,
  std::string sessions_root_dir,
  Options opt
) : db_(db),
    cfg_snapshot_(std::move(cfg_snapshot)),
    ocfg_from_cfg_(std::move(ocfg_from_cfg)),
    tool_ext_or_null_(tool_ext_or_null),
    sessions_root_dir_(std::move(sessions_root_dir)),
    opt_(opt) {
  if (opt_.max_concurrency <= 0) opt_.max_concurrency = 1;
  if (opt_.max_concurrency > 16) opt_.max_concurrency = 16;
  if (opt_.poll_ms <= 0) opt_.poll_ms = 50;
  if (opt_.poll_ms > 5000) opt_.poll_ms = 5000;
  if (opt_.max_scan_jobs == 0) opt_.max_scan_jobs = 16;
}

JobEngine::~JobEngine() {
  stop();
}

void JobEngine::recover_inflight_jobs_best_effort() {
  if (!db_ || !db_->is_open()) return;
  std::string err;
  (void)db_->recover_inflight_jobs_resumable(unix_ms_now(), &err);
}

bool JobEngine::start(std::string* out_error) {
  if (out_error) out_error->clear();
  if (running_.load()) return true;
  if (!db_ || !db_->is_open()) {
    if (out_error) *out_error = "job engine requires an open AgentDb";
    return false;
  }
  if (!cfg_snapshot_ || !ocfg_from_cfg_) {
    if (out_error) *out_error = "job engine missing cfg snapshot function(s)";
    return false;
  }

  recover_inflight_jobs_best_effort();

  stop_.store(false);
  running_.store(true);
  workers_.clear();
  workers_.reserve((size_t)opt_.max_concurrency);
  for (int i = 0; i < opt_.max_concurrency; i++) {
    workers_.emplace_back([this]() { worker_main(); });
  }
  return true;
}

void JobEngine::stop() {
  stop_.store(true);
  if (!running_.load()) return;
  for (auto& th : workers_) {
    if (th.joinable()) th.join();
  }
  workers_.clear();
  running_.store(false);
}

void JobEngine::worker_main() {
  while (!stop_.load()) {
    if (drain_is_active()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
      continue;
    }
    const int64_t now = unix_ms_now();
    AgentDb::JobRow job;
    if (claim_one(now, &job)) {
      execute_job(job);
      continue;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(opt_.poll_ms));
  }
}

bool JobEngine::claim_one(int64_t now_unix_ms, AgentDb::JobRow* out_job) {
  if (!out_job) return false;
  *out_job = AgentDb::JobRow{};
  if (!db_ || !db_->is_open()) return false;

  std::vector<AgentDb::JobRow> queued;
  std::string err;
  if (!db_->list_jobs_by_status("queued", opt_.max_scan_jobs, &queued, &err)) {
    return false;
  }

  for (auto& j : queued) {
    if (stop_.load()) return false;
    if (j.job_id.empty()) continue;
    if (is_terminal_status(j.status)) continue;

    // If cancellation was requested while queued, finalize immediately.
    if (j.cancel_requested) {
      j.status = "cancelled";
      j.updated_unix_ms = now_unix_ms;
      j.error = j.error.empty() ? "cancelled" : j.error;
      j.stop_reason = j.stop_reason.empty() ? "cancelled" : j.stop_reason;
      (void)db_->upsert_job(j, nullptr);
      continue;
    }

    if (j.request_json.empty()) {
      j.status = "interrupted";
      j.updated_unix_ms = now_unix_ms;
      j.error = j.error.empty() ? "missing request_json; cannot resume" : j.error;
      j.stop_reason = j.stop_reason.empty() ? "missing_request_json" : j.stop_reason;
      (void)db_->upsert_job(j, nullptr);
      continue;
    }

    if (!db_->claim_job(j.job_id, now_unix_ms, &err)) {
      continue;
    }

    AgentDb::JobRow fresh;
    if (!db_->get_job(j.job_id, &fresh, &err)) {
      fresh = j;
      fresh.status = "running";
      fresh.updated_unix_ms = now_unix_ms;
    }
    *out_job = std::move(fresh);
    return true;
  }

  return false;
}

void JobEngine::execute_job(const AgentDb::JobRow& job) {
  if (!db_ || !db_->is_open()) return;
  if (job.job_id.empty() || job.request_json.empty()) return;

  const std::string job_id = job.job_id;
  const int64_t now = unix_ms_now();

  // Ensure the in-memory job exists so UIs can stream progress during resumed execution.
  if (!job_create(job_id)) {
    // If already present, keep it.
  }

  std::string trace_id = job.trace_id;
  if (trace_id.empty()) trace_id = extract_trace_id_best_effort(job.request_json);
  if (!trace_id.empty()) {
    job_set_trace_id(job_id, trace_id);
    AgentDb::JobRow tr;
    tr.job_id = job_id;
    tr.trace_id = trace_id;
    tr.updated_unix_ms = now;
    (void)db_->upsert_job(tr, nullptr);
  }

  job_set_status(job_id, "running", "");
  {
    Json::Value d(Json::objectValue);
    d["source"] = "job_engine";
    d["job_id"] = job_id;
    if (!trace_id.empty()) d["trace_id"] = trace_id;
    d["status"] = "running";
    d["ts_unix_ms"] = (Json::Int64)now;
    job_append_event(job_id, "start", json_stringify_compact(d));
  }

  // Execute using the same run pipeline.
  const DaemonConfig cfg = cfg_snapshot_();
  const OpenAIClientConfig ocfg = ocfg_from_cfg_(cfg);
  const std::string req_body = sanitize_job_request_for_resume(job.request_json);
  Json::Value out = run_request_to_json_internal(cfg, ocfg, db_, tool_ext_or_null_, sessions_root_dir_, req_body, job_id.c_str());

  job_set_result(job_id, out);

  // Persist terminal status/result.
  const bool ok = out.isObject() && out.isMember("ok") && out["ok"].isBool() && out["ok"].asBool();
  const bool cancelled =
    out.isObject() &&
    ((out.isMember("cancelled") && out["cancelled"].isBool() && out["cancelled"].asBool()) ||
     (out.isMember("error") && out["error"].isString() && out["error"].asString() == "cancelled"));
  const std::string status = cancelled ? "cancelled" : (ok ? "done" : "error");
  std::string error;
  if (!ok && out.isObject() && out.isMember("error") && out["error"].isString()) error = out["error"].asString();
  if (cancelled) error = "cancelled";

  std::string stop_reason = ok ? "done" : "error";
  std::string last_err_reason;
  if (out.isObject() && out.isMember("events") && out["events"].isArray()) {
    for (Json::ArrayIndex i = 0; i < out["events"].size(); i++) {
      const auto& ev = out["events"][i];
      if (!ev.isObject()) continue;
      const auto& t = ev["type"];
      const auto& d = ev["data"];
      if (!t.isString() || !d.isObject()) continue;
      if (t.asString() == "error" && d.isMember("reason") && d["reason"].isString()) {
        last_err_reason = d["reason"].asString();
      }
      if (t.asString() == "cancelled") {
        if (d.isMember("reason") && d["reason"].isString()) stop_reason = d["reason"].asString();
        else stop_reason = "cancelled";
      }
    }
  }
  if (!ok && !last_err_reason.empty()) stop_reason = last_err_reason;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  AgentDb::JobRow jr;
  jr.job_id = job_id;
  jr.session_id = job.session_id;
  jr.trace_id = trace_id;
  jr.request_json = req_body; // keep the redacted request for future resume/debug
  jr.created_unix_ms = job.created_unix_ms > 0 ? job.created_unix_ms : now;
  jr.updated_unix_ms = unix_ms_now();
  jr.status = status;
  jr.cancel_requested = false;
  jr.error = error;
  jr.stop_reason = stop_reason;
  jr.result_json = Json::writeString(wb, out);
  jr.last_heartbeat_unix_ms = 0;
  (void)db_->upsert_job(jr, nullptr);
}

}  // namespace agentd
