#include "job_manager.h"

#include "json_util.h"

#include <chrono>
#include <cctype>
#include <map>
#include <mutex>
#include <vector>
#include <algorithm>
#include <sstream>
#include <thread>

#include "net_compat.h"

namespace agentd {

static std::mutex g_jobs_mu;
static std::map<std::string, JobState> g_jobs;
constexpr size_t kMaxJobEventDataBytes = 256 * 1024;

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

static const char* job_event_schema_for_type(const std::string& type) {
  if (type == "assistant_delta") return "run_event_payload_assistant_delta_v1";
  if (type == "assistant_message") return "run_event_payload_assistant_message_v1";
  if (type == "tool_call") return "run_event_payload_tool_call_v1";
  if (type == "tool_result") return "run_event_payload_tool_result_v1";
  if (type == "llm_usage") return "run_event_payload_llm_usage_v1";
  if (type == "artifact") return "run_event_payload_artifact_v1";
  if (type == "ui_action") return "run_event_payload_ui_action_v1";
  if (type == "heartbeat") return "run_event_payload_heartbeat_v1";
  if (type == "error") return "run_event_payload_error_v1";
  return nullptr;
}

static bool write_all_fd(socket_t fd, const char* data, size_t n) {
  size_t off = 0;
  while (off < n) {
    socket_io_t w = socket_write(fd, data + off, n - off);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w == kSocketError && socket_should_retry(socket_last_error())) {
      continue;
    }
    return false;
  }
  return true;
}

bool write_all_fd(socket_t fd, const std::string& s) {
  return write_all_fd(fd, s.data(), s.size());
}

bool sse_send(socket_t fd, const std::string& event, const std::string& data_json, const std::string& id) {
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

bool sse_ping(socket_t fd) {
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

  const size_t data_bytes = data_json.size();
  const bool truncated = data_bytes > kMaxJobEventDataBytes;
  std::string data_view = data_json;
  if (truncated) {
    data_view.resize(kMaxJobEventDataBytes);
  }

  Json::Value data;
  std::string perr;
  if (!data_view.empty() && !truncated && json_parse_any(data_view, &data, &perr)) {
    // ok
  } else {
    data = data_view;
  }

  Json::Value e(Json::objectValue);
  e["type"] = type;
  if (const char* schema = job_event_schema_for_type(type)) {
    e["schema"] = schema;
  }
  if (!it->second.trace_id.empty()) {
    e["trace_id"] = it->second.trace_id;
  }
  if (truncated) {
    e["data_truncated"] = true;
    e["data_bytes"] = (Json::Int64)data_bytes;
    e["data_bytes_kept"] = (Json::Int64)data_view.size();
  }
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
  const bool cancelled =
    result.isObject() &&
    ((result.isMember("cancelled") && result["cancelled"].isBool() && result["cancelled"].asBool()) ||
     (result.isMember("error") && result["error"].isString() && result["error"].asString() == "cancelled"));
  it->second.status = cancelled ? "cancelled" : (ok ? "done" : "error");
  if (cancelled) {
    it->second.error = "cancelled";
  } else if (!ok && result.isObject() && result.isMember("error") && result["error"].isString()) {
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

bool job_get_snapshot(const std::string& id, uint64_t cursor, size_t max_events, bool include_events, JobSnapshot* out) {
  if (!out) return false;
  *out = JobSnapshot{};
  if (max_events == 0) max_events = 256;
  if (max_events > 2048) max_events = 2048;

  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return false;
  const JobState& s = it->second;

  out->id = s.id;
  out->status = s.status;
  out->trace_id = s.trace_id;
  out->cancel_requested = s.cancel_requested;
  out->error = s.error;
  out->result = s.result;
  out->created_unix_ms = s.created_unix_ms;
  out->updated_unix_ms = s.updated_unix_ms;

  const uint64_t base = s.events_offset;
  const uint64_t end = base + (uint64_t)s.events.size();
  out->events_cursor_base = base;
  out->events_cursor_end = end;

  if (!include_events) {
    out->events_included = false;
    return true;
  }

  out->events_included = true;
  out->events = Json::Value(Json::arrayValue);

  bool reset = false;
  uint64_t cur = cursor;
  if (cur < base) {
    reset = true;
    cur = base;
  }
  if (cur > end) {
    cur = end;
  }
  out->events_reset = reset;
  out->events_cursor_start = cur;

  const uint64_t start_idx = cur - base;
  const uint64_t avail = end - cur;
  const uint64_t take = std::min<uint64_t>((uint64_t)max_events, avail);
  for (uint64_t i = 0; i < take; i++) {
    out->events.append(s.events[(Json::ArrayIndex)(start_idx + i)]);
  }
  out->events_cursor_next = cur + take;
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

void job_set_trace_id(const std::string& id, const std::string& trace_id) {
  std::lock_guard<std::mutex> lk(g_jobs_mu);
  auto it = g_jobs.find(id);
  if (it == g_jobs.end()) return;
  it->second.trace_id = trace_id;
  it->second.updated_unix_ms = now_unix_ms();
}

void job_gc(int64_t ttl_ms, size_t max_jobs) {
  const int64_t now = now_unix_ms();
  if (ttl_ms < 0) ttl_ms = 0;

  std::lock_guard<std::mutex> lk(g_jobs_mu);
  if (g_jobs.empty()) {
    return;
  }

  auto is_finished = [](const JobState& s) {
    return s.status == "done" || s.status == "error" || s.status == "cancelled" || s.status == "interrupted";
  };

  // TTL-based pruning.
  if (ttl_ms > 0) {
    for (auto it = g_jobs.begin(); it != g_jobs.end();) {
      const JobState& s = it->second;
      if (is_finished(s) && s.updated_unix_ms > 0 && (now - s.updated_unix_ms) > ttl_ms) {
        it = g_jobs.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Count-based pruning (keeps queued/running jobs regardless of max_jobs).
  if (max_jobs > 0 && g_jobs.size() > max_jobs) {
    struct Candidate {
      std::string id;
      int64_t updated_ms = 0;
    };
    std::vector<Candidate> finished;
    finished.reserve(g_jobs.size());
    for (const auto& kv : g_jobs) {
      const JobState& s = kv.second;
      if (!is_finished(s)) continue;
      finished.push_back(Candidate{kv.first, s.updated_unix_ms});
    }
    std::sort(finished.begin(), finished.end(), [](const Candidate& a, const Candidate& b) {
      return a.updated_ms < b.updated_ms;
    });

    // Remove oldest finished jobs first until within budget.
    size_t i = 0;
    while (g_jobs.size() > max_jobs && i < finished.size()) {
      g_jobs.erase(finished[i].id);
      i++;
    }
  }
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
