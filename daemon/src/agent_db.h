#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace agentd {

// Thin SQLite-backed troubleshooting store for agentd.
//
// This is intentionally a *mirror* of daemon activity (sessions/runs/events/tool_records),
// not the canonical persistence format (which remains `.sess` + `.events.jsonl`).
class AgentDb {
 public:
  AgentDb() = default;
  ~AgentDb();

  AgentDb(const AgentDb&) = delete;
  AgentDb& operator=(const AgentDb&) = delete;

  // Opens (and creates) the DB at the given path, runs migrations, and enables WAL mode.
  bool open(const std::string& path, std::string* out_error);
  void close();

  bool is_open() const { return db_ != nullptr; }
  std::string path() const { return path_; }

  // Generic key/value store (daemon-local; used for persisted runtime defaults and other small blobs).
  // Values are opaque UTF-8 strings (typically JSON).
  bool meta_get(const std::string& key, std::string* out_value, std::string* out_error);
  bool meta_set(const std::string& key, const std::string& value, std::string* out_error);

  // Session state (canonical).
  bool upsert_session(const std::string& session_id, int64_t now_unix_ms, std::string* out_error);
  bool session_exists(const std::string& session_id, bool* out_exists, std::string* out_error);
  bool replace_session_messages(
    const std::string& session_id,
    const std::vector<std::pair<std::string, std::string>>& role_and_content,
    int64_t now_unix_ms,
    std::string* out_error
  );
  bool load_session_messages(
    const std::string& session_id,
    std::vector<std::pair<std::string, std::string>>* out_role_and_content,
    std::string* out_error
  );
  bool list_sessions(std::vector<std::string>* out_session_ids_desc, std::string* out_error);
  bool delete_session(const std::string& session_id, std::string* out_error);

  // Run mirror.
  struct RunRow {
    std::string session_id;
    std::string job_id; // optional
    int64_t ts_unix_ms = 0;
    std::string prompt;
    std::string tools;
    std::string model;
    std::string base_url;
    bool stream_assistant = false;
    bool ok = false;
    // Best-effort stop reason:
    // - ok=true: typically "done"
    // - ok=false: typically the last error event's `reason` (e.g. max_steps_exceeded)
    std::string stop_reason;
    // Tool loop counters (best-effort; 0 for tools=none).
    int64_t steps_executed = 0;
    int64_t tool_calls_total = 0;
    // JSON object string mapping tool_name -> count (best-effort).
    std::string tool_calls_by_tool_json;
    // Convenience copy of the last error reason (when known).
    std::string last_error_reason;
    std::string error;
    long http_status = 0;
    std::string http_body;
  };

  // Inserts a run and returns the new run_id.
  bool insert_run(const RunRow& row, int64_t* out_run_id, std::string* out_error);
  bool insert_event(int64_t run_id, int64_t ts_unix_ms, const std::string& type, const std::string& data_json, std::string* out_error);
  bool insert_audit_record(
    const std::string& session_id,
    int64_t ts_unix_ms,
    int64_t run_id,
    const std::string& record_json,
    std::string* out_error
  );
  bool read_audit_records_tail(
    const std::string& session_id,
    size_t max_bytes,
    size_t max_records,
    std::vector<std::string>* out_record_json_desc,
    std::string* out_error
  );
  bool read_audit_records_by_trace_id(
    const std::string& trace_id,
    size_t max_bytes,
    size_t max_records,
    std::vector<std::string>* out_record_json_desc,
    std::string* out_error
  );

  struct ToolRecordRow {
    int64_t run_id = 0;
    std::string tool_name;
    std::string tool_call_id;
    std::string arguments_json;
    std::string result_text;
    std::string result_for_prompt_text;
    bool result_truncated_for_prompt = false;
  };
  bool insert_tool_record(const ToolRecordRow& row, std::string* out_error);

  struct ArtifactRow {
    int64_t run_id = 0;
    int64_t ts_unix_ms = 0;
    std::string session_id;
    std::string tool_call_id;
    std::string path;
    std::string kind;
    std::string mime;
    std::string title;
    bool autoplay = false;
    int repeat = 1;
    std::string artifact_json; // JSON object string (required; stable fallback for future schema changes)
  };
  bool insert_artifact(const ArtifactRow& row, std::string* out_error);
  bool list_artifacts_by_session(
    const std::string& session_id,
    size_t max_artifacts,
    std::vector<ArtifactRow>* out_rows_desc,
    std::string* out_error
  );

  struct UiActionRow {
    int64_t run_id = 0;
    int64_t ts_unix_ms = 0;
    std::string session_id;
    std::string tool_call_id;
    std::string type;
    std::string title;
    std::string message;
    std::string path;
    std::string mime;
    bool autoplay = false;
    int repeat = 1;
    std::string action_json; // JSON object string (required; stable fallback)
  };
  bool insert_ui_action(const UiActionRow& row, std::string* out_error);

  struct ClientEventRow {
    int64_t ts_unix_ms = 0;
    std::string session_id;
    std::string type;
    std::string data_json; // JSON object string (required; stable fallback)
  };
  bool insert_client_event(const ClientEventRow& row, std::string* out_error);
  bool read_client_events_tail_jsonl(
    const std::string& session_id,
    size_t max_bytes,
    size_t max_events,
    std::string* out_jsonl,
    std::string* out_error
  );

