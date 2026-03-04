#include "agent_db.h"

#include "agent_db_sqlite.h"

#include <sqlite3.h>

#include <mutex>
#include <string>
#include <vector>

namespace agentd {

bool AgentDb::insert_workflow_schedule(const WorkflowScheduleRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.schedule_id.empty()) {
    if (out_error) *out_error = "insert_workflow_schedule: schedule_id is empty";
    return false;
  }
  if (row.cron.empty() || row.timezone.empty() || row.spec_json.empty()) {
    if (out_error) *out_error = "insert_workflow_schedule: missing cron/timezone/spec_json";
    return false;
  }

  const int64_t now = row.updated_unix_ms > 0 ? row.updated_unix_ms : agent_db_unix_ms_now();
  const int64_t created = row.created_unix_ms > 0 ? row.created_unix_ms : now;
  const std::string status = row.status.empty() ? "active" : row.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO workflow_schedules(
  schedule_id, created_unix_ms, updated_unix_ms, status, cron, timezone, spec_json, metadata_json,
  last_tick_unix_ms, next_tick_unix_ms, last_error
) VALUES(?,?,?,?,?,?,?,?,?,?,?);
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.schedule_id);
  ok = ok && agent_db_bind_i64(st, 2, created);
  ok = ok && agent_db_bind_i64(st, 3, now);
  ok = ok && agent_db_bind_text(st, 4, status);
  ok = ok && agent_db_bind_text(st, 5, row.cron);
  ok = ok && agent_db_bind_text(st, 6, row.timezone);
  ok = ok && agent_db_bind_text(st, 7, row.spec_json);
  ok = ok && agent_db_bind_text(st, 8, row.metadata_json);
  ok = ok && agent_db_bind_i64(st, 9, row.last_tick_unix_ms);
  ok = ok && agent_db_bind_i64(st, 10, row.next_tick_unix_ms);
  ok = ok && agent_db_bind_text(st, 11, row.last_error);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_workflow_schedule(const std::string& schedule_id, WorkflowScheduleRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)schedule_id;
  (void)out_row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_row || schedule_id.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT schedule_id, created_unix_ms, updated_unix_ms, status, cron, timezone, spec_json, COALESCE(metadata_json,''),
       COALESCE(last_tick_unix_ms,0), COALESCE(next_tick_unix_ms,0), COALESCE(last_error,'')
FROM workflow_schedules
WHERE schedule_id=?
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, schedule_id);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  const int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    out_row->schedule_id = (const char*)sqlite3_column_text(st, 0);
    out_row->created_unix_ms = sqlite3_column_int64(st, 1);
    out_row->updated_unix_ms = sqlite3_column_int64(st, 2);
    out_row->status = (const char*)sqlite3_column_text(st, 3);
    out_row->cron = (const char*)sqlite3_column_text(st, 4);
    out_row->timezone = (const char*)sqlite3_column_text(st, 5);
    out_row->spec_json = (const char*)sqlite3_column_text(st, 6);
    out_row->metadata_json = (const char*)sqlite3_column_text(st, 7);
    out_row->last_tick_unix_ms = sqlite3_column_int64(st, 8);
    out_row->next_tick_unix_ms = sqlite3_column_int64(st, 9);
    out_row->last_error = (const char*)sqlite3_column_text(st, 10);
    sqlite3_finalize(st);
    return true;
  }
  sqlite3_finalize(st);
  return false;
#endif
}

