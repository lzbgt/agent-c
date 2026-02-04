#pragma once

#include <cstdint>
#include <map>
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
  // Optional integer fields in upsert calls use this sentinel to mean "do not modify existing value".
  // This value should never be produced by external API requests (we clamp request priorities to a small range).
  static constexpr int kIntUnset = (-2147483647 - 1);

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
    int priority = kIntUnset;  // higher runs sooner; unset => do not modify
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
    int priority = kIntUnset; // higher workflows run sooner; unset => do not modify
    int64_t deadline_unix_ms = 0; // optional scheduler-level deadline (0 disables)
    std::string idempotency_key;  // optional submit dedupe key (scoped by session_id)
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
    int priority = kIntUnset; // higher tasks run sooner; unset => do not modify
    int64_t created_unix_ms = 0;
    int64_t updated_unix_ms = 0;
    std::string status; // queued|running|done|error|cancelled
    bool allow_error = false; // if true, task status=error does not fail the workflow
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

  struct WorkflowEventRow {
    int64_t event_id = 0;
    std::string workflow_id;
    std::string task_id; // optional
    int64_t ts_unix_ms = 0;
    std::string type;      // workflow_status|task_status|...
    std::string data_json; // JSON object string (required; stable fallback)
  };

  // Inserts a workflow and its tasks in a single transaction.
  bool create_workflow(const WorkflowRow& wf, const std::vector<WorkflowTaskRow>& tasks, std::string* out_error);
  bool get_workflow(const std::string& workflow_id, WorkflowRow* out_row, std::string* out_error);
  bool get_workflow_by_idempotency_key(
    const std::string& session_id_or_empty,
    const std::string& idempotency_key,
    WorkflowRow* out_row,
    std::string* out_error
  );
  bool list_workflows_by_status(
    const std::string& status,
    size_t max_rows,
    std::vector<WorkflowRow>* out_rows_desc,
    std::string* out_error
  );
  bool list_workflow_tasks(const std::string& workflow_id, std::vector<WorkflowTaskRow>* out_rows, std::string* out_error);

  // Admission control helper: counts workflow tasks with status queued|running.
  bool count_workflow_inflight_tasks_total(int64_t* out_count, std::string* out_error);
  bool count_workflow_inflight_tasks_for_session(const std::string& session_id_or_empty, int64_t* out_count, std::string* out_error);

  struct WorkflowSchedulerStats {
    int64_t now_unix_ms = 0;
    std::map<std::string, int64_t> workflows_by_status;
    std::map<std::string, int64_t> tasks_by_status;
    int64_t tasks_queued_ready = 0;
    int64_t tasks_queued_not_ready = 0;
  };

  // Lightweight queue pressure / scheduling visibility helper.
  bool get_workflow_scheduler_stats(
    int64_t now_unix_ms,
    WorkflowSchedulerStats* out_stats,
    std::string* out_error
  );

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

  // Attempts to claim a queued workflow task while enforcing simple fairness/budget caps.
  // Caps are best-effort but deterministic within a single agentd process (AgentDb is mutex-locked).
  //
  // - max_inflight_per_workflow: max number of tasks with status='running' in the same workflow (<=0 disables)
  // - max_inflight_per_session: max number of tasks with status='running' across all workflows sharing the same session_id (<=0 disables)
  //
  // Returns true only when the task was claimed.
  bool claim_workflow_task_budgeted(
    const std::string& workflow_id,
    const std::string& task_id,
    int64_t now_unix_ms,
    int new_attempt,
    int max_inflight_per_workflow,
    int max_inflight_per_session,
    const std::string& session_id,
    std::string* out_error
  );

  // Recovery on daemon startup: move running workflows/tasks back to queued so they can resume.
  bool recover_inflight_workflows(int64_t now_unix_ms, std::string* out_error);

  // Workflow event log (durable; used for workflow SSE streaming and audit/debug).
  bool insert_workflow_event(const WorkflowEventRow& row, int64_t* out_event_id, std::string* out_error);
  bool list_workflow_events(
    const std::string& workflow_id,
    int64_t after_event_id,
    size_t max_rows,
    std::vector<WorkflowEventRow>* out_rows_asc,
    std::string* out_error
  );

  // --- Edge interop (UM‑EAIS v0.1) ---
  //
  // This is a transport-agnostic broker layer that persists:
  // - node registry (capability manifests + tags/tools)
  // - inbound UM‑BMP messages (dedupe by msg_id)
  // - outbound UM‑BMP messages (per-node outbox cursor)
  // - task state machine and events (TASK_ASSIGN/ACK/EVENT/DONE/FAILED)

  struct EdgeNodeRow {
    std::string node_id;
    std::string model;
    std::string fw_git_sha;
    std::string caps_sha256;
    std::string manifest_json;           // JSON object string (UM‑ACDS manifest), optional
    std::string tags_json;               // JSON array string of strings, optional
    std::string tools_json;              // JSON array string of strings, optional
    std::string hardware_presence_json;  // JSON object string, optional
    std::string health_json;             // JSON object string, optional
    int64_t last_hello_utc_ms = 0;
    int64_t last_heartbeat_utc_ms = 0;
  };

  bool upsert_edge_node(const EdgeNodeRow& row, std::string* out_error);
  bool get_edge_node(const std::string& node_id, EdgeNodeRow* out_row, std::string* out_error);
  bool list_edge_nodes(size_t max_rows, std::vector<EdgeNodeRow>* out_rows_desc, std::string* out_error);

  struct EdgeInboxMessageRow {
    std::string msg_id;
    int64_t ts_utc_ms = 0;
    std::string type;
    std::string from_id;
    std::string to_id;
    std::string envelope_json; // full envelope JSON object string
  };

  // Inserts an inbound message. Dedupe is by msg_id primary key:
  // - returns true when inserted
  // - returns true and sets out_deduped=true when msg_id already exists
  // - returns false only on unexpected failure
  bool insert_edge_inbox_message(const EdgeInboxMessageRow& row, bool* out_deduped, std::string* out_error);

  struct EdgeOutboxMessageRow {
    int64_t outbox_id = 0;
    std::string node_id;
    int64_t ts_utc_ms = 0;
    std::string envelope_json; // full envelope JSON object string
  };

  bool insert_edge_outbox_message(const EdgeOutboxMessageRow& row, int64_t* out_outbox_id, std::string* out_error);
  bool list_edge_outbox_messages(
    const std::string& node_id,
    int64_t after_outbox_id,
    size_t max_rows,
    std::vector<EdgeOutboxMessageRow>* out_rows_asc,
    std::string* out_error
  );

  struct EdgeTaskRow {
    std::string task_id;
    std::string step_id;
    std::string node_id;
    std::string idempotency_key;
    std::string mode; // invoke|agent
    std::string tool_name; // for mode=invoke (optional; best-effort)
    int64_t deadline_utc_ms = 0;
    std::string payload_json; // JSON object string (mode-specific)
    std::string state;        // QUEUED|RUNNING|SUCCEEDED|FAILED|TIMED_OUT|CANCELED
    int64_t created_utc_ms = 0;
    int64_t updated_utc_ms = 0;
    std::string result_json; // JSON object string (optional)
    std::string error;
  };

  bool upsert_edge_task(const EdgeTaskRow& row, std::string* out_error);
  bool get_edge_task(const std::string& task_id, const std::string& step_id, EdgeTaskRow* out_row, std::string* out_error);
  bool get_edge_task_by_node_idempotency(
    const std::string& node_id,
    const std::string& idempotency_key,
    EdgeTaskRow* out_row,
    std::string* out_error
  );
  bool list_edge_tasks_by_state(
    const std::string& state,
    size_t max_rows,
    std::vector<EdgeTaskRow>* out_rows_desc,
    std::string* out_error
  );

  bool list_edge_tasks_expired_deadline(
    int64_t now_utc_ms,
    size_t max_rows,
    std::vector<EdgeTaskRow>* out_rows_desc,
    std::string* out_error
  );

  struct EdgeTaskEventRow {
    int64_t id = 0;
    std::string task_id;
    std::string step_id;
    int64_t ts_utc_ms = 0;
    std::string state;
    std::string data_json; // JSON object string
  };

  bool insert_edge_task_event(const EdgeTaskEventRow& row, int64_t* out_id, std::string* out_error);

  struct EdgeSensorEventRow {
    int64_t id = 0;
    std::string node_id;
    std::string event_type;
    int64_t ts_utc_ms = 0;
    double confidence = 0.0; // optional; 0 when missing
    std::string data_json;   // JSON object string
  };

  bool insert_edge_sensor_event(const EdgeSensorEventRow& row, int64_t* out_id, std::string* out_error);

  struct EdgeToolRateStateRow {
    std::string node_id;
    std::string tool_name;
    int64_t window_start_utc_ms = 0;
    int window_count = 0;
    int64_t last_call_utc_ms = 0;
  };

  bool get_edge_tool_rate_state(
    const std::string& node_id,
    const std::string& tool_name,
    EdgeToolRateStateRow* out_row,
    std::string* out_error
  );
  bool upsert_edge_tool_rate_state(const EdgeToolRateStateRow& row, std::string* out_error);

  struct EdgeRuleRow {
    std::string rule_id;
    bool enabled = true;
    std::string event_type;
    double min_confidence = 0.0;
    int cooldown_ms = 0;
    int64_t last_fired_utc_ms = 0;
    std::string action_json; // JSON object string
    int64_t created_utc_ms = 0;
    int64_t updated_utc_ms = 0;
  };

  bool upsert_edge_rule(const EdgeRuleRow& row, std::string* out_error);
  bool get_edge_rule(const std::string& rule_id, EdgeRuleRow* out_row, std::string* out_error);
  bool delete_edge_rule(const std::string& rule_id, std::string* out_error);
  bool list_edge_rules(size_t max_rows, std::vector<EdgeRuleRow>* out_rows_desc, std::string* out_error);

  struct EdgeWorkflowRow {
    std::string workflow_id;
    std::string goal;
    std::string status; // QUEUED|RUNNING|SUCCEEDED|FAILED|CANCELED
    int priority = 0;
    std::string spec_json; // JSON object string
    int64_t created_utc_ms = 0;
    int64_t updated_utc_ms = 0;
    std::string error;
  };

  struct EdgeWorkflowStepRow {
    std::string workflow_id;
    std::string step_id;
    std::string kind;            // invoke_tool|run_agent|join
    std::string depends_on_json; // JSON array string
    std::string target_json;     // JSON object string
    std::string payload_json;    // JSON object string
    std::string join_mode;       // all|any (join only)
    int64_t deadline_utc_ms = 0;
    int attempt = 0;
    int max_attempts = 1;
    int64_t next_ready_utc_ms = 0;
    int backoff_ms = 0;
    std::string state; // PENDING|QUEUED|RUNNING|SUCCEEDED|FAILED|TIMED_OUT|CANCELED
    int64_t created_utc_ms = 0;
    int64_t updated_utc_ms = 0;
    std::string error;
  };

  struct EdgeWorkflowEventRow {
    int64_t id = 0;
    std::string workflow_id;
    int64_t ts_utc_ms = 0;
    std::string type;
    std::string data_json; // JSON object string
  };

  bool create_edge_workflow(
    const EdgeWorkflowRow& wf,
    const std::vector<EdgeWorkflowStepRow>& steps,
    std::string* out_error
  );
  bool get_edge_workflow(const std::string& workflow_id, EdgeWorkflowRow* out_row, std::string* out_error);
  bool upsert_edge_workflow(const EdgeWorkflowRow& wf, std::string* out_error);
  bool list_edge_workflows_by_status(
    const std::string& status,
    size_t max_rows,
    std::vector<EdgeWorkflowRow>* out_rows_desc,
    std::string* out_error
  );
  bool list_edge_workflow_steps(
    const std::string& workflow_id,
    std::vector<EdgeWorkflowStepRow>* out_rows,
    std::string* out_error
  );
  bool upsert_edge_workflow_step(const EdgeWorkflowStepRow& step, std::string* out_error);
  bool insert_edge_workflow_event(const EdgeWorkflowEventRow& row, int64_t* out_id, std::string* out_error);
  bool list_edge_workflow_events(
    const std::string& workflow_id,
    int64_t after_id,
    size_t max_rows,
    std::vector<EdgeWorkflowEventRow>* out_rows_asc,
    std::string* out_error
  );


 private:
  bool ensure_schema_locked(std::string* out_error);
  bool exec_locked(const std::string& sql, std::string* out_error);
  bool upsert_session_locked(const std::string& session_id, int64_t now_unix_ms, std::string* out_error);

  sqlite3* db_ = nullptr;
  std::string path_;
  mutable std::mutex mu_;
};

}  // namespace agentd
