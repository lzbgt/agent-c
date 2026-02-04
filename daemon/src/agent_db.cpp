#include "agent_db.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <sstream>

namespace agentd {
namespace {

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string sqlite_err(sqlite3* db) {
  if (!db) return "sqlite: (db is null)";
  const char* msg = sqlite3_errmsg(db);
  return msg ? std::string(msg) : std::string("sqlite: unknown error");
}

static bool bind_text(sqlite3_stmt* st, int idx, const std::string& s) {
  return sqlite3_bind_text(st, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

static bool bind_text_or_null(sqlite3_stmt* st, int idx, const std::string& s) {
  if (s.empty()) return sqlite3_bind_null(st, idx) == SQLITE_OK;
  return bind_text(st, idx, s);
}

static bool bind_i64(sqlite3_stmt* st, int idx, int64_t v) {
  return sqlite3_bind_int64(st, idx, (sqlite3_int64)v) == SQLITE_OK;
}

static bool bind_i32(sqlite3_stmt* st, int idx, int v) {
  return sqlite3_bind_int(st, idx, v) == SQLITE_OK;
}

static bool bind_i32_or_null(sqlite3_stmt* st, int idx, int v) {
  if (v == AgentDb::kIntUnset) return sqlite3_bind_null(st, idx) == SQLITE_OK;
  return bind_i32(st, idx, v);
}

static bool step_row(sqlite3_stmt* st) {
  const int rc = sqlite3_step(st);
  return rc == SQLITE_ROW;
}

static bool step_done(sqlite3_stmt* st) {
  const int rc = sqlite3_step(st);
  return rc == SQLITE_DONE;
}

#else
static std::string sqlite_err(void*) { return "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)"; }
#endif

}  // namespace

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
    if (out_error) *out_error = sqlite_err(db);
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
        *out_error = sqlite_err(db_);
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
  const int kSchemaVersion = 16;

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
      if (step_row(st)) {
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

  // Record schema version.
  {
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','" << kSchemaVersion << "');";
	  if (!exec_locked(oss.str(), out_error)) return false;
	  }
	  return true;
#endif
}

bool AgentDb::upsert_job(const JobRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.job_id.empty()) {
    if (out_error) *out_error = "upsert_job: job_id is empty";
    return false;
  }
  const int64_t now = (row.updated_unix_ms > 0) ? row.updated_unix_ms : unix_ms_now();
  const int64_t created = (row.created_unix_ms > 0) ? row.created_unix_ms : now;
  const std::string status = row.status.empty() ? "queued" : row.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO jobs(
  job_id, session_id, trace_id, request_json, priority, created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(job_id) DO UPDATE SET
  session_id = CASE
    WHEN excluded.session_id IS NOT NULL AND excluded.session_id <> '' THEN excluded.session_id
    ELSE jobs.session_id
  END,
  trace_id = CASE
    WHEN excluded.trace_id IS NOT NULL AND excluded.trace_id <> '' THEN excluded.trace_id
    ELSE jobs.trace_id
  END,
  request_json = CASE
    WHEN excluded.request_json IS NOT NULL AND excluded.request_json <> '' THEN excluded.request_json
    ELSE jobs.request_json
  END,
  priority = CASE
    WHEN excluded.priority IS NOT NULL THEN excluded.priority
    ELSE jobs.priority
  END,
  updated_unix_ms = excluded.updated_unix_ms,
  status = CASE
    WHEN jobs.status IN ('done','error','cancelled','interrupted') THEN jobs.status
    ELSE excluded.status
  END,
  cancel_requested = CASE
    WHEN jobs.cancel_requested = 1 OR excluded.cancel_requested = 1 THEN 1
    ELSE 0
  END,
  error = CASE
    WHEN excluded.error IS NOT NULL AND excluded.error <> '' THEN excluded.error
    ELSE jobs.error
  END,
  stop_reason = CASE
    WHEN excluded.stop_reason IS NOT NULL AND excluded.stop_reason <> '' THEN excluded.stop_reason
    ELSE jobs.stop_reason
  END,
  result_json = CASE
    WHEN excluded.result_json IS NOT NULL AND excluded.result_json <> '' THEN excluded.result_json
    ELSE jobs.result_json
  END,
  last_heartbeat_unix_ms = CASE
    WHEN excluded.last_heartbeat_unix_ms IS NOT NULL AND excluded.last_heartbeat_unix_ms > COALESCE(jobs.last_heartbeat_unix_ms,0)
      THEN excluded.last_heartbeat_unix_ms
    ELSE jobs.last_heartbeat_unix_ms
  END;
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && bind_text(st, 1, row.job_id);
  ok = ok && bind_text_or_null(st, 2, row.session_id);
  ok = ok && bind_text_or_null(st, 3, row.trace_id);
  ok = ok && bind_text_or_null(st, 4, row.request_json);
  ok = ok && bind_i32_or_null(st, 5, row.priority);
  ok = ok && bind_i64(st, 6, created);
  ok = ok && bind_i64(st, 7, now);
  ok = ok && bind_text(st, 8, status);
  ok = ok && bind_i32(st, 9, row.cancel_requested ? 1 : 0);
  ok = ok && bind_text_or_null(st, 10, row.error);
  ok = ok && bind_text_or_null(st, 11, row.stop_reason);
  ok = ok && bind_text_or_null(st, 12, row.result_json);
  ok = ok && bind_i64(st, 13, row.last_heartbeat_unix_ms);

  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) {
    *out_error = sqlite_err(db_);
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_job(const std::string& job_id, JobRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = JobRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)job_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (job_id.empty()) {
    if (out_error) *out_error = "get_job: job_id is empty";
    return false;
  }
  if (!out_row) {
    if (out_error) *out_error = "get_job: out_row is null";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
		SELECT
		  job_id, session_id, trace_id, request_json, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
		FROM jobs
		WHERE job_id=?
		LIMIT 1;
		)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, job_id);
  bool found = false;
	  if (ok && step_row(st)) {
	    found = true;
	    const unsigned char* jid = sqlite3_column_text(st, 0);
	    const unsigned char* sid = sqlite3_column_text(st, 1);
	    const unsigned char* tid = sqlite3_column_text(st, 2);
	    const unsigned char* rq = sqlite3_column_text(st, 3);
	    out_row->job_id = jid ? (const char*)jid : "";
	    out_row->session_id = sid ? (const char*)sid : "";
	    out_row->trace_id = tid ? (const char*)tid : "";
	    out_row->request_json = rq ? (const char*)rq : "";
	    out_row->priority = sqlite3_column_int(st, 4);
	    out_row->created_unix_ms = sqlite3_column_int64(st, 5);
	    out_row->updated_unix_ms = sqlite3_column_int64(st, 6);
	    const unsigned char* stxt = sqlite3_column_text(st, 7);
	    out_row->status = stxt ? (const char*)stxt : "";
	    out_row->cancel_requested = sqlite3_column_int(st, 8) != 0;
	    const unsigned char* etxt = sqlite3_column_text(st, 9);
	    out_row->error = etxt ? (const char*)etxt : "";
	    const unsigned char* srtxt = sqlite3_column_text(st, 10);
	    out_row->stop_reason = srtxt ? (const char*)srtxt : "";
	    const unsigned char* rj = sqlite3_column_text(st, 11);
	    out_row->result_json = rj ? (const char*)rj : "";
	    out_row->last_heartbeat_unix_ms = sqlite3_column_int64(st, 12);
	  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::delete_job(const std::string& job_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)job_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (job_id.empty()) {
    if (out_error) *out_error = "delete_job: job_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "DELETE FROM jobs WHERE job_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, job_id);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::mark_inflight_jobs_interrupted(int64_t now_unix_ms, const std::string& reason, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  (void)reason;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const std::string r = reason.empty() ? "daemon_restart" : reason;
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE jobs
SET
  updated_unix_ms=?,
  status='interrupted',
  error=CASE WHEN error IS NULL OR error='' THEN 'interrupted by restart' ELSE error END,
  stop_reason=?
WHERE status='queued' OR status='running';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_i64(st, 1, now_unix_ms > 0 ? now_unix_ms : unix_ms_now());
  ok = ok && bind_text(st, 2, r);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_jobs_by_status(
  const std::string& status,
  size_t max_rows,
  std::vector<JobRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT job_id, session_id, trace_id, request_json, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
FROM jobs
WHERE status=?
ORDER BY COALESCE(priority,0) DESC, updated_unix_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, status);
  ok = ok && bind_i64(st, 2, (int64_t)max_rows);
  while (ok && step_row(st)) {
    JobRow row;
    const unsigned char* jid = sqlite3_column_text(st, 0);
    const unsigned char* sid = sqlite3_column_text(st, 1);
    const unsigned char* tid = sqlite3_column_text(st, 2);
    const unsigned char* rq = sqlite3_column_text(st, 3);
    row.job_id = jid ? (const char*)jid : "";
    row.session_id = sid ? (const char*)sid : "";
    row.trace_id = tid ? (const char*)tid : "";
    row.request_json = rq ? (const char*)rq : "";
    row.priority = sqlite3_column_int(st, 4);
    row.created_unix_ms = sqlite3_column_int64(st, 5);
    row.updated_unix_ms = sqlite3_column_int64(st, 6);
    const unsigned char* stxt = sqlite3_column_text(st, 7);
    row.status = stxt ? (const char*)stxt : "";
    row.cancel_requested = sqlite3_column_int(st, 8) != 0;
    const unsigned char* etxt = sqlite3_column_text(st, 9);
    row.error = etxt ? (const char*)etxt : "";
    const unsigned char* srtxt = sqlite3_column_text(st, 10);
    row.stop_reason = srtxt ? (const char*)srtxt : "";
    const unsigned char* rj = sqlite3_column_text(st, 11);
    row.result_json = rj ? (const char*)rj : "";
    row.last_heartbeat_unix_ms = sqlite3_column_int64(st, 12);
    out_rows_desc->push_back(std::move(row));
  }
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::claim_job(const std::string& job_id, int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)job_id;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (job_id.empty()) {
    if (out_error) *out_error = "claim_job: job_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE jobs
SET status='running',
    updated_unix_ms=?,
    last_heartbeat_unix_ms=?
WHERE job_id=? AND status='queued' AND (cancel_requested IS NULL OR cancel_requested=0);
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : unix_ms_now();
  bool ok = true;
  ok = ok && bind_i64(st, 1, now);
  ok = ok && bind_i64(st, 2, now);
  ok = ok && bind_text(st, 3, job_id);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  const int changed = sqlite3_changes(db_);
  return changed > 0;
#endif
}

bool AgentDb::recover_inflight_jobs_resumable(int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : unix_ms_now();
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  bool ok = true;
  ok = ok && exec_locked(
    "UPDATE jobs SET status='queued', updated_unix_ms=" + std::to_string((long long)now) + " WHERE status='running';",
    out_error);
  ok = ok && exec_locked(
    "UPDATE jobs SET status='interrupted', updated_unix_ms=" + std::to_string((long long)now) +
      ", error=CASE WHEN error IS NULL OR error='' THEN 'missing request_json; cannot resume' ELSE error END"
      ", stop_reason=CASE WHEN stop_reason IS NULL OR stop_reason='' THEN 'missing_request_json' ELSE stop_reason END"
      " WHERE (status='queued' OR status='running') AND (request_json IS NULL OR request_json='');",
    out_error);
  if (ok) {
    ok = exec_locked("COMMIT;", out_error);
  } else {
    std::string ign;
    (void)exec_locked("ROLLBACK;", &ign);
  }
  return ok;
#endif
}

bool AgentDb::recover_inflight_workflows(int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : unix_ms_now();

  // At-least-once semantics: if a task was running when the daemon died, we cannot know if it completed.
  // Recover it back to queued so it can resume.
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  bool ok = true;
  ok = ok && exec_locked("UPDATE workflows SET status='queued', updated_unix_ms=" + std::to_string((long long)now) +
                           " WHERE status='running';", out_error);
  ok = ok && exec_locked("UPDATE workflow_tasks SET status='queued', updated_unix_ms=" + std::to_string((long long)now) +
                           " WHERE status='running';", out_error);
  if (ok) {
    ok = exec_locked("COMMIT;", out_error);
  } else {
    std::string ign;
    (void)exec_locked("ROLLBACK;", &ign);
  }
  return ok;
#endif
}

bool AgentDb::create_workflow(const WorkflowRow& wf, const std::vector<WorkflowTaskRow>& tasks, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  (void)tasks;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty()) {
    if (out_error) *out_error = "create_workflow: workflow_id is empty";
    return false;
  }
  if (wf.spec_json.empty()) {
    if (out_error) *out_error = "create_workflow: spec_json is empty";
    return false;
  }
  if (tasks.empty()) {
    if (out_error) *out_error = "create_workflow: tasks empty";
    return false;
  }

  const int64_t now = wf.updated_unix_ms > 0 ? wf.updated_unix_ms : unix_ms_now();
  const int64_t created = wf.created_unix_ms > 0 ? wf.created_unix_ms : now;
  const std::string status = wf.status.empty() ? "queued" : wf.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;

  bool ok = true;
  {
	    sqlite3_stmt* st = nullptr;
	    const char* sql = R"SQL(
	INSERT INTO workflows(
	  workflow_id, session_id, trace_id, priority, created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	) VALUES(?,?,?,?,?,?,?,?,?,?,?);
	)SQL";
	    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
	      ok = false;
	      if (out_error) *out_error = sqlite_err(db_);
	    } else {
	      ok = ok && bind_text(st, 1, wf.workflow_id);
	      ok = ok && bind_text_or_null(st, 2, wf.session_id);
	      ok = ok && bind_text(st, 3, wf.trace_id);
	      ok = ok && bind_i32_or_null(st, 4, wf.priority);
	      ok = ok && bind_i64(st, 5, created);
	      ok = ok && bind_i64(st, 6, now);
	      ok = ok && bind_text(st, 7, status);
	      ok = ok && bind_i32(st, 8, wf.cancel_requested ? 1 : 0);
	      ok = ok && bind_text(st, 9, wf.error);
	      ok = ok && bind_text(st, 10, wf.spec_json);
	      ok = ok && bind_text(st, 11, wf.result_json);
	      ok = ok && step_done(st);
	      if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
	      sqlite3_finalize(st);
	    }
	  }

  if (ok) {
	    sqlite3_stmt* st = nullptr;
	    const char* sql = R"SQL(
	INSERT INTO workflow_tasks(
	  workflow_id, task_id, priority, created_unix_ms, updated_unix_ms, status, allow_error, attempt, max_attempts, ready_unix_ms,
	  started_unix_ms, finished_unix_ms, depends_on_json, request_json, expect_json, result_json, error
	) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
	)SQL";
	    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
	      ok = false;
	      if (out_error) *out_error = sqlite_err(db_);
	    } else {
	      for (const auto& t : tasks) {
	        if (t.task_id.empty() || t.request_json.empty()) {
	          ok = false;
	          if (out_error && out_error->empty()) *out_error = "create_workflow: task missing task_id/request_json";
	          break;
	        }
        const int64_t t_created = t.created_unix_ms > 0 ? t.created_unix_ms : created;
        const int64_t t_updated = t.updated_unix_ms > 0 ? t.updated_unix_ms : now;
        const std::string t_status = t.status.empty() ? "queued" : t.status;
        const int t_attempt = t.attempt < 0 ? 0 : t.attempt;
        const int t_max_attempts = t.max_attempts <= 0 ? 1 : t.max_attempts;
        const int64_t t_ready = t.ready_unix_ms > 0 ? t.ready_unix_ms : 0;
        const int64_t t_started = t.started_unix_ms > 0 ? t.started_unix_ms : 0;
        const int64_t t_finished = t.finished_unix_ms > 0 ? t.finished_unix_ms : 0;

	        sqlite3_reset(st);
	        sqlite3_clear_bindings(st);
	        ok = ok && bind_text(st, 1, wf.workflow_id);
	        ok = ok && bind_text(st, 2, t.task_id);
	        ok = ok && bind_i32_or_null(st, 3, t.priority);
		        ok = ok && bind_i64(st, 4, t_created);
		        ok = ok && bind_i64(st, 5, t_updated);
		        ok = ok && bind_text(st, 6, t_status);
		        ok = ok && bind_i32(st, 7, t.allow_error ? 1 : 0);
		        ok = ok && bind_i32(st, 8, t_attempt);
		        ok = ok && bind_i32(st, 9, t_max_attempts);
		        ok = ok && bind_i64(st, 10, t_ready);
		        ok = ok && bind_i64(st, 11, t_started);
		        ok = ok && bind_i64(st, 12, t_finished);
		        ok = ok && bind_text(st, 13, t.depends_on_json);
		        ok = ok && bind_text(st, 14, t.request_json);
		        ok = ok && bind_text(st, 15, t.expect_json);
		        ok = ok && bind_text(st, 16, t.result_json);
		        ok = ok && bind_text(st, 17, t.error);
		        ok = ok && step_done(st);
		        if (!ok) {
		          if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
		          break;
		        }
      }
      sqlite3_finalize(st);
    }
  }

