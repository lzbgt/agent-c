#include "agent_db.h"

#include <chrono>
#include <filesystem>
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

static bool bind_i64(sqlite3_stmt* st, int idx, int64_t v) {
  return sqlite3_bind_int64(st, idx, (sqlite3_int64)v) == SQLITE_OK;
}

static bool bind_i32(sqlite3_stmt* st, int idx, int v) {
  return sqlite3_bind_int(st, idx, v) == SQLITE_OK;
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
  const int kSchemaVersion = 8;

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
  job_id, session_id, created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
) VALUES(?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(job_id) DO UPDATE SET
  session_id = CASE
    WHEN excluded.session_id IS NOT NULL AND excluded.session_id <> '' THEN excluded.session_id
    ELSE jobs.session_id
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
  ok = ok && bind_text(st, 2, row.session_id);
  ok = ok && bind_i64(st, 3, created);
  ok = ok && bind_i64(st, 4, now);
  ok = ok && bind_text(st, 5, status);
  ok = ok && bind_i32(st, 6, row.cancel_requested ? 1 : 0);
  ok = ok && bind_text(st, 7, row.error);
  ok = ok && bind_text(st, 8, row.stop_reason);
  ok = ok && bind_text(st, 9, row.result_json);
  ok = ok && bind_i64(st, 10, row.last_heartbeat_unix_ms);

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
  job_id, session_id, created_unix_ms, updated_unix_ms, status, cancel_requested, error, stop_reason, result_json, last_heartbeat_unix_ms
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
    out_row->job_id = jid ? (const char*)jid : "";
    out_row->session_id = sid ? (const char*)sid : "";
    out_row->created_unix_ms = sqlite3_column_int64(st, 2);
    out_row->updated_unix_ms = sqlite3_column_int64(st, 3);
    const unsigned char* stxt = sqlite3_column_text(st, 4);
    out_row->status = stxt ? (const char*)stxt : "";
    out_row->cancel_requested = sqlite3_column_int(st, 5) != 0;
    const unsigned char* etxt = sqlite3_column_text(st, 6);
    out_row->error = etxt ? (const char*)etxt : "";
    const unsigned char* srtxt = sqlite3_column_text(st, 7);
    out_row->stop_reason = srtxt ? (const char*)srtxt : "";
    const unsigned char* rj = sqlite3_column_text(st, 8);
    out_row->result_json = rj ? (const char*)rj : "";
    out_row->last_heartbeat_unix_ms = sqlite3_column_int64(st, 9);
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

}  // namespace agentd
