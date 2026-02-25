#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <cstring>
#include <filesystem>
#include <functional>
#include <sstream>

namespace agentd {

AgentDb::~AgentDb() {
  close();
}

bool AgentDb::open(const std::string& path, std::string* out_error) {
  if (out_error) out_error->clear();
  close();
  if (path.empty()) {
    if (out_error) *out_error = "db path is empty";
    return false;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  (void)path;
  return false;
#else
  {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        if (out_error) *out_error = std::string("failed to create db directory: ") + ec.message();
        return false;
      }
    }
  }

  sqlite3* db = nullptr;
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db);
    if (db) sqlite3_close(db);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    db_ = db;
    path_ = path;
    if (!ensure_schema_locked(out_error)) {
      sqlite3_close(db_);
      db_ = nullptr;
      path_.clear();
      return false;
    }
  }

  return true;
#endif
}

void AgentDb::close() {
  std::lock_guard<std::mutex> lock(mu_);
#if defined(AGENT_HAVE_SQLITE3)
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
#else
  db_ = nullptr;
#endif
  path_.clear();
}

bool AgentDb::exec_locked(const std::string& sql, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)sql;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    if (out_error) {
      if (err) {
        *out_error = err;
      } else {
        *out_error = agent_db_sqlite_err(db_);
      }
    }
    if (err) sqlite3_free(err);
    return false;
  }
  return true;
#endif
}