  if (ok) {
    ok = exec_locked("COMMIT;", out_error);
  } else {
    std::string ign;
    (void)exec_locked("ROLLBACK;", &ign);
  }
  return ok;
#endif
}

bool AgentDb::get_workflow(const std::string& workflow_id, WorkflowRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = WorkflowRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "get_workflow: workflow_id is empty";
    return false;
  }
  if (!out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
	sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	FROM workflows
	WHERE workflow_id=?
	LIMIT 1;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, workflow_id);
  bool found = false;
	  if (ok && step_row(st)) {
	    found = true;
	    const unsigned char* wid = sqlite3_column_text(st, 0);
	    const unsigned char* sid = sqlite3_column_text(st, 1);
	    const unsigned char* tid = sqlite3_column_text(st, 2);
	    const unsigned char* stxt = sqlite3_column_text(st, 6);
	    const unsigned char* etxt = sqlite3_column_text(st, 8);
	    const unsigned char* spec = sqlite3_column_text(st, 9);
	    const unsigned char* res = sqlite3_column_text(st, 10);
	    out_row->workflow_id = wid ? (const char*)wid : "";
	    out_row->session_id = sid ? (const char*)sid : "";
	    out_row->trace_id = tid ? (const char*)tid : "";
	    out_row->priority = sqlite3_column_int(st, 3);
	    out_row->created_unix_ms = sqlite3_column_int64(st, 4);
	    out_row->updated_unix_ms = sqlite3_column_int64(st, 5);
	    out_row->status = stxt ? (const char*)stxt : "";
	    out_row->cancel_requested = sqlite3_column_int(st, 7) != 0;
	    out_row->error = etxt ? (const char*)etxt : "";
	    out_row->spec_json = spec ? (const char*)spec : "";
	    out_row->result_json = res ? (const char*)res : "";
	  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_workflows_by_status(
  const std::string& status,
  size_t max_rows,
  std::vector<WorkflowRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
	sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	SELECT workflow_id, session_id, trace_id, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	FROM workflows
	WHERE status=?
	ORDER BY COALESCE(priority,0) DESC, updated_unix_ms DESC
	LIMIT ?;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, status);
  ok = ok && bind_i64(st, 2, (int64_t)max_rows);
	  while (ok && step_row(st)) {
	    WorkflowRow row;
	    const unsigned char* wid = sqlite3_column_text(st, 0);
	    const unsigned char* sid = sqlite3_column_text(st, 1);
	    const unsigned char* tid = sqlite3_column_text(st, 2);
	    const unsigned char* stxt = sqlite3_column_text(st, 6);
	    const unsigned char* etxt = sqlite3_column_text(st, 8);
	    const unsigned char* spec = sqlite3_column_text(st, 9);
	    const unsigned char* res = sqlite3_column_text(st, 10);
	    row.workflow_id = wid ? (const char*)wid : "";
	    row.session_id = sid ? (const char*)sid : "";
	    row.trace_id = tid ? (const char*)tid : "";
	    row.priority = sqlite3_column_int(st, 3);
	    row.created_unix_ms = sqlite3_column_int64(st, 4);
	    row.updated_unix_ms = sqlite3_column_int64(st, 5);
	    row.status = stxt ? (const char*)stxt : "";
	    row.cancel_requested = sqlite3_column_int(st, 7) != 0;
	    row.error = etxt ? (const char*)etxt : "";
	    row.spec_json = spec ? (const char*)spec : "";
	    row.result_json = res ? (const char*)res : "";
	    out_rows_desc->push_back(std::move(row));
	  }
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_workflow_tasks(const std::string& workflow_id, std::vector<WorkflowTaskRow>* out_rows, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_rows) out_rows->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_workflow_tasks: workflow_id is empty";
    return false;
  }
  if (!out_rows) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
	sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	SELECT task_id, COALESCE(priority,0), created_unix_ms, updated_unix_ms, status, attempt, max_attempts, ready_unix_ms, started_unix_ms, finished_unix_ms,
	       depends_on_json, request_json, expect_json, result_json, error, COALESCE(allow_error,0)
	FROM workflow_tasks
	WHERE workflow_id=?
	ORDER BY created_unix_ms ASC, task_id ASC;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, workflow_id);
	  while (ok && step_row(st)) {
	    WorkflowTaskRow row;
	    row.workflow_id = workflow_id;
	    const unsigned char* tid = sqlite3_column_text(st, 0);
	    row.task_id = tid ? (const char*)tid : "";
	    row.priority = sqlite3_column_int(st, 1);
	    row.created_unix_ms = sqlite3_column_int64(st, 2);
	    row.updated_unix_ms = sqlite3_column_int64(st, 3);
	    const unsigned char* stxt = sqlite3_column_text(st, 4);
	    row.status = stxt ? (const char*)stxt : "";
	    row.attempt = sqlite3_column_int(st, 5);
	    row.max_attempts = sqlite3_column_int(st, 6);
	    row.ready_unix_ms = sqlite3_column_int64(st, 7);
	    row.started_unix_ms = sqlite3_column_int64(st, 8);
	    row.finished_unix_ms = sqlite3_column_int64(st, 9);
	    const unsigned char* deps = sqlite3_column_text(st, 10);
	    const unsigned char* req = sqlite3_column_text(st, 11);
	    const unsigned char* exp = sqlite3_column_text(st, 12);
	    const unsigned char* res = sqlite3_column_text(st, 13);
	    const unsigned char* etxt = sqlite3_column_text(st, 14);
	    row.allow_error = sqlite3_column_int(st, 15) != 0;
	    row.depends_on_json = deps ? (const char*)deps : "";
	    row.request_json = req ? (const char*)req : "";
	    row.expect_json = exp ? (const char*)exp : "";
	    row.result_json = res ? (const char*)res : "";
	    row.error = etxt ? (const char*)etxt : "";
	    out_rows->push_back(std::move(row));
	  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::get_workflow_scheduler_stats(
  int64_t now_unix_ms,
  WorkflowSchedulerStats* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = WorkflowSchedulerStats{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_stats) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const int64_t now = (now_unix_ms > 0) ? now_unix_ms : unix_ms_now();
  out_stats->now_unix_ms = now;

  auto read_group_counts = [&](const char* sql, std::map<std::string, int64_t>* out) -> bool {
    if (!out) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = sqlite_err(db_);
      return false;
    }
    bool ok = true;
    while (ok && step_row(st)) {
      const unsigned char* s = sqlite3_column_text(st, 0);
      const std::string key = s ? (const char*)s : "";
      const int64_t cnt = sqlite3_column_int64(st, 1);
      if (!key.empty()) (*out)[key] = cnt;
    }
    if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return ok;
  };

  if (!read_group_counts("SELECT status, COUNT(1) FROM workflows GROUP BY status;", &out_stats->workflows_by_status)) {
    return false;
  }
  if (!read_group_counts("SELECT status, COUNT(1) FROM workflow_tasks GROUP BY status;", &out_stats->tasks_by_status)) {
    return false;
  }

  // queued ready / not-ready split (useful for backpressure/debugging).
  {
    sqlite3_stmt* st = nullptr;
    const char* sql_ready = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks
WHERE status='queued' AND (ready_unix_ms IS NULL OR ready_unix_ms<=?);
)SQL";
    if (sqlite3_prepare_v2(db_, sql_ready, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = sqlite_err(db_);
      return false;
    }
    bool ok = true;
    ok = ok && bind_i64(st, 1, now);
    if (ok && sqlite3_step(st) == SQLITE_ROW) {
      out_stats->tasks_queued_ready = sqlite3_column_int64(st, 0);
    } else if (!ok) {
      if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    sqlite3_finalize(st);
  }
  {
    sqlite3_stmt* st = nullptr;
    const char* sql_not_ready = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks
WHERE status='queued' AND ready_unix_ms IS NOT NULL AND ready_unix_ms>?;
)SQL";
    if (sqlite3_prepare_v2(db_, sql_not_ready, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = sqlite_err(db_);
      return false;
    }
    bool ok = true;
    ok = ok && bind_i64(st, 1, now);
    if (ok && sqlite3_step(st) == SQLITE_ROW) {
      out_stats->tasks_queued_not_ready = sqlite3_column_int64(st, 0);
    } else if (!ok) {
      if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    sqlite3_finalize(st);
  }

  return true;
#endif
}

bool AgentDb::upsert_workflow(const WorkflowRow& wf, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty()) {
    if (out_error) *out_error = "upsert_workflow: workflow_id is empty";
    return false;
  }
  const int64_t now = wf.updated_unix_ms > 0 ? wf.updated_unix_ms : unix_ms_now();
  const int64_t created = wf.created_unix_ms > 0 ? wf.created_unix_ms : now;
  const std::string status = wf.status.empty() ? "queued" : wf.status;
  const std::string spec = wf.spec_json.empty() ? "{}" : wf.spec_json;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	INSERT INTO workflows(
	  workflow_id, session_id, trace_id, priority, created_unix_ms, updated_unix_ms, status, cancel_requested, error, spec_json, result_json
	) VALUES(?,?,?,?,?,?,?,?,?,?,?)
	ON CONFLICT(workflow_id) DO UPDATE SET
	  session_id = CASE WHEN excluded.session_id IS NOT NULL AND excluded.session_id <> '' THEN excluded.session_id ELSE workflows.session_id END,
	  trace_id = CASE WHEN excluded.trace_id IS NOT NULL AND excluded.trace_id <> '' THEN excluded.trace_id ELSE workflows.trace_id END,
	  priority = CASE WHEN excluded.priority IS NOT NULL THEN excluded.priority ELSE workflows.priority END,
	  updated_unix_ms = excluded.updated_unix_ms,
	  status = excluded.status,
	  cancel_requested = excluded.cancel_requested,
	  error = excluded.error,
	  spec_json = excluded.spec_json,
	  result_json = excluded.result_json;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
	bool ok = true;
	ok = ok && bind_text(st, 1, wf.workflow_id);
	ok = ok && bind_text_or_null(st, 2, wf.session_id);
	ok = ok && bind_text(st, 3, wf.trace_id);
	ok = ok && bind_i32_or_null(st, 4, wf.priority);
	ok = ok && bind_i64(st, 5, created);
	ok = ok && bind_i64(st, 6, now);
	ok = ok && bind_text(st, 7, status);
	ok = ok && bind_i32(st, 8, wf.cancel_requested ? 1 : 0);
	ok = ok && bind_text(st, 9, wf.error);
	ok = ok && bind_text(st, 10, spec);
	ok = ok && bind_text(st, 11, wf.result_json);
	ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::upsert_workflow_task(const WorkflowTaskRow& task, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (task.workflow_id.empty() || task.task_id.empty()) {
    if (out_error) *out_error = "upsert_workflow_task: workflow_id/task_id empty";
    return false;
  }
  const int64_t now = task.updated_unix_ms > 0 ? task.updated_unix_ms : unix_ms_now();
  const int64_t created = task.created_unix_ms > 0 ? task.created_unix_ms : now;
  const std::string status = task.status.empty() ? "queued" : task.status;
  const int attempt = task.attempt < 0 ? 0 : task.attempt;
  const int max_attempts = task.max_attempts <= 0 ? 1 : task.max_attempts;
  const int64_t ready = task.ready_unix_ms > 0 ? task.ready_unix_ms : 0;
  const int64_t started = task.started_unix_ms > 0 ? task.started_unix_ms : 0;
  const int64_t finished = task.finished_unix_ms > 0 ? task.finished_unix_ms : 0;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
	const char* sql = R"SQL(
	INSERT INTO workflow_tasks(
	  workflow_id, task_id, priority, created_unix_ms, updated_unix_ms, status, allow_error, attempt, max_attempts, ready_unix_ms,
	  started_unix_ms, finished_unix_ms, depends_on_json, request_json, expect_json, result_json, error
	) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
	ON CONFLICT(workflow_id, task_id) DO UPDATE SET
	  priority = CASE WHEN excluded.priority IS NOT NULL THEN excluded.priority ELSE workflow_tasks.priority END,
	  updated_unix_ms = excluded.updated_unix_ms,
	  status = excluded.status,
	  allow_error = excluded.allow_error,
	  attempt = excluded.attempt,
	  max_attempts = excluded.max_attempts,
	  ready_unix_ms = excluded.ready_unix_ms,
	  started_unix_ms = excluded.started_unix_ms,
	  finished_unix_ms = excluded.finished_unix_ms,
	  depends_on_json = excluded.depends_on_json,
	  request_json = excluded.request_json,
	  expect_json = excluded.expect_json,
	  result_json = excluded.result_json,
	  error = excluded.error;
	)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
	bool ok = true;
	ok = ok && bind_text(st, 1, task.workflow_id);
	ok = ok && bind_text(st, 2, task.task_id);
	ok = ok && bind_i32_or_null(st, 3, task.priority);
	ok = ok && bind_i64(st, 4, created);
	ok = ok && bind_i64(st, 5, now);
	ok = ok && bind_text(st, 6, status);
	ok = ok && bind_i32(st, 7, task.allow_error ? 1 : 0);
	ok = ok && bind_i32(st, 8, attempt);
	ok = ok && bind_i32(st, 9, max_attempts);
	ok = ok && bind_i64(st, 10, ready);
	ok = ok && bind_i64(st, 11, started);
	ok = ok && bind_i64(st, 12, finished);
	ok = ok && bind_text(st, 13, task.depends_on_json);
	ok = ok && bind_text(st, 14, task.request_json);
	ok = ok && bind_text(st, 15, task.expect_json);
	ok = ok && bind_text(st, 16, task.result_json);
	ok = ok && bind_text(st, 17, task.error);
	ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::claim_workflow_task(
  const std::string& workflow_id,
  const std::string& task_id,
  int64_t now_unix_ms,
  int new_attempt,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)task_id;
  (void)now_unix_ms;
  (void)new_attempt;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || task_id.empty()) {
    if (out_error) *out_error = "claim_workflow_task: workflow_id/task_id empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE workflow_tasks
SET
  status='running',
  updated_unix_ms=?,
  started_unix_ms=CASE WHEN started_unix_ms IS NULL OR started_unix_ms=0 THEN ? ELSE started_unix_ms END,
  attempt=?
WHERE workflow_id=? AND task_id=? AND status='queued';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : unix_ms_now();
  bool ok = true;
  ok = ok && bind_i64(st, 1, now);
  ok = ok && bind_i64(st, 2, now);
  ok = ok && bind_i32(st, 3, new_attempt < 0 ? 0 : new_attempt);
  ok = ok && bind_text(st, 4, workflow_id);
  ok = ok && bind_text(st, 5, task_id);
  ok = ok && step_done(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  const int changed = sqlite3_changes(db_);
  sqlite3_finalize(st);
  return changed > 0;
#endif
}

bool AgentDb::claim_workflow_task_budgeted(
  const std::string& workflow_id,
  const std::string& task_id,
  int64_t now_unix_ms,
  int new_attempt,
  int max_inflight_per_workflow,
  int max_inflight_per_session,
  const std::string& session_id,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)task_id;
  (void)now_unix_ms;
  (void)new_attempt;
  (void)max_inflight_per_workflow;
  (void)max_inflight_per_session;
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || task_id.empty()) {
    if (out_error) *out_error = "claim_workflow_task_budgeted: workflow_id/task_id empty";
    return false;
  }
  if (max_inflight_per_workflow <= 0 && max_inflight_per_session <= 0) {
    return claim_workflow_task(workflow_id, task_id, now_unix_ms, new_attempt, out_error);
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  auto count_i64 = [&](const char* sql, std::function<bool(sqlite3_stmt*)> binder, int64_t* out) -> bool {
    if (!out) return false;
    *out = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
      return false;
    }
    bool ok = true;
    if (binder) ok = ok && binder(st);
    if (ok) {
      const int rc = sqlite3_step(st);
      if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int64(st, 0);
        ok = true;
      } else if (rc == SQLITE_DONE) {
        *out = 0;
        ok = true;
      } else {
        ok = false;
      }
    }
    if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return ok;
  };

  if (max_inflight_per_workflow > 0) {
    int64_t running = 0;
    const char* sql = "SELECT COUNT(1) FROM workflow_tasks WHERE workflow_id=? AND status='running';";
    if (!count_i64(sql, [&](sqlite3_stmt* st) { return bind_text(st, 1, workflow_id); }, &running)) {
      return false;
    }
    if (running >= (int64_t)max_inflight_per_workflow) {
      // Budget exceeded: not an error; just don't claim.
      return false;
    }
  }

  if (max_inflight_per_session > 0 && !session_id.empty()) {
    int64_t running = 0;
    const char* sql = R"SQL(
SELECT COUNT(1)
FROM workflow_tasks t
JOIN workflows w ON w.workflow_id = t.workflow_id
WHERE w.session_id=? AND t.status='running';
)SQL";
    if (!count_i64(sql, [&](sqlite3_stmt* st) { return bind_text(st, 1, session_id); }, &running)) {
      return false;
    }
    if (running >= (int64_t)max_inflight_per_session) {
      // Budget exceeded: not an error; just don't claim.
      return false;
    }
  }

  // Claim the task (same semantics as claim_workflow_task).
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE workflow_tasks
SET
  status='running',
  updated_unix_ms=?,
  started_unix_ms=CASE WHEN started_unix_ms IS NULL OR started_unix_ms=0 THEN ? ELSE started_unix_ms END,
  attempt=?
