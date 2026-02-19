#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <sstream>

namespace agentd {

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
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  const bool ok =
    agent_db_bind_text(st, 1, session_id) &&
    agent_db_bind_i64(st, 2, ts_unix_ms) &&
    ((run_id > 0) ? agent_db_bind_i64(st, 3, run_id) : (sqlite3_bind_null(st, 3) == SQLITE_OK)) &&
    agent_db_bind_text(st, 4, record_json) &&
    agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
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
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<std::string> rows;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    const std::string s = txt ? (const char*)txt : "";
    if (s.empty()) continue;
    if (used + s.size() > max_bytes) break;
    used += s.size();
    rows.push_back(s);
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
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
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, needle) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<std::string> rows;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    const std::string s = txt ? (const char*)txt : "";
    if (s.empty()) continue;
    if (used + s.size() > max_bytes) break;
    used += s.size();
    rows.push_back(s);
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_record_json_desc) *out_record_json_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::read_edge_task_events_by_trace_id(
  const std::string& trace_id,
  size_t max_bytes,
  size_t max_records,
  std::vector<EdgeTaskEventRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
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
    "SELECT id, task_id, step_id, ts_utc_ms, state, data_json "
    "FROM edge_task_events WHERE data_json LIKE ? ESCAPE '\\' "
    "ORDER BY ts_utc_ms DESC, id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, needle) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<EdgeTaskEventRow> rows;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    EdgeTaskEventRow r;
    r.id = (int64_t)sqlite3_column_int64(st, 0);
    {
      const unsigned char* txt = sqlite3_column_text(st, 1);
      r.task_id = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 2);
      r.step_id = txt ? (const char*)txt : "";
    }
    r.ts_utc_ms = (int64_t)sqlite3_column_int64(st, 3);
    {
      const unsigned char* txt = sqlite3_column_text(st, 4);
      r.state = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 5);
      r.data_json = txt ? (const char*)txt : "";
    }
    if (r.data_json.empty()) continue;
    if (used + r.data_json.size() > max_bytes) break;
    used += r.data_json.size();
    rows.push_back(std::move(r));
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_rows_desc) *out_rows_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::read_edge_inbox_messages_by_trace_id(
  const std::string& trace_id,
  size_t max_bytes,
  size_t max_records,
  std::vector<EdgeInboxMessageRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
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
    "SELECT msg_id, ts_utc_ms, type, from_id, to_id, envelope_json "
    "FROM edge_inbox_messages WHERE envelope_json LIKE ? ESCAPE '\\' "
    "ORDER BY ts_utc_ms DESC, msg_id DESC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, needle) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<EdgeInboxMessageRow> rows;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    EdgeInboxMessageRow r;
    {
      const unsigned char* txt = sqlite3_column_text(st, 0);
      r.msg_id = txt ? (const char*)txt : "";
    }
    r.ts_utc_ms = (int64_t)sqlite3_column_int64(st, 1);
    {
      const unsigned char* txt = sqlite3_column_text(st, 2);
      r.type = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 3);
      r.from_id = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 4);
      r.to_id = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 5);
      r.envelope_json = txt ? (const char*)txt : "";
    }
    if (r.envelope_json.empty()) continue;
    if (used + r.envelope_json.size() > max_bytes) break;
    used += r.envelope_json.size();
    rows.push_back(std::move(r));
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_rows_desc) *out_rows_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::read_workflow_events_by_trace_id(
  const std::string& trace_id,
  size_t max_bytes,
  size_t max_records,
  std::vector<WorkflowEventRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
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

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT e.event_id, e.workflow_id, e.task_id, e.ts_unix_ms, e.type, e.data_json
FROM workflow_events e
JOIN workflows w ON w.workflow_id = e.workflow_id
WHERE w.trace_id=?
ORDER BY e.ts_unix_ms DESC, e.event_id DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = agent_db_bind_text(st, 1, trace_id) && sqlite3_bind_int(st, 2, (int)max_records) == SQLITE_OK;
  std::vector<WorkflowEventRow> rows;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    WorkflowEventRow r;
    r.event_id = (int64_t)sqlite3_column_int64(st, 0);
    {
      const unsigned char* txt = sqlite3_column_text(st, 1);
      r.workflow_id = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 2);
      r.task_id = txt ? (const char*)txt : "";
    }
    r.ts_unix_ms = (int64_t)sqlite3_column_int64(st, 3);
    {
      const unsigned char* txt = sqlite3_column_text(st, 4);
      r.type = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 5);
      r.data_json = txt ? (const char*)txt : "";
    }
    if (r.data_json.empty()) continue;
    if (used + r.data_json.size() > max_bytes) break;
    used += r.data_json.size();
    rows.push_back(std::move(r));
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_rows_desc) *out_rows_desc = std::move(rows);
  return ok;
#endif
}

bool AgentDb::read_edge_workflow_events_by_trace_id(
  const std::string& trace_id,
  size_t max_bytes,
  size_t max_records,
  std::vector<EdgeWorkflowEventRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
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

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT id, workflow_id, ts_utc_ms, type, data_json
FROM edge_workflow_events
WHERE workflow_id=?
   OR (workflow_id IN (SELECT DISTINCT task_id FROM edge_tasks WHERE trace_id=?) AND workflow_id<>?)
ORDER BY ts_utc_ms DESC, id DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, trace_id);
  ok = ok && agent_db_bind_text(st, 2, trace_id);
  ok = ok && agent_db_bind_text(st, 3, trace_id);
  ok = ok && sqlite3_bind_int(st, 4, (int)max_records) == SQLITE_OK;
  std::vector<EdgeWorkflowEventRow> rows;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    EdgeWorkflowEventRow r;
    r.id = (int64_t)sqlite3_column_int64(st, 0);
    {
      const unsigned char* txt = sqlite3_column_text(st, 1);
      r.workflow_id = txt ? (const char*)txt : "";
    }
    r.ts_utc_ms = (int64_t)sqlite3_column_int64(st, 2);
    {
      const unsigned char* txt = sqlite3_column_text(st, 3);
      r.type = txt ? (const char*)txt : "";
    }
    {
      const unsigned char* txt = sqlite3_column_text(st, 4);
      r.data_json = txt ? (const char*)txt : "";
    }
    if (r.data_json.empty()) continue;
    if (used + r.data_json.size() > max_bytes) break;
    used += r.data_json.size();
    rows.push_back(std::move(r));
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_rows_desc) *out_rows_desc = std::move(rows);
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
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id) && sqlite3_bind_int(st, 2, (int)max_artifacts) == SQLITE_OK;
  std::vector<ArtifactRow> rows;
  while (ok && agent_db_step_row(st)) {
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
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
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
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id) && sqlite3_bind_int(st, 2, (int)max_events) == SQLITE_OK;
  std::vector<std::string> lines_desc;
  size_t used = 0;
  while (ok && agent_db_step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    const std::string s = txt ? (const char*)txt : "";
    if (s.empty()) continue;
    const size_t add = s.size() + 1;
    if (used + add > max_bytes) break;
    used += add;
    lines_desc.push_back(s);
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
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

}  // namespace agentd
