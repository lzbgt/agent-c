#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <string>
#include <utility>
#include <vector>

namespace agentd {

bool AgentDb::upsert_edge_task(const EdgeTaskRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.task_id.empty() || row.step_id.empty()) {
    if (out_error) *out_error = "upsert_edge_task: task_id/step_id is empty";
    return false;
  }
  if (row.node_id.empty() || row.idempotency_key.empty() || row.mode.empty() || row.payload_json.empty() || row.state.empty()) {
    if (out_error) *out_error = "upsert_edge_task: missing required fields";
    return false;
  }
  const int64_t now = agent_db_unix_ms_now();
  const int64_t created = row.created_utc_ms > 0 ? row.created_utc_ms : now;
  const int64_t updated = row.updated_utc_ms > 0 ? row.updated_utc_ms : now;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_tasks(
  task_id, step_id, node_id, idempotency_key, trace_id, resource_lock, mode, tool_name, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, result_sha256, attest_json, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(task_id, step_id) DO UPDATE SET
  node_id = excluded.node_id,
  idempotency_key = excluded.idempotency_key,
  trace_id = CASE WHEN excluded.trace_id IS NOT NULL AND excluded.trace_id <> '' THEN excluded.trace_id ELSE edge_tasks.trace_id END,
  resource_lock = CASE WHEN excluded.resource_lock IS NOT NULL AND excluded.resource_lock <> '' THEN excluded.resource_lock ELSE edge_tasks.resource_lock END,
  mode = CASE WHEN excluded.mode IS NOT NULL AND excluded.mode <> '' THEN excluded.mode ELSE edge_tasks.mode END,
  tool_name = CASE WHEN excluded.tool_name IS NOT NULL AND excluded.tool_name <> '' THEN excluded.tool_name ELSE edge_tasks.tool_name END,
  deadline_utc_ms = CASE WHEN excluded.deadline_utc_ms > 0 THEN excluded.deadline_utc_ms ELSE edge_tasks.deadline_utc_ms END,
  payload_json = CASE WHEN excluded.payload_json IS NOT NULL AND excluded.payload_json <> '' THEN excluded.payload_json ELSE edge_tasks.payload_json END,
  state = excluded.state,
  created_utc_ms = edge_tasks.created_utc_ms,
  updated_utc_ms = excluded.updated_utc_ms,
  result_json = CASE WHEN excluded.result_json IS NOT NULL AND excluded.result_json <> '' THEN excluded.result_json ELSE edge_tasks.result_json END,
  result_sha256 = CASE WHEN excluded.result_sha256 IS NOT NULL AND excluded.result_sha256 <> '' THEN excluded.result_sha256 ELSE edge_tasks.result_sha256 END,
  attest_json = CASE WHEN excluded.attest_json IS NOT NULL AND excluded.attest_json <> '' THEN excluded.attest_json ELSE edge_tasks.attest_json END,
  error = CASE WHEN excluded.error IS NOT NULL AND excluded.error <> '' THEN excluded.error ELSE edge_tasks.error END;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.task_id);
  ok = ok && agent_db_bind_text(st, 2, row.step_id);
  ok = ok && agent_db_bind_text(st, 3, row.node_id);
  ok = ok && agent_db_bind_text(st, 4, row.idempotency_key);
  ok = ok && agent_db_bind_text_or_null(st, 5, row.trace_id);
  ok = ok && agent_db_bind_text_or_null(st, 6, row.resource_lock);
  ok = ok && agent_db_bind_text(st, 7, row.mode);
  ok = ok && agent_db_bind_text_or_null(st, 8, row.tool_name);
  ok = ok && agent_db_bind_i64(st, 9, row.deadline_utc_ms);
  ok = ok && agent_db_bind_text(st, 10, row.payload_json);
  ok = ok && agent_db_bind_text(st, 11, row.state);
  ok = ok && agent_db_bind_i64(st, 12, created);
  ok = ok && agent_db_bind_i64(st, 13, updated);
  ok = ok && agent_db_bind_text_or_null(st, 14, row.result_json);
  ok = ok && agent_db_bind_text_or_null(st, 15, row.result_sha256);
  ok = ok && agent_db_bind_text_or_null(st, 16, row.attest_json);
  ok = ok && agent_db_bind_text_or_null(st, 17, row.error);

  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_edge_task(const std::string& task_id, const std::string& step_id, EdgeTaskRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeTaskRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task_id;
  (void)step_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (task_id.empty() || step_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), COALESCE(resource_lock,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, COALESCE(result_sha256,''), COALESCE(attest_json,''), error
FROM edge_tasks
WHERE task_id=? AND step_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, task_id);
  ok = ok && agent_db_bind_text(st, 2, step_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* lock = sqlite3_column_text(st, 5);
    const unsigned char* m = sqlite3_column_text(st, 6);
    const unsigned char* tool = sqlite3_column_text(st, 7);
    const unsigned char* p = sqlite3_column_text(st, 9);
    const unsigned char* stt = sqlite3_column_text(st, 10);
    const unsigned char* r = sqlite3_column_text(st, 13);
    const unsigned char* rh = sqlite3_column_text(st, 14);
    const unsigned char* aj = sqlite3_column_text(st, 15);
    const unsigned char* e = sqlite3_column_text(st, 16);
    out_row->task_id = t ? (const char*)t : "";
    out_row->step_id = s ? (const char*)s : "";
    out_row->node_id = n ? (const char*)n : "";
    out_row->idempotency_key = k ? (const char*)k : "";
    out_row->trace_id = trc ? (const char*)trc : "";
    out_row->resource_lock = lock ? (const char*)lock : "";
    out_row->mode = m ? (const char*)m : "";
    out_row->tool_name = tool ? (const char*)tool : "";
    out_row->deadline_utc_ms = sqlite3_column_int64(st, 8);
    out_row->payload_json = p ? (const char*)p : "";
    out_row->state = stt ? (const char*)stt : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 11);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 12);
    out_row->result_json = r ? (const char*)r : "";
    out_row->result_sha256 = rh ? (const char*)rh : "";
    out_row->attest_json = aj ? (const char*)aj : "";
    out_row->error = e ? (const char*)e : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::get_edge_task_by_node_idempotency(
  const std::string& node_id,
  const std::string& idempotency_key,
  EdgeTaskRow* out_row,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeTaskRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  (void)idempotency_key;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (node_id.empty() || idempotency_key.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), COALESCE(resource_lock,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, COALESCE(result_sha256,''), COALESCE(attest_json,''), error
FROM edge_tasks
WHERE node_id=? AND idempotency_key=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, node_id);
  ok = ok && agent_db_bind_text(st, 2, idempotency_key);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* lock = sqlite3_column_text(st, 5);
    const unsigned char* m = sqlite3_column_text(st, 6);
    const unsigned char* tool = sqlite3_column_text(st, 7);
    const unsigned char* p = sqlite3_column_text(st, 9);
    const unsigned char* stt = sqlite3_column_text(st, 10);
    const unsigned char* r = sqlite3_column_text(st, 13);
    const unsigned char* rh = sqlite3_column_text(st, 14);
    const unsigned char* aj = sqlite3_column_text(st, 15);
    const unsigned char* e = sqlite3_column_text(st, 16);
    out_row->task_id = t ? (const char*)t : "";
    out_row->step_id = s ? (const char*)s : "";
    out_row->node_id = n ? (const char*)n : "";
    out_row->idempotency_key = k ? (const char*)k : "";
    out_row->trace_id = trc ? (const char*)trc : "";
    out_row->resource_lock = lock ? (const char*)lock : "";
    out_row->mode = m ? (const char*)m : "";
    out_row->tool_name = tool ? (const char*)tool : "";
    out_row->deadline_utc_ms = sqlite3_column_int64(st, 8);
    out_row->payload_json = p ? (const char*)p : "";
    out_row->state = stt ? (const char*)stt : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 11);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 12);
    out_row->result_json = r ? (const char*)r : "";
    out_row->result_sha256 = rh ? (const char*)rh : "";
    out_row->attest_json = aj ? (const char*)aj : "";
    out_row->error = e ? (const char*)e : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_edge_tasks_by_state(
  const std::string& state,
  size_t max_rows,
  std::vector<EdgeTaskRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)state;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (state.empty()) {
    if (out_error) *out_error = "list_edge_tasks_by_state: state is empty";
    return false;
  }
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), COALESCE(resource_lock,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, COALESCE(result_sha256,''), COALESCE(attest_json,''), error
FROM edge_tasks
WHERE state=?
ORDER BY updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, state);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeTaskRow row;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* lock = sqlite3_column_text(st, 5);
    const unsigned char* m = sqlite3_column_text(st, 6);
    const unsigned char* tool = sqlite3_column_text(st, 7);
    const unsigned char* p = sqlite3_column_text(st, 9);
    const unsigned char* stt = sqlite3_column_text(st, 10);
    const unsigned char* r = sqlite3_column_text(st, 13);
    const unsigned char* rh = sqlite3_column_text(st, 14);
    const unsigned char* aj = sqlite3_column_text(st, 15);
    const unsigned char* e = sqlite3_column_text(st, 16);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.trace_id = trc ? (const char*)trc : "";
    row.resource_lock = lock ? (const char*)lock : "";
    row.mode = m ? (const char*)m : "";
    row.tool_name = tool ? (const char*)tool : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 8);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 11);
    row.updated_utc_ms = sqlite3_column_int64(st, 12);
    row.result_json = r ? (const char*)r : "";
    row.result_sha256 = rh ? (const char*)rh : "";
    row.attest_json = aj ? (const char*)aj : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::list_edge_tasks_by_trace_id(
  const std::string& trace_id,
  size_t max_rows,
  std::vector<EdgeTaskRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)trace_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (trace_id.empty()) {
    if (out_error) *out_error = "list_edge_tasks_by_trace_id: trace_id is empty";
    return false;
  }
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), COALESCE(resource_lock,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, COALESCE(result_sha256,''), COALESCE(attest_json,''), error
FROM edge_tasks
WHERE trace_id=?
ORDER BY updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, trace_id);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeTaskRow row;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* lock = sqlite3_column_text(st, 5);
    const unsigned char* m = sqlite3_column_text(st, 6);
    const unsigned char* tool = sqlite3_column_text(st, 7);
    const unsigned char* p = sqlite3_column_text(st, 9);
    const unsigned char* stt = sqlite3_column_text(st, 10);
    const unsigned char* r = sqlite3_column_text(st, 13);
    const unsigned char* rh = sqlite3_column_text(st, 14);
    const unsigned char* aj = sqlite3_column_text(st, 15);
    const unsigned char* e = sqlite3_column_text(st, 16);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.trace_id = trc ? (const char*)trc : "";
    row.resource_lock = lock ? (const char*)lock : "";
    row.mode = m ? (const char*)m : "";
    row.tool_name = tool ? (const char*)tool : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 8);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 11);
    row.updated_utc_ms = sqlite3_column_int64(st, 12);
    row.result_json = r ? (const char*)r : "";
    row.result_sha256 = rh ? (const char*)rh : "";
    row.attest_json = aj ? (const char*)aj : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::get_edge_task_lock_conflict(
  const std::string& node_id,
  const std::string& resource_lock,
  std::string* out_task_id,
  std::string* out_step_id,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_task_id) out_task_id->clear();
  if (out_step_id) out_step_id->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  (void)resource_lock;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_task_id || !out_step_id) return false;
  if (node_id.empty() || resource_lock.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id
FROM edge_tasks
WHERE node_id=?
  AND resource_lock=?
  AND (state='QUEUED' OR state='RUNNING')
ORDER BY updated_utc_ms DESC
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, node_id);
  ok = ok && agent_db_bind_text(st, 2, resource_lock);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    *out_task_id = t ? (const char*)t : "";
    *out_step_id = s ? (const char*)s : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_edge_tasks_expired_deadline(
  int64_t now_utc_ms,
  size_t max_rows,
  std::vector<EdgeTaskRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_utc_ms;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  const int64_t now = now_utc_ms > 0 ? now_utc_ms : agent_db_unix_ms_now();
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), COALESCE(resource_lock,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, COALESCE(result_sha256,''), COALESCE(attest_json,''), error
FROM edge_tasks
WHERE (state='QUEUED' OR state='RUNNING')
  AND deadline_utc_ms > 0
  AND deadline_utc_ms < ?
ORDER BY deadline_utc_ms ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeTaskRow row;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* lock = sqlite3_column_text(st, 5);
    const unsigned char* m = sqlite3_column_text(st, 6);
    const unsigned char* tool = sqlite3_column_text(st, 7);
    const unsigned char* p = sqlite3_column_text(st, 9);
    const unsigned char* stt = sqlite3_column_text(st, 10);
    const unsigned char* r = sqlite3_column_text(st, 13);
    const unsigned char* rh = sqlite3_column_text(st, 14);
    const unsigned char* aj = sqlite3_column_text(st, 15);
    const unsigned char* e = sqlite3_column_text(st, 16);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.trace_id = trc ? (const char*)trc : "";
    row.resource_lock = lock ? (const char*)lock : "";
    row.mode = m ? (const char*)m : "";
    row.tool_name = tool ? (const char*)tool : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 8);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 11);
    row.updated_utc_ms = sqlite3_column_int64(st, 12);
    row.result_json = r ? (const char*)r : "";
    row.result_sha256 = rh ? (const char*)rh : "";
    row.attest_json = aj ? (const char*)aj : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::get_edge_tool_rate_state(
  const std::string& node_id,
  const std::string& tool_name,
  EdgeToolRateStateRow* out_row,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeToolRateStateRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  (void)tool_name;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (node_id.empty() || tool_name.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT node_id, tool_name, window_start_utc_ms, window_count, last_call_utc_ms
FROM edge_tool_rate_state
WHERE node_id=? AND tool_name=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, node_id);
  ok = ok && agent_db_bind_text(st, 2, tool_name);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* n = sqlite3_column_text(st, 0);
    const unsigned char* t = sqlite3_column_text(st, 1);
    out_row->node_id = n ? (const char*)n : "";
    out_row->tool_name = t ? (const char*)t : "";
    out_row->window_start_utc_ms = sqlite3_column_int64(st, 2);
    out_row->window_count = sqlite3_column_int(st, 3);
    out_row->last_call_utc_ms = sqlite3_column_int64(st, 4);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::upsert_edge_tool_rate_state(const EdgeToolRateStateRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.node_id.empty() || row.tool_name.empty()) {
    if (out_error) *out_error = "upsert_edge_tool_rate_state: node_id/tool_name empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_tool_rate_state(node_id, tool_name, window_start_utc_ms, window_count, last_call_utc_ms)
VALUES(?,?,?,?,?)
ON CONFLICT(node_id, tool_name) DO UPDATE SET
  window_start_utc_ms = excluded.window_start_utc_ms,
  window_count = excluded.window_count,
  last_call_utc_ms = excluded.last_call_utc_ms;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.node_id);
  ok = ok && agent_db_bind_text(st, 2, row.tool_name);
  ok = ok && agent_db_bind_i64(st, 3, row.window_start_utc_ms);
  ok = ok && (sqlite3_bind_int(st, 4, row.window_count) == SQLITE_OK);
  ok = ok && agent_db_bind_i64(st, 5, row.last_call_utc_ms);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::upsert_edge_rule(const EdgeRuleRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.rule_id.empty() || row.event_type.empty() || row.action_json.empty()) {
    if (out_error) *out_error = "upsert_edge_rule: missing rule_id/event_type/action_json";
    return false;
  }
  const int64_t now = agent_db_unix_ms_now();
  const int64_t created = row.created_utc_ms > 0 ? row.created_utc_ms : now;
  const int64_t updated = row.updated_utc_ms > 0 ? row.updated_utc_ms : now;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_rules(
  rule_id, enabled, event_type, min_confidence, cooldown_ms, last_fired_utc_ms, action_json, created_utc_ms, updated_utc_ms
) VALUES(?,?,?,?,?,?,?,?,?)
ON CONFLICT(rule_id) DO UPDATE SET
  enabled = excluded.enabled,
  event_type = excluded.event_type,
  min_confidence = excluded.min_confidence,
  cooldown_ms = excluded.cooldown_ms,
  last_fired_utc_ms = excluded.last_fired_utc_ms,
  action_json = excluded.action_json,
  updated_utc_ms = excluded.updated_utc_ms;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.rule_id);
  ok = ok && (sqlite3_bind_int(st, 2, row.enabled ? 1 : 0) == SQLITE_OK);
  ok = ok && agent_db_bind_text(st, 3, row.event_type);
  ok = ok && (sqlite3_bind_double(st, 4, row.min_confidence) == SQLITE_OK);
  ok = ok && (sqlite3_bind_int(st, 5, row.cooldown_ms) == SQLITE_OK);
  ok = ok && agent_db_bind_i64(st, 6, row.last_fired_utc_ms);
  ok = ok && agent_db_bind_text(st, 7, row.action_json);
  ok = ok && agent_db_bind_i64(st, 8, created);
  ok = ok && agent_db_bind_i64(st, 9, updated);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_edge_rule(const std::string& rule_id, EdgeRuleRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeRuleRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)rule_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (rule_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT rule_id, enabled, event_type, min_confidence, cooldown_ms, last_fired_utc_ms, action_json, created_utc_ms, updated_utc_ms
FROM edge_rules
WHERE rule_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, rule_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* rid = sqlite3_column_text(st, 0);
    const unsigned char* et = sqlite3_column_text(st, 2);
    const unsigned char* aj = sqlite3_column_text(st, 6);
    out_row->rule_id = rid ? (const char*)rid : "";
    out_row->enabled = sqlite3_column_int(st, 1) != 0;
    out_row->event_type = et ? (const char*)et : "";
    out_row->min_confidence = sqlite3_column_double(st, 3);
    out_row->cooldown_ms = sqlite3_column_int(st, 4);
    out_row->last_fired_utc_ms = sqlite3_column_int64(st, 5);
    out_row->action_json = aj ? (const char*)aj : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 7);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 8);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::delete_edge_rule(const std::string& rule_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)rule_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (rule_id.empty()) {
    if (out_error) *out_error = "delete_edge_rule: rule_id empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "DELETE FROM edge_rules WHERE rule_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, rule_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_edge_rules(size_t max_rows, std::vector<EdgeRuleRow>* out_rows_desc, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
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
SELECT rule_id, enabled, event_type, min_confidence, cooldown_ms, last_fired_utc_ms, action_json, created_utc_ms, updated_utc_ms
FROM edge_rules
ORDER BY updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeRuleRow row;
    const unsigned char* rid = sqlite3_column_text(st, 0);
    const unsigned char* et = sqlite3_column_text(st, 2);
    const unsigned char* aj = sqlite3_column_text(st, 6);
    row.rule_id = rid ? (const char*)rid : "";
    row.enabled = sqlite3_column_int(st, 1) != 0;
    row.event_type = et ? (const char*)et : "";
    row.min_confidence = sqlite3_column_double(st, 3);
    row.cooldown_ms = sqlite3_column_int(st, 4);
    row.last_fired_utc_ms = sqlite3_column_int64(st, 5);
    row.action_json = aj ? (const char*)aj : "";
    row.created_utc_ms = sqlite3_column_int64(st, 7);
    row.updated_utc_ms = sqlite3_column_int64(st, 8);
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}


}  // namespace agentd