WHERE workflow_id=? AND task_id=? AND status='queued';
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : unix_ms_now();
  bool ok = true;
  ok = ok && bind_i64(st, 1, now);
  ok = ok && bind_i64(st, 2, now);
  ok = ok && bind_i32(st, 3, new_attempt < 0 ? 0 : new_attempt);
  ok = ok && bind_text(st, 4, workflow_id);
  ok = ok && bind_text(st, 5, task_id);
  ok = ok && step_done(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  const int changed = sqlite3_changes(db_);
  sqlite3_finalize(st);
  return changed > 0;
#endif
}

bool AgentDb::get_scene_state(
  const std::string& session_id,
  std::string* out_scene_json,
  int64_t* out_updated_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_scene_json) out_scene_json->clear();
  if (out_updated_unix_ms) *out_updated_unix_ms = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (session_id.empty()) {
    if (out_error) *out_error = "get_scene_state: session_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT updated_unix_ms, scene_json FROM scene_states WHERE session_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, session_id);
  if (ok && step_row(st)) {
    if (out_updated_unix_ms) *out_updated_unix_ms = sqlite3_column_int64(st, 0);
    const unsigned char* txt = sqlite3_column_text(st, 1);
    if (out_scene_json && txt) *out_scene_json = (const char*)txt;
  } else {
    // Missing row is not an error; treat as empty scene.
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::put_scene_state(
  const std::string& session_id,
  const std::string& scene_json,
  int64_t updated_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)scene_json;
  (void)updated_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (session_id.empty()) {
    if (out_error) *out_error = "put_scene_state: session_id is empty";
    return false;
  }
  if (scene_json.empty()) {
    if (out_error) *out_error = "put_scene_state: scene_json is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  // Ensure parent session row exists (FK target).
  if (!upsert_session_locked(session_id, updated_unix_ms > 0 ? updated_unix_ms : unix_ms_now(), out_error)) {
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO scene_states(session_id, updated_unix_ms, scene_json) "
    "VALUES(?,?,?) "
    "ON CONFLICT(session_id) DO UPDATE SET updated_unix_ms=excluded.updated_unix_ms, scene_json=excluded.scene_json;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, session_id);
  ok = ok && bind_i64(st, 2, updated_unix_ms > 0 ? updated_unix_ms : unix_ms_now());
  ok = ok && bind_text(st, 3, scene_json);
  if (ok) {
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
      ok = false;
      if (out_error) *out_error = sqlite_err(db_);
    }
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::meta_get(const std::string& key, std::string* out_value, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_value) out_value->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)key;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT value FROM meta WHERE key=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, key);
  std::string v;
  if (ok && step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    if (txt) v = (const char*)txt;
  } else {
    // Missing key is not an error.
    ok = true;
  }
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (out_value) *out_value = v;
  return ok;
#endif
}

bool AgentDb::meta_set(const std::string& key, const std::string& value, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)key;
  (void)value;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const bool ok = bind_text(st, 1, key) && bind_text(st, 2, value) && step_done(st);
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::load_session_messages(
  const std::string& session_id,
  std::vector<std::pair<std::string, std::string>>* out_role_and_content,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_role_and_content) out_role_and_content->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT role, content FROM messages WHERE session_id=? ORDER BY idx ASC;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, session_id);
  std::vector<std::pair<std::string, std::string>> rows;
  while (ok && step_row(st)) {
    const unsigned char* role = sqlite3_column_text(st, 0);
    const unsigned char* content = sqlite3_column_text(st, 1);
    rows.emplace_back(role ? (const char*)role : "", content ? (const char*)content : "");
    if (rows.size() > 200000) {
      ok = false;
      if (out_error) *out_error = "too many messages";
      break;
    }
  }
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_role_and_content) *out_role_and_content = std::move(rows);
  return ok;