  // Durable Scene state (server-owned; used to re-render the WebUI Scene after refresh).
  // Stored as a JSON object string mapping entity_id -> entity object.
  //
  // The canonical update mechanism is "apply ops then upsert state" (see scene_store.*).
  bool get_scene_state(
    const std::string& session_id,
    std::string* out_scene_json,
    int64_t* out_updated_unix_ms,
    std::string* out_error
  );
  bool put_scene_state(
    const std::string& session_id,
    const std::string& scene_json,
    int64_t updated_unix_ms,
    std::string* out_error
  );

  // Job durability (async run lifecycle).
  struct JobRow {
    std::string job_id;
    std::string session_id; // best-effort; may be empty for no_session runs
    std::string trace_id;   // best-effort; used for correlation
    std::string request_json; // JSON object string (redacted; used for job resume)
    int64_t created_unix_ms = 0;
    int64_t updated_unix_ms = 0;
    std::string status; // queued|running|done|error|cancelled|interrupted
    bool cancel_requested = false;
    std::string error;
    std::string stop_reason; // best-effort; e.g. done|max_steps_exceeded|daemon_restart
    std::string result_json; // JSON object string; may be empty for non-terminal states
    int64_t last_heartbeat_unix_ms = 0;
  };

  // Upserts a job row by job_id. This is intended to be cheap and safe to call on every job transition.
  bool upsert_job(const JobRow& row, std::string* out_error);
  bool get_job(const std::string& job_id, JobRow* out_row, std::string* out_error);
  bool delete_job(const std::string& job_id, std::string* out_error);

  // On daemon restart, queued/running jobs cannot be resumed. Mark them as interrupted so UIs can be truthful.
  bool mark_inflight_jobs_interrupted(int64_t now_unix_ms, const std::string& reason, std::string* out_error);

  // Lists jobs by status (ordered by updated_unix_ms DESC).
  bool list_jobs_by_status(
    const std::string& status,
    size_t max_rows,
    std::vector<JobRow>* out_rows_desc,
    std::string* out_error
  );

  // Attempts to transition a queued job to running. Returns true only when the job was claimed.
  bool claim_job(const std::string& job_id, int64_t now_unix_ms, std::string* out_error);

  // Recovery on daemon startup: move running -> queued (resumable) and mark any inflight job missing request_json as interrupted.
  bool recover_inflight_jobs_resumable(int64_t now_unix_ms, std::string* out_error);

  // Durable workflows (task graphs / DAGs).
  //
  // Unlike async jobs, workflows are designed to be resumable after daemon restart. Inflight tasks are
  // recovered back to queued state (at-least-once execution semantics).
  struct WorkflowRow {
    std::string workflow_id;
    std::string session_id; // optional
    std::string trace_id;   // optional
    int64_t created_unix_ms = 0;
    int64_t updated_unix_ms = 0;
    std::string status; // queued|running|done|error|cancelled
    bool cancel_requested = false;
    std::string error;
    std::string spec_json;   // JSON object string (submit request, best-effort redacted)
    std::string result_json; // JSON object string (final aggregated results), optional
  };

  struct WorkflowTaskRow {
    std::string workflow_id;
    std::string task_id;
    int64_t created_unix_ms = 0;
    int64_t updated_unix_ms = 0;
    std::string status; // queued|running|done|error|cancelled
    int attempt = 0;
    int max_attempts = 1;
    int64_t ready_unix_ms = 0;   // do not start before this time (retry/backoff)
    int64_t started_unix_ms = 0; // best-effort
    int64_t finished_unix_ms = 0;
    std::string depends_on_json; // JSON array string of task_ids (optional)
    std::string request_json;    // JSON object string (run request)
    std::string expect_json;     // JSON object string (optional)
    std::string result_json;     // JSON object string (run response)
    std::string error;
  };

  // Inserts a workflow and its tasks in a single transaction.
  bool create_workflow(const WorkflowRow& wf, const std::vector<WorkflowTaskRow>& tasks, std::string* out_error);
  bool get_workflow(const std::string& workflow_id, WorkflowRow* out_row, std::string* out_error);
  bool list_workflows_by_status(
    const std::string& status,
    size_t max_rows,
    std::vector<WorkflowRow>* out_rows_desc,
    std::string* out_error
  );
  bool list_workflow_tasks(const std::string& workflow_id, std::vector<WorkflowTaskRow>* out_rows, std::string* out_error);

  // Upserts workflow/task metadata (cheap transition updates).
  bool upsert_workflow(const WorkflowRow& wf, std::string* out_error);
  bool upsert_workflow_task(const WorkflowTaskRow& task, std::string* out_error);

  // Attempts to transition a queued task to running. Returns true only when the task was claimed.
  bool claim_workflow_task(
    const std::string& workflow_id,
    const std::string& task_id,
    int64_t now_unix_ms,
    int new_attempt,
    std::string* out_error
  );

  // Recovery on daemon startup: move running workflows/tasks back to queued so they can resume.
  bool recover_inflight_workflows(int64_t now_unix_ms, std::string* out_error);

 private:
  bool ensure_schema_locked(std::string* out_error);
  bool exec_locked(const std::string& sql, std::string* out_error);
  bool upsert_session_locked(const std::string& session_id, int64_t now_unix_ms, std::string* out_error);

  sqlite3* db_ = nullptr;
  std::string path_;
  mutable std::mutex mu_;
};

}  // namespace agentd