bool AgentDb::list_workflow_schedules(
  const std::string& status,
  size_t limit,
  size_t offset,
  std::vector<WorkflowScheduleRow>* out_rows,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)limit;
  (void)offset;
  (void)out_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows) return false;
  out_rows->clear();
  const size_t lim = std::max<size_t>(1, std::min<size_t>(1000, limit));
  const size_t off = std::min<size_t>(1000000, offset);

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  std::string sql =
    "SELECT schedule_id, created_unix_ms, updated_unix_ms, status, cron, timezone, spec_json, COALESCE(metadata_json,''), "
    "COALESCE(last_tick_unix_ms,0), COALESCE(next_tick_unix_ms,0), COALESCE(last_error,'') "
    "FROM workflow_schedules ";
  if (!status.empty()) sql += "WHERE status=? ";
  sql += "ORDER BY updated_unix_ms DESC LIMIT ? OFFSET ?;";

  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  int idx = 1;
  bool ok = true;
  if (!status.empty()) ok = ok && agent_db_bind_text(st, idx++, status);
  ok = ok && agent_db_bind_i64(st, idx++, (int64_t)lim);
  ok = ok && agent_db_bind_i64(st, idx++, (int64_t)off);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  for (;;) {
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    WorkflowScheduleRow row;
    row.schedule_id = (const char*)sqlite3_column_text(st, 0);
    row.created_unix_ms = sqlite3_column_int64(st, 1);
    row.updated_unix_ms = sqlite3_column_int64(st, 2);
    row.status = (const char*)sqlite3_column_text(st, 3);
    row.cron = (const char*)sqlite3_column_text(st, 4);
    row.timezone = (const char*)sqlite3_column_text(st, 5);
    row.spec_json = (const char*)sqlite3_column_text(st, 6);
    row.metadata_json = (const char*)sqlite3_column_text(st, 7);
    row.last_tick_unix_ms = sqlite3_column_int64(st, 8);
    row.next_tick_unix_ms = sqlite3_column_int64(st, 9);
    row.last_error = (const char*)sqlite3_column_text(st, 10);
    out_rows->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::list_workflow_schedules_due(
  int64_t now_unix_ms,
  size_t limit,
  std::vector<WorkflowScheduleRow>* out_rows,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_unix_ms;
  (void)limit;
  (void)out_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows) return false;
  out_rows->clear();
  const size_t lim = std::max<size_t>(1, std::min<size_t>(1024, limit));
  const int64_t now = now_unix_ms <= 0 ? agent_db_unix_ms_now() : now_unix_ms;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT schedule_id, created_unix_ms, updated_unix_ms, status, cron, timezone, spec_json, COALESCE(metadata_json,''), "
    "COALESCE(last_tick_unix_ms,0), COALESCE(next_tick_unix_ms,0), COALESCE(last_error,'') "
    "FROM workflow_schedules "
    "WHERE status='active' AND next_tick_unix_ms>0 AND next_tick_unix_ms<=? "
    "ORDER BY next_tick_unix_ms ASC LIMIT ?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, now);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)lim);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  for (;;) {
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    WorkflowScheduleRow row;
    row.schedule_id = (const char*)sqlite3_column_text(st, 0);
    row.created_unix_ms = sqlite3_column_int64(st, 1);
    row.updated_unix_ms = sqlite3_column_int64(st, 2);
    row.status = (const char*)sqlite3_column_text(st, 3);
    row.cron = (const char*)sqlite3_column_text(st, 4);
    row.timezone = (const char*)sqlite3_column_text(st, 5);
    row.spec_json = (const char*)sqlite3_column_text(st, 6);
    row.metadata_json = (const char*)sqlite3_column_text(st, 7);
    row.last_tick_unix_ms = sqlite3_column_int64(st, 8);
    row.next_tick_unix_ms = sqlite3_column_int64(st, 9);
    row.last_error = (const char*)sqlite3_column_text(st, 10);
    out_rows->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::update_workflow_schedule_status(
  const std::string& schedule_id,
  const std::string& status,
  int64_t now_unix_ms,
  bool* out_found,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_found) *out_found = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)schedule_id;
  (void)status;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (schedule_id.empty() || status.empty()) {
    if (out_error) *out_error = "update_workflow_schedule_status: missing schedule_id/status";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "UPDATE workflow_schedules SET status=?, updated_unix_ms=? WHERE schedule_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, status);
  ok = ok && agent_db_bind_i64(st, 2, now);
  ok = ok && agent_db_bind_text(st, 3, schedule_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  if (ok && out_found) *out_found = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::update_workflow_schedule_ticks(
  const std::string& schedule_id,
  int64_t last_tick_unix_ms,
  int64_t next_tick_unix_ms,
  const std::string& last_error,
  int64_t now_unix_ms,
  bool* out_found,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_found) *out_found = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)schedule_id;
  (void)last_tick_unix_ms;
  (void)next_tick_unix_ms;
  (void)last_error;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (schedule_id.empty()) {
    if (out_error) *out_error = "update_workflow_schedule_ticks: schedule_id empty";
    return false;
  }
  const int64_t now = now_unix_ms > 0 ? now_unix_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE workflow_schedules
SET last_tick_unix_ms=?, next_tick_unix_ms=?, last_error=?, updated_unix_ms=?
WHERE schedule_id=?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, last_tick_unix_ms);
  ok = ok && agent_db_bind_i64(st, 2, next_tick_unix_ms);
  ok = ok && agent_db_bind_text(st, 3, last_error);
  ok = ok && agent_db_bind_i64(st, 4, now);
  ok = ok && agent_db_bind_text(st, 5, schedule_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  if (ok && out_found) *out_found = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::delete_workflow_schedule(const std::string& schedule_id, bool* out_found, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_found) *out_found = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)schedule_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (schedule_id.empty()) {
    if (out_error) *out_error = "delete_workflow_schedule: schedule_id empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "DELETE FROM workflow_schedules WHERE schedule_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, schedule_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  if (ok && out_found) *out_found = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_workflow_schedule_run(
  const WorkflowScheduleRunRow& row,
  bool* out_inserted,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_inserted) *out_inserted = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.schedule_id.empty() || row.tick_unix_ms <= 0 || row.workflow_id.empty()) {
    if (out_error) *out_error = "insert_workflow_schedule_run: missing schedule_id/tick/workflow_id";
    return false;
  }
  const int64_t created = row.created_unix_ms > 0 ? row.created_unix_ms : agent_db_unix_ms_now();
  const std::string status = row.status.empty() ? "enqueued" : row.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT OR IGNORE INTO workflow_schedule_runs(
  schedule_id, tick_unix_ms, workflow_id, created_unix_ms, status, error
) VALUES(?,?,?,?,?,?);
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.schedule_id);
  ok = ok && agent_db_bind_i64(st, 2, row.tick_unix_ms);
  ok = ok && agent_db_bind_text(st, 3, row.workflow_id);
  ok = ok && agent_db_bind_i64(st, 4, created);
  ok = ok && agent_db_bind_text(st, 5, status);
  ok = ok && agent_db_bind_text(st, 6, row.error);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  if (ok && out_inserted) *out_inserted = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::update_workflow_schedule_run_status(
  const std::string& schedule_id,
  int64_t tick_unix_ms,
  const std::string& status,
  const std::string& error,
  bool* out_found,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_found) *out_found = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)schedule_id;
  (void)tick_unix_ms;
  (void)status;
  (void)error;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (schedule_id.empty() || tick_unix_ms <= 0 || status.empty()) {
    if (out_error) *out_error = "update_workflow_schedule_run_status: missing fields";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "UPDATE workflow_schedule_runs SET status=?, error=? WHERE schedule_id=? AND tick_unix_ms=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, status);
  ok = ok && agent_db_bind_text(st, 2, error);
  ok = ok && agent_db_bind_text(st, 3, schedule_id);
  ok = ok && agent_db_bind_i64(st, 4, tick_unix_ms);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  if (ok && out_found) *out_found = sqlite3_changes(db_) > 0;
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_workflow_schedule_runs(
  const std::string& schedule_id,
  size_t limit,
  size_t offset,
  std::vector<WorkflowScheduleRunRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)schedule_id;
  (void)limit;
  (void)offset;
  (void)out_rows_desc;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  out_rows_desc->clear();
  if (schedule_id.empty()) {
    if (out_error) *out_error = "list_workflow_schedule_runs: schedule_id empty";
    return false;
  }
  const size_t lim = std::max<size_t>(1, std::min<size_t>(1000, limit));
  const size_t off = std::min<size_t>(1000000, offset);

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT schedule_id, tick_unix_ms, workflow_id, created_unix_ms, status, COALESCE(error,'')
FROM workflow_schedule_runs
WHERE schedule_id=?
ORDER BY tick_unix_ms DESC
LIMIT ? OFFSET ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, schedule_id);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)lim);
  ok = ok && agent_db_bind_i64(st, 3, (int64_t)off);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  for (;;) {
    const int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      sqlite3_finalize(st);
      return false;
    }
    WorkflowScheduleRunRow row;
    row.schedule_id = (const char*)sqlite3_column_text(st, 0);
    row.tick_unix_ms = sqlite3_column_int64(st, 1);
    row.workflow_id = (const char*)sqlite3_column_text(st, 2);
    row.created_unix_ms = sqlite3_column_int64(st, 3);
    row.status = (const char*)sqlite3_column_text(st, 4);
    row.error = (const char*)sqlite3_column_text(st, 5);
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
#endif
}

}  // namespace agentd