#endif
}

bool AgentDb::list_sessions(std::vector<std::string>* out_session_ids_desc, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_session_ids_desc) out_session_ids_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT session_id FROM sessions ORDER BY updated_unix_ms DESC LIMIT 5000;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  std::vector<std::string> ids;
  while (step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    if (txt) ids.push_back((const char*)txt);
  }
  sqlite3_finalize(st);
  if (out_session_ids_desc) *out_session_ids_desc = std::move(ids);
  return true;
#endif
}

bool AgentDb::insert_audit_record(
  const std::string& session_id,
  int64_t ts_unix_ms,
  int64_t run_id,
  const std::string& record_json,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)ts_unix_ms;
  (void)run_id;
  (void)record_json;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  if (!upsert_session_locked(session_id, ts_unix_ms, out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO audit_records(session_id, ts_unix_ms, run_id, record_json) VALUES(?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  const bool ok =
    bind_text(st, 1, session_id) &&
    bind_i64(st, 2, ts_unix_ms) &&
    ((run_id > 0) ? bind_i64(st, 3, run_id) : (sqlite3_bind_null(st, 3) == SQLITE_OK)) &&
    bind_text(st, 4, record_json) &&
    step_done(st);
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  if (!exec_locked("COMMIT;", out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  return true;
#endif
}

bool AgentDb::read_audit_records_tail(
  const std::string& session_id,
  size_t max_bytes,
  size_t max_records,
  std::vector<std::string>* out_record_json_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_record_json_desc) out_record_json_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)max_bytes;
  (void)max_records;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (max_bytes == 0) max_bytes = 1024 * 1024;
  if (max_records == 0) max_records = 200;
  if (max_records > 2000) max_records = 2000;

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT record_json FROM audit_records WHERE session_id=? ORDER BY ts_unix_ms DESC, id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, session_id) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<std::string> rows;
  size_t used = 0;
  while (ok && step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    const std::string s = txt ? (const char*)txt : "";
    if (s.empty()) continue;
    if (used + s.size() > max_bytes) break;
    used += s.size();
    rows.push_back(s);
  }
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_record_json_desc) *out_record_json_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::read_audit_records_by_trace_id(
  const std::string& trace_id,
  size_t max_bytes,
  size_t max_records,
  std::vector<std::string>* out_record_json_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_record_json_desc) out_record_json_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)trace_id;
  (void)max_bytes;
  (void)max_records;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (trace_id.empty()) {
    if (out_error) *out_error = "trace_id is empty";
    return false;
  }
  if (max_bytes == 0) max_bytes = 1024 * 1024;
  if (max_records == 0) max_records = 200;
  if (max_records > 2000) max_records = 2000;

  // Best-effort JSON search:
  // - audit records are stored as compact JSON (indentation=""), so the substring pattern is stable.
  // - This intentionally avoids a schema migration and is good enough for debugging tools.
  //
  // Pattern example: %"trace_id":"trace_xxx"%
  //
  // Escape LIKE wildcards so trace_ids containing '_' don't become false-positive patterns.
  auto escape_like = [](std::string s) -> std::string {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
      if (c == '%' || c == '_' || c == '\\') out.push_back('\\');
      out.push_back(c);
    }
    return out;
  };
  const std::string needle = std::string("%\"trace_id\":\"") + escape_like(trace_id) + "\"%";

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT record_json FROM audit_records WHERE record_json LIKE ? ESCAPE '\\' "
    "ORDER BY ts_unix_ms DESC, id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, needle) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<std::string> rows;
  size_t used = 0;
  while (ok && step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    const std::string s = txt ? (const char*)txt : "";
    if (s.empty()) continue;
    if (used + s.size() > max_bytes) break;
    used += s.size();
    rows.push_back(s);
  }
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_record_json_desc) *out_record_json_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::list_artifacts_by_session(
  const std::string& session_id,
  size_t max_artifacts,
  std::vector<ArtifactRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)max_artifacts;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (max_artifacts == 0) max_artifacts = 64;
  if (max_artifacts > 512) max_artifacts = 512;

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT run_id, ts_unix_ms, session_id, tool_call_id, path, kind, mime, title, autoplay, repeat, artifact_json "
    "FROM artifacts WHERE session_id=? ORDER BY ts_unix_ms DESC, id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, session_id) && sqlite3_bind_int(st, 2, (int)max_artifacts) == SQLITE_OK;
  std::vector<ArtifactRow> rows;
  while (ok && step_row(st)) {
    ArtifactRow r;
    r.run_id = sqlite3_column_int64(st, 0);
    r.ts_unix_ms = sqlite3_column_int64(st, 1);
    {
      const unsigned char* txt = sqlite3_column_text(st, 2);
      if (txt) r.session_id = (const char*)txt;
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 3);
      if (txt) r.tool_call_id = (const char*)txt;
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 4);
      if (txt) r.path = (const char*)txt;
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 5);
      if (txt) r.kind = (const char*)txt;
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 6);
      if (txt) r.mime = (const char*)txt;
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 7);
      if (txt) r.title = (const char*)txt;
    }
    r.autoplay = sqlite3_column_int(st, 8) != 0;
    r.repeat = sqlite3_column_int(st, 9);
    {
      const unsigned char* txt = sqlite3_column_text(st, 10);
      if (txt) r.artifact_json = (const char*)txt;
    }
    rows.push_back(std::move(r));
  }
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_rows_desc) *out_rows_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::read_client_events_tail_jsonl(
  const std::string& session_id,
  size_t max_bytes,
  size_t max_events,
  std::string* out_jsonl,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_jsonl) out_jsonl->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)max_bytes;
  (void)max_events;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (!out_jsonl) {
    if (out_error) *out_error = "missing out_jsonl";
    return false;
  }
  if (max_bytes == 0) max_bytes = 256 * 1024;
  if (max_bytes > 1024 * 1024) max_bytes = 1024 * 1024;
  if (max_events == 0) max_events = 2000;
  if (max_events > 5000) max_events = 5000;

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT data_json FROM client_events WHERE session_id=? ORDER BY ts_unix_ms DESC, id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, session_id) && sqlite3_bind_int(st, 2, (int)max_events) == SQLITE_OK;
  std::vector<std::string> lines_desc;
  size_t used = 0;
  while (ok && step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    const std::string s = txt ? (const char*)txt : "";
    if (s.empty()) continue;
    const size_t add = s.size() + 1;
    if (used + add > max_bytes) break;
    used += add;
    lines_desc.push_back(s);
  }
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;

  // Convert to JSONL in chronological order (oldest first).
  std::ostringstream oss;
  for (auto it = lines_desc.rbegin(); it != lines_desc.rend(); ++it) {
    oss << *it << "\n";
  }
  *out_jsonl = oss.str();
  return true;
