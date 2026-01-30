#include "job_manager.h"

#include <chrono>
#include <cctype>
#include <cerrno>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include <unistd.h>

namespace agentd {

static std::string json_stringify(const Json::Value& v) {
  Json::StreamWriterBuilder b;
  b["indentation"] = "";
  return Json::writeString(b, v);
}

static bool json_parse_any(const std::string& s, Json::Value* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(s);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs)) {
    if (out_err) *out_err = errs;
    return false;
  }
  *out = v;
  return true;
}

static std::mutex g_jobs_mu;
static std::map<std::string, JobState> g_jobs;

int64_t now_unix_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()
         ).count();
}

std::string new_job_id() {
  static std::atomic<uint64_t> counter{0};
  const uint64_t n = ++counter;
  return "job_" + std::to_string((long long)now_unix_ms()) + "_" + std::to_string((long long)n);
}

static bool write_all_fd(int fd, const char* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, data + off, n - off);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w == -1 && (errno == EINTR)) {
      continue;
    }
    return false;
  }
  return true;
}

bool write_all_fd(int fd, const std::string& s) {
  return write_all_fd(fd, s.data(), s.size());
}

bool sse_send(int fd, const std::string& event, const std::string& data_json, const std::string& id) {
  std::string out;
  out.reserve(event.size() + data_json.size() + 64);
  if (!event.empty()) {
    out += "event: ";
    out += event;
    out += "\n";
  }
  if (!id.empty()) {
    out += "id: ";
    out += id;
    out += "\n";
  }
  out += "data: ";
  out += data_json;
  out += "\n\n";
  return write_all_fd(fd, out);
}

bool sse_ping(int fd) {
  return write_all_fd(fd, ": ping\n\n");
}

void job_set_status(const std::string& id, const std::string& status, const std::string& error) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return;
  it->second.status = status;
  it->second.error = error;
  it->second.updated_unix_ms = now_unix_ms();
}

void job_append_event(const std::string& id, const std::string& type, const std::string& data_json) {
  constexpr Json::ArrayIndex kHardMax = 4096;
  constexpr Json::ArrayIndex kSoftMax = 4608; // rebuild window to avoid O(n) per event

  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return;

  Json::Value data;
  std::string perr;
  if (!data_json.empty() && json_parse_any(data_json, &data, &perr)) {
    // ok
  } else {
    data = data_json;
  }

  Json::Value e(Json::objectValue);
  e["type"] = type;
  e["data"] = data;
  it->second.events.append(e);
  it->second.updated_unix_ms = now_unix_ms();

  const Json::ArrayIndex sz = it->second.events.size();
  if (sz > kSoftMax) {
    // Keep the last kHardMax events.
    const Json::ArrayIndex start = (sz > kHardMax) ? (sz - kHardMax) : 0;
    Json::Value trimmed(Json::arrayValue);
    for (Json::ArrayIndex i = start; i < sz; i++) {
      trimmed.append(it->second.events[i]);
    }
    it->second.events_offset += (uint64_t)start;
    it->second.events = std::move(trimmed);
  }
}

void job_set_result(const std::string& id, const Json::Value& result) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return;
  it->second.result = result;
  const bool ok = result.isObject() && result.isMember("ok") && result["ok"].isBool() && result["ok"].asBool();
  it->second.status = ok ? "done" : "error";
  if (!ok && result.isObject() && result.isMember("error") && result["error"].isString()) {
    it->second.error = result["error"].asString();
  }
  it->second.updated_unix_ms = now_unix_ms();
}

bool job_get(const std::string& id, JobState* out) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool job_create(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  if (g_jobs.find(id) != g_jobs.end()) return false;
  JobState s;
  s.id = id;
  s.status = "queued";
  s.created_unix_ms = now_unix_ms();
  s.updated_unix_ms = s.created_unix_ms;
  s.events = Json::Value(Json::arrayValue);
  s.events_offset = 0;
  g_jobs[id] = s;
  return true;
}

bool job_delete(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return false;
  // Only allow deletion when not running.
  if (it->second.status == "running" || it->second.status == "queued") {
    return false;
  }
  g_jobs.erase(it);
  return true;
}

bool job_request_cancel(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return false;
  if (it->second.status != "running" && it->second.status != "queued") {
    return false;
  }
  it->second.cancel_requested = true;
  it->second.updated_unix_ms = now_unix_ms();
  return true;
}

bool job_is_cancel_requested(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return false;
  return it->second.cancel_requested;
}

void daemon_job_on_tool_loop_event(void* vctx, const char* type, const char* data_json) {
  if (!vctx || !type) return;
  auto* ctx = static_cast<DaemonJobEventHookCtx*>(vctx);
  const int64_t now = now_unix_ms();
  if (ctx->last_any_event_ms) ctx->last_any_event_ms->store(now);
  if (ctx->last_non_heartbeat_ms) ctx->last_non_heartbeat_ms->store(now);
  if (ctx->phase) {
    const std::string t(type);
    if (t == "llm_request") ctx->phase->store(kPhaseWaitingLlm);
    else if (t == "llm_response") ctx->phase->store(kPhaseIdle);
    else if (t == "tool_call") ctx->phase->store(kPhaseRunningTool);
    else if (t == "tool_result") ctx->phase->store(kPhaseIdle);
  }
  job_append_event(ctx->job_id, type, data_json ? data_json : "");
}

void daemon_job_emit_heartbeat(
  const std::string& job_id,
  int phase,
  int64_t since_non_heartbeat_ms,
  int64_t since_any_event_ms
) {
  Json::Value d(Json::objectValue);
  d["job_id"] = job_id;
  d["ts_unix_ms"] = (Json::Int64)now_unix_ms();
  d["phase"] = phase;
  d["since_last_non_heartbeat_ms"] = (Json::Int64)since_non_heartbeat_ms;
  d["since_last_any_event_ms"] = (Json::Int64)since_any_event_ms;
  job_append_event(job_id, "heartbeat", json_stringify(d));
}

}  // namespace agentd
