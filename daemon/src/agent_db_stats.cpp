#include "agent_db.h"

#include <sqlite3.h>

#include <sstream>

namespace agentd {
namespace {

static bool count_table_locked(sqlite3* db, const char* table, int64_t* out_count, std::string* out_error) {
  if (out_count) *out_count = 0;
  if (!db || !table || !table[0] || !out_count) {
    if (out_error) *out_error = "count_table_locked: invalid args";
    return false;
  }
  std::string sql = std::string("SELECT COUNT(1) FROM ") + table + ";";
  sqlite3_stmt* st = nullptr;
  const int rc = sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &st, nullptr);
  if (rc != SQLITE_OK) {
    const char* msg = sqlite3_errmsg(db);
    if (msg && std::string(msg).find("no such table") != std::string::npos) {
      *out_count = 0;
      return true;
    }
    if (out_error) *out_error = msg ? msg : "count_table_locked: prepare failed";
    return false;
  }
  const int step_rc = sqlite3_step(st);
  if (step_rc == SQLITE_ROW) {
    *out_count = sqlite3_column_int64(st, 0);
  } else if (step_rc != SQLITE_DONE) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return false;
  }
  sqlite3_finalize(st);
  return true;
}

static bool read_group_counts_locked(
  sqlite3* db,
  const char* sql,
  std::map<std::string, int64_t>* out_counts,
  std::string* out_error
) {
  if (out_counts) out_counts->clear();
  if (!db || !sql || !out_counts) {
    if (out_error) *out_error = "read_group_counts_locked: invalid args";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const int rc = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
  if (rc != SQLITE_OK) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    return false;
  }
  while (true) {
    const int step_rc = sqlite3_step(st);
    if (step_rc == SQLITE_ROW) {
      const char* k = (const char*)sqlite3_column_text(st, 0);
      const int64_t v = sqlite3_column_int64(st, 1);
      if (k && k[0]) (*out_counts)[k] = v;
    } else if (step_rc == SQLITE_DONE) {
      break;
    } else {
      if (out_error) *out_error = sqlite3_errmsg(db);
      sqlite3_finalize(st);
      return false;
    }
  }
  sqlite3_finalize(st);
  return true;
}

}  // namespace

bool AgentDb::get_job_status_counts(JobStatusCounts* out_counts, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_counts) *out_counts = JobStatusCounts{};
  if (!out_counts) {
    if (out_error) *out_error = "get_job_status_counts: missing output";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "get_job_status_counts: db not open";
    return false;
  }
  if (!read_group_counts_locked(db_, "SELECT status, COUNT(1) FROM jobs GROUP BY status;", &out_counts->by_status, out_error)) {
    return false;
  }
  int64_t total = 0;
  for (const auto& kv : out_counts->by_status) total += kv.second;
  out_counts->total = total;
  return true;
}

bool AgentDb::get_table_counts(TableCounts* out_counts, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_counts) *out_counts = TableCounts{};
  if (!out_counts) {
    if (out_error) *out_error = "get_table_counts: missing output";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "get_table_counts: db not open";
    return false;
  }
  bool ok = true;
  ok = ok && count_table_locked(db_, "sessions", &out_counts->sessions, out_error);
  ok = ok && count_table_locked(db_, "messages", &out_counts->messages, out_error);
  ok = ok && count_table_locked(db_, "runs", &out_counts->runs, out_error);
  ok = ok && count_table_locked(db_, "events", &out_counts->events, out_error);
  ok = ok && count_table_locked(db_, "artifacts", &out_counts->artifacts, out_error);
  ok = ok && count_table_locked(db_, "ui_actions", &out_counts->ui_actions, out_error);
  ok = ok && count_table_locked(db_, "client_events", &out_counts->client_events, out_error);
  ok = ok && count_table_locked(db_, "audit_records", &out_counts->audit_records, out_error);
  ok = ok && count_table_locked(db_, "jobs", &out_counts->jobs, out_error);
  ok = ok && count_table_locked(db_, "workflows", &out_counts->workflows, out_error);
  ok = ok && count_table_locked(db_, "workflow_tasks", &out_counts->workflow_tasks, out_error);
  ok = ok && count_table_locked(db_, "workflow_events", &out_counts->workflow_events, out_error);
  ok = ok && count_table_locked(db_, "workflow_schedules", &out_counts->workflow_schedules, out_error);
  ok = ok && count_table_locked(db_, "workflow_schedule_runs", &out_counts->workflow_schedule_runs, out_error);
  ok = ok && count_table_locked(db_, "edge_nodes", &out_counts->edge_nodes, out_error);
  ok = ok && count_table_locked(db_, "edge_tasks", &out_counts->edge_tasks, out_error);
  ok = ok && count_table_locked(db_, "edge_workflows", &out_counts->edge_workflows, out_error);
  ok = ok && count_table_locked(db_, "blob_manifest", &out_counts->blob_manifest, out_error);
  ok = ok && count_table_locked(db_, "artifact_blobs", &out_counts->artifact_blobs, out_error);
  return ok;
}

}  // namespace agentd
