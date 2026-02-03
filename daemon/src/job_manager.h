#pragma once

#include <json/json.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace agentd {

struct JobState {
  std::string id;
  std::string status; // queued|running|done|error|cancelled
  std::string trace_id;
  bool cancel_requested = false;
  Json::Value result; // final JSON result (same shape as /api/v1/run)
  std::string error;
  // Live event stream (best-effort) captured while a job is running. This is used by the UI
  // to show progress instead of appearing to "hang" during long tool loops.
  Json::Value events = Json::Value(Json::arrayValue);
  uint64_t events_offset = 0; // number of events dropped from the front (cursor base)
  int64_t created_unix_ms = 0;
  int64_t updated_unix_ms = 0;
};

// Lightweight job snapshot returned to APIs/streamers.
//
// Important: job events can grow large (thousands). Avoid copying the entire `JobState` in hot paths:
// - the SSE stream loops frequently
// - the polling endpoint can be called frequently by UIs
//
// This snapshot includes:
// - metadata (status/error/result timestamps)
// - event cursor base/end
// - optionally a bounded slice of events starting at a cursor
struct JobSnapshot {
  std::string id;
  std::string status; // queued|running|done|error|cancelled
  std::string trace_id;
  bool cancel_requested = false;
  Json::Value result; // only meaningful for done/error
  std::string error;

  int64_t created_unix_ms = 0;
  int64_t updated_unix_ms = 0;

  uint64_t events_cursor_base = 0;
  uint64_t events_cursor_end = 0;

  // When include_events was requested:
  bool events_included = false;
  bool events_reset = false;        // true when requested cursor < cursor_base
  uint64_t events_cursor_start = 0; // effective cursor used for the slice (after clamping)
  uint64_t events_cursor_next = 0;  // cursor after the returned slice
  Json::Value events = Json::Value(Json::arrayValue);
};

enum JobProgressPhase {
  kPhaseIdle = 0,
  kPhaseWaitingLlm = 1,
  kPhaseRunningTool = 2,
};

struct DaemonJobEventHookCtx {
  std::string job_id;
  std::atomic<int64_t>* last_any_event_ms = nullptr;
  std::atomic<int64_t>* last_non_heartbeat_ms = nullptr;
  std::atomic<int>* phase = nullptr;
};

int64_t now_unix_ms();
std::string new_job_id();

void job_set_status(const std::string& id, const std::string& status, const std::string& error);
void job_append_event(const std::string& id, const std::string& type, const std::string& data_json);
void job_set_result(const std::string& id, const Json::Value& result);

bool job_get(const std::string& id, JobState* out);
bool job_create(const std::string& id);
bool job_delete(const std::string& id);
bool job_request_cancel(const std::string& id);
bool job_is_cancel_requested(const std::string& id);
void job_set_trace_id(const std::string& id, const std::string& trace_id);

// Gets a lightweight snapshot of a job. If include_events is true, returns at most max_events events starting
// from cursor (using absolute cursors based on events_offset).
bool job_get_snapshot(const std::string& id, uint64_t cursor, size_t max_events, bool include_events, JobSnapshot* out);

// Best-effort GC for long-running daemons.
// - Removes jobs with status done/error older than ttl_ms (based on updated_unix_ms).
// - If max_jobs > 0, prunes oldest done/error jobs until count <= max_jobs (never removes queued/running).
void job_gc(int64_t ttl_ms, size_t max_jobs);

// Hook for tool-loop / provider event capture into a running job's event stream.
void daemon_job_on_tool_loop_event(void* vctx, const char* type, const char* data_json);
void daemon_job_emit_heartbeat(
  const std::string& job_id,
  int phase,
  int64_t since_non_heartbeat_ms,
  int64_t since_any_event_ms
);

// SSE helpers used by /api/v1/job/stream.
bool sse_send(int fd, const std::string& event, const std::string& data_json, const std::string& id = "");
bool sse_ping(int fd);

// Low-level helper used by SSE endpoints to write HTTP/SSE wire bytes to a socket.
bool write_all_fd(int fd, const std::string& s);

}  // namespace agentd