bool AgentDb::ensure_schema_locked(std::string* out_error) {
#if !defined(AGENT_HAVE_SQLITE3)
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  const int kSchemaVersion = 32;

  // Pragmas for multi-connection safety and performance.
  if (!exec_locked("PRAGMA journal_mode=WAL;", out_error)) return false;
  if (!exec_locked("PRAGMA synchronous=NORMAL;", out_error)) return false;
  if (!exec_locked("PRAGMA busy_timeout=5000;", out_error)) return false;
  if (!exec_locked("PRAGMA foreign_keys=ON;", out_error)) return false;

  // Schema versioning.
  if (!exec_locked("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);", out_error)) return false;

  int cur_ver = 0;
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT value FROM meta WHERE key='schema_version' LIMIT 1;";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) == SQLITE_OK && st) {
      if (agent_db_step_row(st)) {
        const unsigned char* txt = sqlite3_column_text(st, 0);
        if (txt && txt[0]) {
          cur_ver = std::atoi((const char*)txt);
        }
      }
      sqlite3_finalize(st);
    } else {
      if (st) sqlite3_finalize(st);
    }
  }

  if (cur_ver > kSchemaVersion) {
    if (out_error) {
      *out_error = "db schema_version is newer than this binary (db=" + std::to_string(cur_ver) +
        " > binary=" + std::to_string(kSchemaVersion) + ")";
    }
    return false;
  }

  // v1 schema (idempotent).
  const char* schema_sql = R"SQL(
	CREATE TABLE IF NOT EXISTS sessions(
	  session_id TEXT PRIMARY KEY,
	  created_unix_ms INTEGER NOT NULL,
  updated_unix_ms INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS messages(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL,
  idx INTEGER NOT NULL,
  role TEXT NOT NULL,
  content TEXT NOT NULL,
  mm_json TEXT,
  mm_bytes INTEGER,
  mm_truncated INTEGER,
  created_unix_ms INTEGER NOT NULL,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS messages_by_session ON messages(session_id, idx);

CREATE TABLE IF NOT EXISTS runs(
  run_id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL,
  job_id TEXT,
  ts_unix_ms INTEGER NOT NULL,
  prompt TEXT NOT NULL,
  tools TEXT NOT NULL,
  model TEXT,
  base_url TEXT,
  stream_assistant INTEGER NOT NULL,
  ok INTEGER NOT NULL,
  error TEXT,
  http_status INTEGER,
  http_body TEXT,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS runs_by_session ON runs(session_id, ts_unix_ms DESC);
CREATE INDEX IF NOT EXISTS runs_by_job ON runs(job_id);

CREATE TABLE IF NOT EXISTS events(
  event_id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id INTEGER NOT NULL,
  ts_unix_ms INTEGER NOT NULL,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL,
  FOREIGN KEY(run_id) REFERENCES runs(run_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS events_by_run ON events(run_id, event_id);

CREATE TABLE IF NOT EXISTS tool_records(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id INTEGER NOT NULL,
  tool_name TEXT NOT NULL,
  tool_call_id TEXT,
  arguments_json TEXT,
  result_text TEXT,
  result_for_prompt_text TEXT,
  result_truncated_for_prompt INTEGER NOT NULL,
  FOREIGN KEY(run_id) REFERENCES runs(run_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS tool_records_by_run ON tool_records(run_id, id);
)SQL";
  if (!exec_locked(schema_sql, out_error)) return false;

  auto column_exists = [&](const char* table, const char* column) -> bool {
    if (!table || !column) return false;
    sqlite3_stmt* st = nullptr;
    std::string sql = "PRAGMA table_info(";
    sql += table;
    sql += ");";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      if (st) sqlite3_finalize(st);
      return false;
    }
    bool found = false;
    while (agent_db_step_row(st)) {
      const unsigned char* name = sqlite3_column_text(st, 1);
      if (name && std::strcmp((const char*)name, column) == 0) {
        found = true;
        break;
      }
    }
    sqlite3_finalize(st);
    return found;
  };

  if (cur_ver < 2) {
    const char* schema_v2 = R"SQL(
CREATE TABLE IF NOT EXISTS artifacts(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id INTEGER NOT NULL,
  ts_unix_ms INTEGER NOT NULL,
  session_id TEXT NOT NULL,
  tool_call_id TEXT,
  path TEXT NOT NULL,
  kind TEXT,
  mime TEXT,
  title TEXT,
  autoplay INTEGER NOT NULL,
  repeat INTEGER NOT NULL,
  artifact_json TEXT NOT NULL,
  FOREIGN KEY(run_id) REFERENCES runs(run_id) ON DELETE CASCADE,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS artifacts_by_run ON artifacts(run_id, id);
CREATE INDEX IF NOT EXISTS artifacts_by_session ON artifacts(session_id, ts_unix_ms DESC);
CREATE INDEX IF NOT EXISTS artifacts_by_path ON artifacts(path);
)SQL";
    if (!exec_locked(schema_v2, out_error)) return false;
    cur_ver = 2;
  }

  if (cur_ver < 3) {
    const char* schema_v3 = R"SQL(
ALTER TABLE runs ADD COLUMN stop_reason TEXT;
ALTER TABLE runs ADD COLUMN steps_executed INTEGER;
ALTER TABLE runs ADD COLUMN tool_calls_total INTEGER;
ALTER TABLE runs ADD COLUMN tool_calls_by_tool_json TEXT;
ALTER TABLE runs ADD COLUMN last_error_reason TEXT;
CREATE INDEX IF NOT EXISTS runs_by_stop_reason ON runs(stop_reason);
CREATE INDEX IF NOT EXISTS runs_by_last_error_reason ON runs(last_error_reason);
)SQL";
    if (!exec_locked(schema_v3, out_error)) return false;
    cur_ver = 3;
  }

  if (cur_ver < 4) {
    const char* schema_v4 = R"SQL(
CREATE TABLE IF NOT EXISTS ui_actions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  run_id INTEGER NOT NULL,
  ts_unix_ms INTEGER NOT NULL,
  session_id TEXT NOT NULL,
  tool_call_id TEXT,
  type TEXT,
  title TEXT,
  message TEXT,
  path TEXT,
  mime TEXT,
  autoplay INTEGER NOT NULL,
  repeat INTEGER NOT NULL,
  action_json TEXT NOT NULL,
  FOREIGN KEY(run_id) REFERENCES runs(run_id) ON DELETE CASCADE,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS ui_actions_by_run ON ui_actions(run_id, id);
CREATE INDEX IF NOT EXISTS ui_actions_by_session ON ui_actions(session_id, ts_unix_ms DESC);
CREATE INDEX IF NOT EXISTS ui_actions_by_type ON ui_actions(type);
)SQL";
    if (!exec_locked(schema_v4, out_error)) return false;
    cur_ver = 4;
  }

  if (cur_ver < 5) {
    const char* schema_v5 = R"SQL(
CREATE TABLE IF NOT EXISTS client_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts_unix_ms INTEGER NOT NULL,
  session_id TEXT NOT NULL,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS client_events_by_session ON client_events(session_id, ts_unix_ms DESC);
CREATE INDEX IF NOT EXISTS client_events_by_type ON client_events(type);
)SQL";
    if (!exec_locked(schema_v5, out_error)) return false;
    cur_ver = 5;
  }

  if (cur_ver < 6) {
    const char* schema_v6 = R"SQL(
CREATE TABLE IF NOT EXISTS audit_records(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL,
  ts_unix_ms INTEGER NOT NULL,
  run_id INTEGER,
  record_json TEXT NOT NULL,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE,
  FOREIGN KEY(run_id) REFERENCES runs(run_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS audit_records_by_session ON audit_records(session_id, ts_unix_ms DESC, id DESC);
)SQL";
    if (!exec_locked(schema_v6, out_error)) return false;
    cur_ver = 6;
  }

  if (cur_ver < 7) {
    const char* schema_v7 = R"SQL(
CREATE TABLE IF NOT EXISTS scene_states(
  session_id TEXT PRIMARY KEY,
  updated_unix_ms INTEGER NOT NULL,
  scene_json TEXT NOT NULL,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS scene_states_by_updated ON scene_states(updated_unix_ms DESC);
)SQL";
    if (!exec_locked(schema_v7, out_error)) return false;
    cur_ver = 7;
  }

  if (cur_ver < 8) {
    const char* schema_v8 = R"SQL(
CREATE TABLE IF NOT EXISTS jobs(
  job_id TEXT PRIMARY KEY,
  session_id TEXT,
  created_unix_ms INTEGER NOT NULL,
  updated_unix_ms INTEGER NOT NULL,
  status TEXT NOT NULL,
  cancel_requested INTEGER NOT NULL,
  error TEXT,
  stop_reason TEXT,
  result_json TEXT,
  last_heartbeat_unix_ms INTEGER
);
CREATE INDEX IF NOT EXISTS jobs_by_status ON jobs(status, updated_unix_ms DESC);
CREATE INDEX IF NOT EXISTS jobs_by_session ON jobs(session_id, updated_unix_ms DESC);
)SQL";
    if (!exec_locked(schema_v8, out_error)) return false;
    cur_ver = 8;
  }

  if (cur_ver < 9) {
    const char* schema_v9 = R"SQL(
CREATE TABLE IF NOT EXISTS workflows(
  workflow_id TEXT PRIMARY KEY,
  session_id TEXT,
  trace_id TEXT,
  created_unix_ms INTEGER NOT NULL,
  updated_unix_ms INTEGER NOT NULL,
  status TEXT NOT NULL,
  cancel_requested INTEGER NOT NULL,
  error TEXT,
  spec_json TEXT NOT NULL,
  result_json TEXT,
  FOREIGN KEY(session_id) REFERENCES sessions(session_id) ON DELETE SET NULL
);
CREATE INDEX IF NOT EXISTS workflows_by_status ON workflows(status, updated_unix_ms DESC);
CREATE INDEX IF NOT EXISTS workflows_by_trace ON workflows(trace_id);
CREATE INDEX IF NOT EXISTS workflows_by_session ON workflows(session_id, updated_unix_ms DESC);

CREATE TABLE IF NOT EXISTS workflow_tasks(
  workflow_id TEXT NOT NULL,
  task_id TEXT NOT NULL,
  created_unix_ms INTEGER NOT NULL,
  updated_unix_ms INTEGER NOT NULL,
  status TEXT NOT NULL,
  attempt INTEGER NOT NULL,
  max_attempts INTEGER NOT NULL,
  ready_unix_ms INTEGER NOT NULL,
  started_unix_ms INTEGER,
  finished_unix_ms INTEGER,
  depends_on_json TEXT,
  request_json TEXT NOT NULL,
  expect_json TEXT,
  result_json TEXT,
  error TEXT,
  PRIMARY KEY(workflow_id, task_id),
  FOREIGN KEY(workflow_id) REFERENCES workflows(workflow_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS workflow_tasks_by_workflow ON workflow_tasks(workflow_id, updated_unix_ms DESC);
CREATE INDEX IF NOT EXISTS workflow_tasks_by_status ON workflow_tasks(status, ready_unix_ms, updated_unix_ms DESC);

CREATE TABLE IF NOT EXISTS workflow_events(
  event_id INTEGER PRIMARY KEY AUTOINCREMENT,
  workflow_id TEXT NOT NULL,
  task_id TEXT,
  ts_unix_ms INTEGER NOT NULL,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL,
  FOREIGN KEY(workflow_id) REFERENCES workflows(workflow_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS workflow_events_by_workflow ON workflow_events(workflow_id, event_id);
)SQL";
    if (!exec_locked(schema_v9, out_error)) return false;
    cur_ver = 9;
  }

  if (cur_ver < 10) {
    const char* schema_v10 = R"SQL(
ALTER TABLE jobs ADD COLUMN trace_id TEXT;
ALTER TABLE jobs ADD COLUMN request_json TEXT;
CREATE INDEX IF NOT EXISTS jobs_by_trace ON jobs(trace_id);
)SQL";
    if (!exec_locked(schema_v10, out_error)) return false;
    cur_ver = 10;
  }

  if (cur_ver < 11) {
    const char* schema_v11 = R"SQL(
CREATE TABLE IF NOT EXISTS workflow_events(
  event_id INTEGER PRIMARY KEY AUTOINCREMENT,
  workflow_id TEXT NOT NULL,
  task_id TEXT,
  ts_unix_ms INTEGER NOT NULL,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL,
  FOREIGN KEY(workflow_id) REFERENCES workflows(workflow_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS workflow_events_by_workflow ON workflow_events(workflow_id, event_id);
)SQL";
    if (!exec_locked(schema_v11, out_error)) return false;
    cur_ver = 11;
  }

  if (cur_ver < 12) {
    const char* schema_v12 = R"SQL(
ALTER TABLE jobs ADD COLUMN priority INTEGER;
ALTER TABLE workflows ADD COLUMN priority INTEGER;
ALTER TABLE workflow_tasks ADD COLUMN priority INTEGER;

UPDATE jobs SET priority=0 WHERE priority IS NULL;
UPDATE workflows SET priority=0 WHERE priority IS NULL;
UPDATE workflow_tasks SET priority=0 WHERE priority IS NULL;

CREATE INDEX IF NOT EXISTS jobs_by_status_prio ON jobs(status, priority DESC, updated_unix_ms DESC);
CREATE INDEX IF NOT EXISTS workflows_by_status_prio ON workflows(status, priority DESC, updated_unix_ms DESC);
CREATE INDEX IF NOT EXISTS workflow_tasks_by_status_prio ON workflow_tasks(status, priority DESC, ready_unix_ms, updated_unix_ms DESC);
)SQL";
    if (!exec_locked(schema_v12, out_error)) return false;
    cur_ver = 12;
  }

  if (cur_ver < 13) {
    const char* schema_v13 = R"SQL(
CREATE TABLE IF NOT EXISTS edge_nodes(
  node_id TEXT PRIMARY KEY,
  model TEXT,
  fw_git_sha TEXT,
  caps_sha256 TEXT,
  manifest_json TEXT,
  tags_json TEXT,
  tools_json TEXT,
  hardware_presence_json TEXT,
  health_json TEXT,
  last_hello_utc_ms INTEGER,
  last_heartbeat_utc_ms INTEGER
);
CREATE INDEX IF NOT EXISTS edge_nodes_by_heartbeat ON edge_nodes(last_heartbeat_utc_ms DESC, node_id);

CREATE TABLE IF NOT EXISTS edge_inbox_messages(
  msg_id TEXT PRIMARY KEY,
  ts_utc_ms INTEGER NOT NULL,
  type TEXT NOT NULL,
  from_id TEXT,
  to_id TEXT,
  envelope_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS edge_inbox_by_type ON edge_inbox_messages(type, ts_utc_ms DESC);

CREATE TABLE IF NOT EXISTS edge_outbox_messages(
  outbox_id INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id TEXT NOT NULL,
  ts_utc_ms INTEGER NOT NULL,
  envelope_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS edge_outbox_by_node ON edge_outbox_messages(node_id, outbox_id);

CREATE TABLE IF NOT EXISTS edge_tasks(
  task_id TEXT NOT NULL,
  step_id TEXT NOT NULL,
  node_id TEXT NOT NULL,
  idempotency_key TEXT NOT NULL,
  mode TEXT NOT NULL,
  deadline_utc_ms INTEGER NOT NULL,
  payload_json TEXT NOT NULL,
  state TEXT NOT NULL,
  created_utc_ms INTEGER NOT NULL,
  updated_utc_ms INTEGER NOT NULL,
  result_json TEXT,
  error TEXT,
  PRIMARY KEY(task_id, step_id),
  UNIQUE(node_id, idempotency_key)
);
CREATE INDEX IF NOT EXISTS edge_tasks_by_state ON edge_tasks(state, updated_utc_ms DESC);
CREATE INDEX IF NOT EXISTS edge_tasks_by_node ON edge_tasks(node_id, updated_utc_ms DESC);

CREATE TABLE IF NOT EXISTS edge_task_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  task_id TEXT NOT NULL,
  step_id TEXT NOT NULL,
  ts_utc_ms INTEGER NOT NULL,
  state TEXT NOT NULL,
  data_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS edge_task_events_by_task ON edge_task_events(task_id, step_id, id);

CREATE TABLE IF NOT EXISTS edge_sensor_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  node_id TEXT NOT NULL,
  event_type TEXT NOT NULL,
  ts_utc_ms INTEGER NOT NULL,
  confidence REAL,
  data_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS edge_sensor_by_type ON edge_sensor_events(event_type, ts_utc_ms DESC, id);
)SQL";
    if (!exec_locked(schema_v13, out_error)) return false;
    cur_ver = 13;
  }

  if (cur_ver < 14) {
    const char* schema_v14 = R"SQL(
ALTER TABLE edge_tasks ADD COLUMN tool_name TEXT;
CREATE INDEX IF NOT EXISTS edge_tasks_by_tool ON edge_tasks(node_id, tool_name, updated_utc_ms DESC);

-- Per-node per-tool rate limiter state (platform-side best-effort guardrail).
CREATE TABLE IF NOT EXISTS edge_tool_rate_state(
  node_id TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  window_start_utc_ms INTEGER NOT NULL,
  window_count INTEGER NOT NULL,
  last_call_utc_ms INTEGER NOT NULL,
  PRIMARY KEY(node_id, tool_name)
);

-- Event-triggered automation rules (UM‑SAFE/UM‑WF bridge): SENSOR_EVENT -> TASK_ASSIGN.
CREATE TABLE IF NOT EXISTS edge_rules(
  rule_id TEXT PRIMARY KEY,
  enabled INTEGER NOT NULL,
  event_type TEXT NOT NULL,
  min_confidence REAL NOT NULL,
  cooldown_ms INTEGER NOT NULL,
  last_fired_utc_ms INTEGER NOT NULL,
  action_json TEXT NOT NULL,
  created_utc_ms INTEGER NOT NULL,
  updated_utc_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS edge_rules_by_event ON edge_rules(event_type, enabled, updated_utc_ms DESC);

-- Durable edge workflows (UM‑WF) executed via edge task dispatch + event ingestion.
CREATE TABLE IF NOT EXISTS edge_workflows(
  workflow_id TEXT PRIMARY KEY,
  goal TEXT,
  status TEXT NOT NULL,
  priority INTEGER NOT NULL,
  spec_json TEXT NOT NULL,
  created_utc_ms INTEGER NOT NULL,
  updated_utc_ms INTEGER NOT NULL,
  error TEXT
);
CREATE INDEX IF NOT EXISTS edge_workflows_by_status ON edge_workflows(status, priority DESC, updated_utc_ms DESC);

CREATE TABLE IF NOT EXISTS edge_workflow_steps(
  workflow_id TEXT NOT NULL,
  step_id TEXT NOT NULL,
  kind TEXT NOT NULL,
  depends_on_json TEXT NOT NULL,
  target_json TEXT NOT NULL,
  payload_json TEXT NOT NULL,
  join_mode TEXT,
  deadline_utc_ms INTEGER NOT NULL,
  state TEXT NOT NULL,
  created_utc_ms INTEGER NOT NULL,
  updated_utc_ms INTEGER NOT NULL,
  error TEXT,
  PRIMARY KEY(workflow_id, step_id)
);
CREATE INDEX IF NOT EXISTS edge_workflow_steps_by_state ON edge_workflow_steps(workflow_id, state, updated_utc_ms DESC);

CREATE TABLE IF NOT EXISTS edge_workflow_events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  workflow_id TEXT NOT NULL,
  ts_utc_ms INTEGER NOT NULL,
  type TEXT NOT NULL,
  data_json TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS edge_workflow_events_by_workflow ON edge_workflow_events(workflow_id, id);
)SQL";
    if (!exec_locked(schema_v14, out_error)) return false;
    cur_ver = 14;
  }

  if (cur_ver < 15) {
    const char* schema_v15 = R"SQL(
-- Edge workflow retries/backoff (platform-side): add attempt tracking and scheduling fields.
ALTER TABLE edge_workflow_steps ADD COLUMN attempt INTEGER NOT NULL DEFAULT 0;
ALTER TABLE edge_workflow_steps ADD COLUMN max_attempts INTEGER NOT NULL DEFAULT 1;
ALTER TABLE edge_workflow_steps ADD COLUMN next_ready_utc_ms INTEGER NOT NULL DEFAULT 0;
ALTER TABLE edge_workflow_steps ADD COLUMN backoff_ms INTEGER NOT NULL DEFAULT 0;
CREATE INDEX IF NOT EXISTS edge_workflow_steps_by_ready ON edge_workflow_steps(workflow_id, state, next_ready_utc_ms, updated_utc_ms DESC);
)SQL";
    if (!exec_locked(schema_v15, out_error)) return false;
    cur_ver = 15;
  }

  if (cur_ver < 16) {
    const char* schema_v16 = R"SQL(
-- Durable workflow soft-fail tasks (best_of_n / first_ok patterns).
ALTER TABLE workflow_tasks ADD COLUMN allow_error INTEGER NOT NULL DEFAULT 0;
)SQL";
    if (!exec_locked(schema_v16, out_error)) return false;
    cur_ver = 16;
  }

  if (cur_ver < 17) {
    const char* schema_v17 = R"SQL(
-- Durable workflow policy columns and submit idempotency (platform-side reliability).
ALTER TABLE workflows ADD COLUMN deadline_unix_ms INTEGER;
ALTER TABLE workflows ADD COLUMN idempotency_key TEXT;

-- Indexing: deadline scanning + idempotency dedupe.
CREATE INDEX IF NOT EXISTS workflows_by_deadline ON workflows(deadline_unix_ms);
CREATE UNIQUE INDEX IF NOT EXISTS workflows_by_idempotency_scope_key
  ON workflows(COALESCE(session_id,''), idempotency_key)
  WHERE idempotency_key IS NOT NULL AND idempotency_key <> '';
)SQL";
    if (!exec_locked(schema_v17, out_error)) return false;
    cur_ver = 17;
  }

  if (cur_ver < 18) {
    const char* schema_v18 = R"SQL(
-- Scheduler scan (oldest-first) index: avoids LIMIT-starvation under large backlogs.
CREATE INDEX IF NOT EXISTS workflows_by_status_prio_created
  ON workflows(status, priority DESC, created_unix_ms ASC, workflow_id);
)SQL";
    if (!exec_locked(schema_v18, out_error)) return false;
    cur_ver = 18;
  }

  if (cur_ver < 19) {
    const char* schema_v19 = R"SQL(
-- Durable workflow budgets must charge retries. Store per-task cumulative usage counters.
ALTER TABLE workflow_tasks ADD COLUMN tool_calls_total_cum INTEGER NOT NULL DEFAULT 0;
ALTER TABLE workflow_tasks ADD COLUMN steps_executed_cum INTEGER NOT NULL DEFAULT 0;
ALTER TABLE workflow_tasks ADD COLUMN elapsed_ms_cum INTEGER NOT NULL DEFAULT 0;
)SQL";
    if (!exec_locked(schema_v19, out_error)) return false;
    cur_ver = 19;
  }

  if (cur_ver < 20) {
    const char* schema_v20 = R"SQL(
ALTER TABLE edge_inbox_messages ADD COLUMN processed INTEGER NOT NULL DEFAULT 0;
ALTER TABLE edge_inbox_messages ADD COLUMN processed_utc_ms INTEGER;

-- Back-compat: prior binaries treated inbox persistence as "processed" (dedupe == do nothing).
-- Set processed=1 for existing rows so upgrades preserve old dedupe semantics.
UPDATE edge_inbox_messages SET processed=1, processed_utc_ms=COALESCE(processed_utc_ms, ts_utc_ms)
  WHERE processed IS NULL OR processed=0;
)SQL";
    if (!exec_locked(schema_v20, out_error)) return false;
    cur_ver = 20;
  }

  if (cur_ver < 21) {
    const char* schema_v21 = R"SQL(
-- Provider usage-based budgets: durable workflow token counters (retry-safe across attempts).
ALTER TABLE workflow_tasks ADD COLUMN prompt_tokens_cum INTEGER NOT NULL DEFAULT 0;
ALTER TABLE workflow_tasks ADD COLUMN completion_tokens_cum INTEGER NOT NULL DEFAULT 0;
ALTER TABLE workflow_tasks ADD COLUMN total_tokens_cum INTEGER NOT NULL DEFAULT 0;
)SQL";
    if (!exec_locked(schema_v21, out_error)) return false;
    cur_ver = 21;
  }

  if (cur_ver < 22) {
    const char* schema_v22 = R"SQL(
-- Edge trace correlation (platform-side): persist trace_id on edge_tasks so correlation survives
-- even if a node omits echoing trace fields on TASK_* messages.
ALTER TABLE edge_tasks ADD COLUMN trace_id TEXT;
CREATE INDEX IF NOT EXISTS edge_tasks_by_trace ON edge_tasks(trace_id, updated_utc_ms DESC);
)SQL";
    if (!exec_locked(schema_v22, out_error)) return false;
    cur_ver = 22;
  }

  if (cur_ver < 23) {
    const char* schema_v23 = R"SQL(
-- Edge tool resource locking (best-effort, platform-side): prevent parallel tool calls that
-- contend for the same physical resource (e.g. motor, ui.led, hvac).
ALTER TABLE edge_tasks ADD COLUMN resource_lock TEXT;
CREATE INDEX IF NOT EXISTS edge_tasks_by_node_lock_state
  ON edge_tasks(node_id, resource_lock, state, updated_utc_ms DESC);
)SQL";
    if (!exec_locked(schema_v23, out_error)) return false;
    cur_ver = 23;
  }

  if (cur_ver < 24) {
    const char* schema_v24 = R"SQL(
-- Edge task attestation surfaces (best-effort, platform-side):
-- - result_sha256: a deterministic hash surface for replay/quorum (computed over stored result_json bytes;
--                  prefer agent_json_c14n_v1 canonicalization when available, otherwise fall back to raw bytes)
-- - attest_json:   optional node-provided attestation blob (opaque JSON object)
ALTER TABLE edge_tasks ADD COLUMN result_sha256 TEXT;
ALTER TABLE edge_tasks ADD COLUMN attest_json TEXT;
CREATE INDEX IF NOT EXISTS edge_tasks_by_result_sha256 ON edge_tasks(result_sha256, updated_utc_ms DESC);
)SQL";
    if (!exec_locked(schema_v24, out_error)) return false;
    cur_ver = 24;
  }

  if (cur_ver < 25) {
    const char* schema_v25 = R"SQL(
-- Durable fair-queue session state for workflow scheduler policies (v2.3+).
-- Used to persist DRR deficits across daemon restarts (best-effort).
CREATE TABLE IF NOT EXISTS workflow_fairq_sessions(
  session_id TEXT PRIMARY KEY,
  deficit INTEGER NOT NULL DEFAULT 0,
  weight INTEGER NOT NULL DEFAULT 1,
  updated_unix_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS workflow_fairq_sessions_by_updated
  ON workflow_fairq_sessions(updated_unix_ms DESC);
)SQL";
    if (!exec_locked(schema_v25, out_error)) return false;
    cur_ver = 25;
  }

  if (cur_ver < 26) {
    const char* schema_v26 = R"SQL(
-- Edge envelope auth anti-replay: track a best-effort monotonic auth.seq per node.
ALTER TABLE edge_nodes ADD COLUMN last_auth_seq INTEGER;
)SQL";
    if (!exec_locked(schema_v26, out_error)) return false;
    cur_ver = 26;
  }

  if (cur_ver < 27) {
    const char* schema_v27 = R"SQL(
-- Run replay bundles: persist redacted request/response and deterministic hash token.
ALTER TABLE runs ADD COLUMN request_json TEXT;
ALTER TABLE runs ADD COLUMN response_json TEXT;
ALTER TABLE runs ADD COLUMN replay_sha256 TEXT;
ALTER TABLE runs ADD COLUMN replay_sha256_alg TEXT;
ALTER TABLE runs ADD COLUMN replay_sha256_schema TEXT;
ALTER TABLE runs ADD COLUMN replay_error TEXT;
CREATE INDEX IF NOT EXISTS runs_by_replay_sha256 ON runs(replay_sha256);
)SQL";
    if (!exec_locked(schema_v27, out_error)) return false;
    cur_ver = 27;
  }

  if (cur_ver < 28) {
    const char* schema_v28 = R"SQL(
-- Blob storage manifest (tiered blob store; v0 local-only).
CREATE TABLE IF NOT EXISTS blob_manifest(
  blob_id TEXT PRIMARY KEY,
  size_bytes INTEGER NOT NULL,
  mime TEXT,
  sha256_hex TEXT NOT NULL,
  created_utc_ms INTEGER NOT NULL,
  last_access_utc_ms INTEGER,
  ref_count INTEGER NOT NULL DEFAULT 0,
  tier TEXT NOT NULL,
  location TEXT NOT NULL,
  etag TEXT,
  storage_class TEXT
);
CREATE INDEX IF NOT EXISTS blob_manifest_by_last_access ON blob_manifest(last_access_utc_ms DESC);
CREATE INDEX IF NOT EXISTS blob_manifest_by_ref_count ON blob_manifest(ref_count, created_utc_ms DESC);

-- Map artifacts to blobs (ref-counted GC).
CREATE TABLE IF NOT EXISTS artifact_blobs(
  artifact_id INTEGER NOT NULL,
  blob_id TEXT NOT NULL,
  PRIMARY KEY(artifact_id, blob_id),
  FOREIGN KEY(artifact_id) REFERENCES artifacts(id) ON DELETE CASCADE,
  FOREIGN KEY(blob_id) REFERENCES blob_manifest(blob_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS artifact_blobs_by_blob ON artifact_blobs(blob_id);

CREATE TRIGGER IF NOT EXISTS artifact_blobs_insert AFTER INSERT ON artifact_blobs
BEGIN
  UPDATE blob_manifest SET ref_count = ref_count + 1 WHERE blob_id = NEW.blob_id;
END;
CREATE TRIGGER IF NOT EXISTS artifact_blobs_delete AFTER DELETE ON artifact_blobs
BEGIN
  UPDATE blob_manifest
  SET ref_count = CASE WHEN ref_count > 0 THEN ref_count - 1 ELSE 0 END
  WHERE blob_id = OLD.blob_id;
END;
)SQL";
    if (!exec_locked(schema_v28, out_error)) return false;
    cur_ver = 28;
  }

  if (cur_ver < 29) {
    if (!column_exists("messages", "mm_json")) {
      if (!exec_locked("ALTER TABLE messages ADD COLUMN mm_json TEXT;", out_error)) return false;
    }
    if (!column_exists("messages", "mm_bytes")) {
      if (!exec_locked("ALTER TABLE messages ADD COLUMN mm_bytes INTEGER;", out_error)) return false;
    }
    if (!column_exists("messages", "mm_truncated")) {
      if (!exec_locked("ALTER TABLE messages ADD COLUMN mm_truncated INTEGER;", out_error)) return false;
    }
    cur_ver = 29;
  }

  if (cur_ver < 30) {
    const char* schema_v30 = R"SQL(
CREATE TABLE IF NOT EXISTS approval_requests(
  approval_id TEXT PRIMARY KEY,
  run_id INTEGER,
  trace_id TEXT,
  session_id TEXT,
  job_id TEXT,
  team_id TEXT,
  tool_name TEXT NOT NULL,
  tool_call_id TEXT,
  tool_args_hash TEXT,
  required_approvals INTEGER NOT NULL,
  role_constraints_json TEXT,
  require_distinct_roles INTEGER,
  status TEXT NOT NULL,
  created_unix_ms INTEGER NOT NULL,
  expires_unix_ms INTEGER,
  decision_reason TEXT
);
CREATE INDEX IF NOT EXISTS approval_requests_by_status ON approval_requests(status, created_unix_ms DESC);
CREATE INDEX IF NOT EXISTS approval_requests_by_trace ON approval_requests(trace_id, created_unix_ms DESC);
CREATE INDEX IF NOT EXISTS approval_requests_by_run ON approval_requests(run_id, created_unix_ms DESC);

CREATE TABLE IF NOT EXISTS approval_decisions(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  approval_id TEXT NOT NULL,
  member_id TEXT NOT NULL,
  member_role TEXT,
  decision TEXT NOT NULL,
  decision_unix_ms INTEGER NOT NULL,
  note TEXT,
  FOREIGN KEY(approval_id) REFERENCES approval_requests(approval_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS approval_decisions_by_approval ON approval_decisions(approval_id, id);
)SQL";
    if (!exec_locked(schema_v30, out_error)) return false;
    cur_ver = 30;
  }

  if (cur_ver < 31) {
    if (!column_exists("approval_decisions", "member_role")) {
      if (!exec_locked("ALTER TABLE approval_decisions ADD COLUMN member_role TEXT;", out_error)) return false;
    }
    cur_ver = 31;
  }

  if (cur_ver < 32) {
    if (!column_exists("approval_requests", "require_distinct_roles")) {
      if (!exec_locked("ALTER TABLE approval_requests ADD COLUMN require_distinct_roles INTEGER;", out_error)) return false;
    }
    cur_ver = 32;
  }

  // Record schema version.
  {
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','" << kSchemaVersion << "');";
	  if (!exec_locked(oss.str(), out_error)) return false;
	  }
	  return true;
#endif
}

}  // namespace agentd