#endif
}

bool AgentDb::upsert_session(const std::string& session_id, int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  return upsert_session_locked(session_id, now_unix_ms, out_error);
#endif
}

bool AgentDb::session_exists(const std::string& session_id, bool* out_exists, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_exists) *out_exists = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT 1 FROM sessions WHERE session_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, session_id);
  bool exists = false;
  if (ok && step_row(st)) {
    exists = true;
  }
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (out_exists) *out_exists = exists;
  return ok;
#endif
}

bool AgentDb::upsert_session_locked(const std::string& session_id, int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO sessions(session_id, created_unix_ms, updated_unix_ms) VALUES(?,?,?) "
    "ON CONFLICT(session_id) DO UPDATE SET updated_unix_ms=excluded.updated_unix_ms;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const bool ok =
    bind_text(st, 1, session_id) &&
    bind_i64(st, 2, now_unix_ms) &&
    bind_i64(st, 3, now_unix_ms) &&
    step_done(st);
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::replace_session_messages(
  const std::string& session_id,
  const std::vector<std::pair<std::string, std::string>>& role_and_content,
  int64_t now_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)role_and_content;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Transaction.
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;

  if (!upsert_session_locked(session_id, now_unix_ms, out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE session_id=?;", -1, &del, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = sqlite_err(db_);
      (void)exec_locked("ROLLBACK;", nullptr);
      return false;
    }
    const bool ok = bind_text(del, 1, session_id) && step_done(del);
    if (!ok && out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(del);
    if (!ok) {
      (void)exec_locked("ROLLBACK;", nullptr);
      return false;
    }
  }

  sqlite3_stmt* ins = nullptr;
  const char* sql = "INSERT INTO messages(session_id, idx, role, content, created_unix_ms) VALUES(?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &ins, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < role_and_content.size(); i++) {
    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    ok = ok &&
      bind_text(ins, 1, session_id) &&
      bind_i32(ins, 2, (int)i) &&
      bind_text(ins, 3, role_and_content[i].first) &&
      bind_text(ins, 4, role_and_content[i].second) &&
      bind_i64(ins, 5, now_unix_ms) &&
      step_done(ins);
    if (!ok) {
      if (out_error) *out_error = sqlite_err(db_);
      break;
    }
  }
  sqlite3_finalize(ins);

  if (!ok) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  if (!exec_locked("COMMIT;", out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  return true;
#endif
}

bool AgentDb::delete_session(const std::string& session_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE session_id=?;", -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const bool ok = bind_text(st, 1, session_id) && step_done(st);
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_run(const RunRow& row, int64_t* out_run_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_run_id) *out_run_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO runs(session_id, job_id, ts_unix_ms, prompt, tools, model, base_url, stream_assistant, ok, "
    "stop_reason, steps_executed, tool_calls_total, tool_calls_by_tool_json, last_error_reason, "
    "error, http_status, http_body) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  bool ok =
    bind_text(st, 1, row.session_id) &&
    bind_text(st, 2, row.job_id) &&
    bind_i64(st, 3, row.ts_unix_ms) &&
    bind_text(st, 4, row.prompt) &&
    bind_text(st, 5, row.tools) &&
    bind_text(st, 6, row.model) &&
    bind_text(st, 7, row.base_url) &&
    bind_i32(st, 8, row.stream_assistant ? 1 : 0) &&
    bind_i32(st, 9, row.ok ? 1 : 0) &&
    bind_text(st, 10, row.stop_reason) &&
    bind_i64(st, 11, row.steps_executed) &&
    bind_i64(st, 12, row.tool_calls_total) &&
    bind_text(st, 13, row.tool_calls_by_tool_json) &&
    bind_text(st, 14, row.last_error_reason) &&
    bind_text(st, 15, row.error) &&
    bind_i32(st, 16, (int)row.http_status) &&
    bind_text(st, 17, row.http_body) &&
    step_done(st);

  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  const int64_t run_id = (int64_t)sqlite3_last_insert_rowid(db_);
  if (out_run_id) *out_run_id = run_id;

  if (!exec_locked("COMMIT;", out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  return true;
#endif
}

bool AgentDb::insert_event(
  int64_t run_id,
  int64_t ts_unix_ms,
  const std::string& type,
  const std::string& data_json,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)run_id;
  (void)ts_unix_ms;
  (void)type;
  (void)data_json;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO events(run_id, ts_unix_ms, type, data_json) VALUES(?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const bool ok =
    bind_i64(st, 1, run_id) &&
    bind_i64(st, 2, ts_unix_ms) &&
    bind_text(st, 3, type) &&
    bind_text(st, 4, data_json) &&
    step_done(st);
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_tool_record(const ToolRecordRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO tool_records(run_id, tool_name, tool_call_id, arguments_json, result_text, result_for_prompt_text, result_truncated_for_prompt) "
    "VALUES(?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  const bool ok =
    bind_i64(st, 1, row.run_id) &&
    bind_text(st, 2, row.tool_name) &&
    bind_text(st, 3, row.tool_call_id) &&
    bind_text(st, 4, row.arguments_json) &&
    bind_text(st, 5, row.result_text) &&
    bind_text(st, 6, row.result_for_prompt_text) &&
    bind_i32(st, 7, row.result_truncated_for_prompt ? 1 : 0) &&
    step_done(st);
  if (!ok && out_error) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_artifact(const ArtifactRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "insert_artifact: session_id is empty";
    return false;
  }
  if (row.path.empty()) {
    if (out_error) *out_error = "insert_artifact: path is empty";
    return false;
  }
  if (row.artifact_json.empty()) {
    if (out_error) *out_error = "insert_artifact: artifact_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Ensure session row exists (for foreign key).
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    return false;
  }

  const char* sql =
    "INSERT INTO artifacts(run_id, ts_unix_ms, session_id, tool_call_id, path, kind, mime, title, autoplay, repeat, artifact_json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && bind_i64(st, 1, row.run_id);
  ok = ok && bind_i64(st, 2, row.ts_unix_ms);
  ok = ok && bind_text(st, 3, row.session_id);
  // tool_call_id may be empty; store NULL in that case for cleanliness.
  if (row.tool_call_id.empty()) {
    ok = ok && (sqlite3_bind_null(st, 4) == SQLITE_OK);
  } else {
    ok = ok && bind_text(st, 4, row.tool_call_id);
  }
  ok = ok && bind_text(st, 5, row.path);
  ok = ok && (row.kind.empty() ? (sqlite3_bind_null(st, 6) == SQLITE_OK) : bind_text(st, 6, row.kind));
  ok = ok && (row.mime.empty() ? (sqlite3_bind_null(st, 7) == SQLITE_OK) : bind_text(st, 7, row.mime));
  ok = ok && (row.title.empty() ? (sqlite3_bind_null(st, 8) == SQLITE_OK) : bind_text(st, 8, row.title));
  ok = ok && bind_i32(st, 9, row.autoplay ? 1 : 0);
  ok = ok && bind_i32(st, 10, row.repeat);
  ok = ok && bind_text(st, 11, row.artifact_json);

  if (!ok) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!step_done(st)) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::insert_ui_action(const UiActionRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "insert_ui_action: session_id is empty";
    return false;
  }
  if (row.action_json.empty()) {
    if (out_error) *out_error = "insert_ui_action: action_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Ensure session row exists (for foreign key).
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    return false;
  }

  const char* sql =
    "INSERT INTO ui_actions(run_id, ts_unix_ms, session_id, tool_call_id, type, title, message, path, mime, autoplay, repeat, action_json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && bind_i64(st, 1, row.run_id);
  ok = ok && bind_i64(st, 2, row.ts_unix_ms);
  ok = ok && bind_text(st, 3, row.session_id);
  if (row.tool_call_id.empty()) ok = ok && (sqlite3_bind_null(st, 4) == SQLITE_OK);
  else ok = ok && bind_text(st, 4, row.tool_call_id);
  ok = ok && (row.type.empty() ? (sqlite3_bind_null(st, 5) == SQLITE_OK) : bind_text(st, 5, row.type));
  ok = ok && (row.title.empty() ? (sqlite3_bind_null(st, 6) == SQLITE_OK) : bind_text(st, 6, row.title));
  ok = ok && (row.message.empty() ? (sqlite3_bind_null(st, 7) == SQLITE_OK) : bind_text(st, 7, row.message));
  ok = ok && (row.path.empty() ? (sqlite3_bind_null(st, 8) == SQLITE_OK) : bind_text(st, 8, row.path));
  ok = ok && (row.mime.empty() ? (sqlite3_bind_null(st, 9) == SQLITE_OK) : bind_text(st, 9, row.mime));
  ok = ok && bind_i32(st, 10, row.autoplay ? 1 : 0);
  ok = ok && bind_i32(st, 11, row.repeat);
  ok = ok && bind_text(st, 12, row.action_json);

  if (!ok) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!step_done(st)) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::insert_client_event(const ClientEventRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "insert_client_event: session_id is empty";
    return false;
  }
  if (row.type.empty()) {
    if (out_error) *out_error = "insert_client_event: type is empty";
    return false;
  }
  if (row.data_json.empty()) {
    if (out_error) *out_error = "insert_client_event: data_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Ensure session row exists (for foreign key).
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    return false;
  }

  const char* sql = "INSERT INTO client_events(ts_unix_ms, session_id, type, data_json) VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && bind_i64(st, 1, row.ts_unix_ms);
  ok = ok && bind_text(st, 2, row.session_id);
  ok = ok && bind_text(st, 3, row.type);
  ok = ok && bind_text(st, 4, row.data_json);

  if (!ok) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!step_done(st)) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::insert_workflow_event(const WorkflowEventRow& row, int64_t* out_event_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_event_id) *out_event_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.workflow_id.empty()) {
    if (out_error) *out_error = "insert_workflow_event: workflow_id is empty";
    return false;
  }
  if (row.type.empty()) {
    if (out_error) *out_error = "insert_workflow_event: type is empty";
    return false;
  }
  if (row.data_json.empty()) {
    if (out_error) *out_error = "insert_workflow_event: data_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const char* sql = "INSERT INTO workflow_events(workflow_id, task_id, ts_unix_ms, type, data_json) VALUES(?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  const int64_t ts = row.ts_unix_ms > 0 ? row.ts_unix_ms : unix_ms_now();
  bool ok = true;
  ok = ok && bind_text(st, 1, row.workflow_id);
  ok = ok && bind_text_or_null(st, 2, row.task_id);
  ok = ok && bind_i64(st, 3, ts);
  ok = ok && bind_text(st, 4, row.type);
  ok = ok && bind_text(st, 5, row.data_json);

  if (!ok) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!step_done(st)) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  const int64_t eid = (int64_t)sqlite3_last_insert_rowid(db_);
  sqlite3_finalize(st);
  if (out_event_id) *out_event_id = eid;
  return true;
#endif
}

bool AgentDb::list_workflow_events(
  const std::string& workflow_id,
  int64_t after_event_id,
  size_t max_rows,
  std::vector<WorkflowEventRow>* out_rows_asc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_asc) out_rows_asc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)after_event_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_workflow_events: workflow_id is empty";
    return false;
  }
  if (!out_rows_asc) return false;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const size_t lim = std::min<size_t>(max_rows == 0 ? 256 : max_rows, 1000);
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT event_id, workflow_id, task_id, ts_unix_ms, type, data_json
FROM workflow_events
WHERE workflow_id=? AND event_id>?
ORDER BY event_id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && bind_text(st, 1, workflow_id);
  ok = ok && bind_i64(st, 2, after_event_id < 0 ? 0 : after_event_id);
  ok = ok && bind_i64(st, 3, (int64_t)lim);
  if (!ok) {
    if (out_error) *out_error = sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  while (step_row(st)) {
    WorkflowEventRow r;
    r.event_id = sqlite3_column_int64(st, 0);
    const unsigned char* wid = sqlite3_column_text(st, 1);
    const unsigned char* tid = sqlite3_column_text(st, 2);
    r.workflow_id = wid ? (const char*)wid : "";
    r.task_id = tid ? (const char*)tid : "";
    r.ts_unix_ms = sqlite3_column_int64(st, 3);
    const unsigned char* type = sqlite3_column_text(st, 4);
    const unsigned char* dj = sqlite3_column_text(st, 5);
    r.type = type ? (const char*)type : "";
    r.data_json = dj ? (const char*)dj : "";
    out_rows_asc->push_back(std::move(r));
    if (out_rows_asc->size() >= lim) break;
  }

  sqlite3_finalize(st);
  return true;
#endif
}

}  // namespace agentd
