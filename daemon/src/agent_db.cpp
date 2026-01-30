#include "agent_db.h"

#include <filesystem>
#include <sstream>

namespace agentd {
namespace {

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>

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
  const int kSchemaVersion = 2;

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

  // Record schema version.
  {
    std::ostringstream oss;
    oss << "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version','" << kSchemaVersion << "');";
	  if (!exec_locked(oss.str(), out_error)) return false;
	  }
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
    "INSERT INTO runs(session_id, job_id, ts_unix_ms, prompt, tools, model, base_url, stream_assistant, ok, error, http_status, http_body) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
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
    bind_text(st, 10, row.error) &&
    bind_i32(st, 11, (int)row.http_status) &&
    bind_text(st, 12, row.http_body) &&
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

}  // namespace agentd
