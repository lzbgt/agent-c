#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace agentd {

bool AgentDb::create_edge_workflow(
  const EdgeWorkflowRow& wf,
  const std::vector<EdgeWorkflowStepRow>& steps,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  (void)steps;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty() || wf.status.empty() || wf.spec_json.empty()) {
    if (out_error) *out_error = "create_edge_workflow: missing workflow_id/status/spec_json";
    return false;
  }
  if (steps.empty()) {
    if (out_error) *out_error = "create_edge_workflow: steps empty";
    return false;
  }
  const int64_t now = wf.updated_utc_ms > 0 ? wf.updated_utc_ms : agent_db_unix_ms_now();
  const int64_t created = wf.created_utc_ms > 0 ? wf.created_utc_ms : now;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  char* err = nullptr;
  if (sqlite3_exec((sqlite3*)db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &err) != SQLITE_OK) {
    if (out_error) *out_error = err ? err : agent_db_sqlite_err((sqlite3*)db_);
    if (err) sqlite3_free(err);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = R"SQL(
INSERT INTO edge_workflows(workflow_id, goal, status, priority, spec_json, created_utc_ms, updated_utc_ms, error)
VALUES(?,?,?,?,?,?,?,?);
)SQL";
    if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    } else {
      ok = ok && agent_db_bind_text(st, 1, wf.workflow_id);
      ok = ok && agent_db_bind_text_or_null(st, 2, wf.goal);
      ok = ok && agent_db_bind_text(st, 3, wf.status);
      ok = ok && (sqlite3_bind_int(st, 4, wf.priority) == SQLITE_OK);
      ok = ok && agent_db_bind_text(st, 5, wf.spec_json);
      ok = ok && agent_db_bind_i64(st, 6, created);
      ok = ok && agent_db_bind_i64(st, 7, now);
      ok = ok && agent_db_bind_text_or_null(st, 8, wf.error);
      ok = ok && agent_db_step_done(st);
      if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
      sqlite3_finalize(st);
    }
  }

  if (ok) {
    sqlite3_stmt* st = nullptr;
    const char* sql = R"SQL(
INSERT INTO edge_workflow_steps(
  workflow_id, step_id, kind, depends_on_json, target_json, payload_json, join_mode, deadline_utc_ms,
  attempt, max_attempts, next_ready_utc_ms, backoff_ms,
  state, created_utc_ms, updated_utc_ms, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
)SQL";
    if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    } else {
      for (const auto& s : steps) {
        if (s.workflow_id.empty() || s.step_id.empty() || s.kind.empty() || s.depends_on_json.empty() || s.target_json.empty() ||
            s.payload_json.empty() || s.state.empty()) {
          ok = false;
          if (out_error && out_error->empty()) *out_error = "create_edge_workflow: invalid step row";
          break;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        const int64_t sc = s.created_utc_ms > 0 ? s.created_utc_ms : created;
        const int64_t su = s.updated_utc_ms > 0 ? s.updated_utc_ms : now;
        ok = ok && agent_db_bind_text(st, 1, s.workflow_id);
        ok = ok && agent_db_bind_text(st, 2, s.step_id);
        ok = ok && agent_db_bind_text(st, 3, s.kind);
        ok = ok && agent_db_bind_text(st, 4, s.depends_on_json);
        ok = ok && agent_db_bind_text(st, 5, s.target_json);
        ok = ok && agent_db_bind_text(st, 6, s.payload_json);
        ok = ok && agent_db_bind_text_or_null(st, 7, s.join_mode);
        ok = ok && agent_db_bind_i64(st, 8, s.deadline_utc_ms);
        ok = ok && (sqlite3_bind_int(st, 9, s.attempt) == SQLITE_OK);
        ok = ok && (sqlite3_bind_int(st, 10, s.max_attempts) == SQLITE_OK);
        ok = ok && agent_db_bind_i64(st, 11, s.next_ready_utc_ms);
        ok = ok && (sqlite3_bind_int(st, 12, s.backoff_ms) == SQLITE_OK);
        ok = ok && agent_db_bind_text(st, 13, s.state);
        ok = ok && agent_db_bind_i64(st, 14, sc);
        ok = ok && agent_db_bind_i64(st, 15, su);
        ok = ok && agent_db_bind_text_or_null(st, 16, s.error);
        ok = ok && agent_db_step_done(st);
        if (!ok) {
          if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
          break;
        }
      }
      sqlite3_finalize(st);
    }
  }

  if (ok) {
    ok = sqlite3_exec((sqlite3*)db_, "COMMIT;", nullptr, nullptr, &err) == SQLITE_OK;
    if (!ok && out_error && out_error->empty()) *out_error = err ? err : agent_db_sqlite_err((sqlite3*)db_);
  } else {
    (void)sqlite3_exec((sqlite3*)db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  }
  if (err) sqlite3_free(err);
  return ok;
#endif
}

bool AgentDb::get_edge_workflow(const std::string& workflow_id, EdgeWorkflowRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeWorkflowRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT workflow_id, COALESCE(goal,''), status, priority, spec_json, created_utc_ms, updated_utc_ms, COALESCE(error,'')
FROM edge_workflows
WHERE workflow_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, workflow_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* wid = sqlite3_column_text(st, 0);
    const unsigned char* goal = sqlite3_column_text(st, 1);
    const unsigned char* stt = sqlite3_column_text(st, 2);
    const unsigned char* spec = sqlite3_column_text(st, 4);
    const unsigned char* errt = sqlite3_column_text(st, 7);
    out_row->workflow_id = wid ? (const char*)wid : "";
    out_row->goal = goal ? (const char*)goal : "";
    out_row->status = stt ? (const char*)stt : "";
    out_row->priority = sqlite3_column_int(st, 3);
    out_row->spec_json = spec ? (const char*)spec : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 5);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 6);
    out_row->error = errt ? (const char*)errt : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::upsert_edge_workflow(const EdgeWorkflowRow& wf, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty() || wf.status.empty() || wf.spec_json.empty()) {
    if (out_error) *out_error = "upsert_edge_workflow: missing workflow_id/status/spec_json";
    return false;
  }
  const int64_t now = wf.updated_utc_ms > 0 ? wf.updated_utc_ms : agent_db_unix_ms_now();
  const int64_t created = wf.created_utc_ms > 0 ? wf.created_utc_ms : now;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_workflows(workflow_id, goal, status, priority, spec_json, created_utc_ms, updated_utc_ms, error)
VALUES(?,?,?,?,?,?,?,?)
ON CONFLICT(workflow_id) DO UPDATE SET
  goal = excluded.goal,
  status = excluded.status,
  priority = excluded.priority,
  spec_json = excluded.spec_json,
  updated_utc_ms = excluded.updated_utc_ms,
  error = excluded.error;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, wf.workflow_id);
  ok = ok && agent_db_bind_text_or_null(st, 2, wf.goal);
  ok = ok && agent_db_bind_text(st, 3, wf.status);
  ok = ok && (sqlite3_bind_int(st, 4, wf.priority) == SQLITE_OK);
  ok = ok && agent_db_bind_text(st, 5, wf.spec_json);
  ok = ok && agent_db_bind_i64(st, 6, created);
  ok = ok && agent_db_bind_i64(st, 7, now);
  ok = ok && agent_db_bind_text_or_null(st, 8, wf.error);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_edge_workflows_by_status(
  const std::string& status,
  size_t max_rows,
  std::vector<EdgeWorkflowRow>* out_rows_desc,
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
  if (status.empty()) {
    if (out_error) *out_error = "list_edge_workflows_by_status: status empty";
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
SELECT workflow_id, COALESCE(goal,''), status, priority, spec_json, created_utc_ms, updated_utc_ms, COALESCE(error,'')
FROM edge_workflows
WHERE status=?
ORDER BY priority DESC, updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, status);
  ok = ok && agent_db_bind_i64(st, 2, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeWorkflowRow row;
    const unsigned char* wid = sqlite3_column_text(st, 0);
    const unsigned char* goal = sqlite3_column_text(st, 1);
    const unsigned char* stt = sqlite3_column_text(st, 2);
    const unsigned char* spec = sqlite3_column_text(st, 4);
    const unsigned char* errt = sqlite3_column_text(st, 7);
    row.workflow_id = wid ? (const char*)wid : "";
    row.goal = goal ? (const char*)goal : "";
    row.status = stt ? (const char*)stt : "";
    row.priority = sqlite3_column_int(st, 3);
    row.spec_json = spec ? (const char*)spec : "";
    row.created_utc_ms = sqlite3_column_int64(st, 5);
    row.updated_utc_ms = sqlite3_column_int64(st, 6);
    row.error = errt ? (const char*)errt : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
  return ok;
#endif
}

bool AgentDb::list_edge_workflow_steps(const std::string& workflow_id, std::vector<EdgeWorkflowStepRow>* out_rows, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_rows) out_rows->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_edge_workflow_steps: workflow_id empty";
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
SELECT step_id, kind, depends_on_json, target_json, payload_json, COALESCE(join_mode,''), deadline_utc_ms,
  attempt, max_attempts, next_ready_utc_ms, backoff_ms,
  state, created_utc_ms, updated_utc_ms, COALESCE(error,'')
FROM edge_workflow_steps
WHERE workflow_id=?
ORDER BY step_id ASC;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, workflow_id);
  while (ok && agent_db_step_row(st)) {
    EdgeWorkflowStepRow row;
    row.workflow_id = workflow_id;
    const unsigned char* sid = sqlite3_column_text(st, 0);
    const unsigned char* kind = sqlite3_column_text(st, 1);
    const unsigned char* deps = sqlite3_column_text(st, 2);
    const unsigned char* tgt = sqlite3_column_text(st, 3);
    const unsigned char* pay = sqlite3_column_text(st, 4);
    const unsigned char* jm = sqlite3_column_text(st, 5);
    const unsigned char* stt = sqlite3_column_text(st, 11);
    const unsigned char* errt = sqlite3_column_text(st, 14);
    row.step_id = sid ? (const char*)sid : "";
    row.kind = kind ? (const char*)kind : "";
    row.depends_on_json = deps ? (const char*)deps : "[]";
    row.target_json = tgt ? (const char*)tgt : "{}";
    row.payload_json = pay ? (const char*)pay : "{}";
    row.join_mode = jm ? (const char*)jm : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 6);
    row.attempt = sqlite3_column_int(st, 7);
    row.max_attempts = sqlite3_column_int(st, 8);
    row.next_ready_utc_ms = sqlite3_column_int64(st, 9);
    row.backoff_ms = sqlite3_column_int(st, 10);
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 12);
    row.updated_utc_ms = sqlite3_column_int64(st, 13);
    row.error = errt ? (const char*)errt : "";
    out_rows->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
  return ok;
#endif
}

bool AgentDb::upsert_edge_workflow_step(const EdgeWorkflowStepRow& step, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)step;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (step.workflow_id.empty() || step.step_id.empty() || step.kind.empty() || step.depends_on_json.empty() || step.target_json.empty() ||
      step.payload_json.empty() || step.state.empty()) {
    if (out_error) *out_error = "upsert_edge_workflow_step: missing required fields";
    return false;
  }
  const int64_t now = step.updated_utc_ms > 0 ? step.updated_utc_ms : agent_db_unix_ms_now();
  const int64_t created = step.created_utc_ms > 0 ? step.created_utc_ms : now;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_workflow_steps(
  workflow_id, step_id, kind, depends_on_json, target_json, payload_json, join_mode, deadline_utc_ms,
  attempt, max_attempts, next_ready_utc_ms, backoff_ms,
  state, created_utc_ms, updated_utc_ms, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(workflow_id, step_id) DO UPDATE SET
  state = excluded.state,
  attempt = excluded.attempt,
  max_attempts = excluded.max_attempts,
  next_ready_utc_ms = excluded.next_ready_utc_ms,
  backoff_ms = excluded.backoff_ms,
  updated_utc_ms = excluded.updated_utc_ms,
  error = excluded.error;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, step.workflow_id);
  ok = ok && agent_db_bind_text(st, 2, step.step_id);
  ok = ok && agent_db_bind_text(st, 3, step.kind);
  ok = ok && agent_db_bind_text(st, 4, step.depends_on_json);
  ok = ok && agent_db_bind_text(st, 5, step.target_json);
  ok = ok && agent_db_bind_text(st, 6, step.payload_json);
  ok = ok && agent_db_bind_text_or_null(st, 7, step.join_mode);
  ok = ok && agent_db_bind_i64(st, 8, step.deadline_utc_ms);
  ok = ok && (sqlite3_bind_int(st, 9, step.attempt) == SQLITE_OK);
  ok = ok && (sqlite3_bind_int(st, 10, step.max_attempts) == SQLITE_OK);
  ok = ok && agent_db_bind_i64(st, 11, step.next_ready_utc_ms);
  ok = ok && (sqlite3_bind_int(st, 12, step.backoff_ms) == SQLITE_OK);
  ok = ok && agent_db_bind_text(st, 13, step.state);
  ok = ok && agent_db_bind_i64(st, 14, created);
  ok = ok && agent_db_bind_i64(st, 15, now);
  ok = ok && agent_db_bind_text_or_null(st, 16, step.error);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_edge_workflow_event(const EdgeWorkflowEventRow& row, int64_t* out_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_id) *out_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.workflow_id.empty() || row.type.empty() || row.data_json.empty()) {
    if (out_error) *out_error = "insert_edge_workflow_event: missing required fields";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_workflow_events(workflow_id, ts_utc_ms, type, data_json) VALUES(?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.workflow_id);
  ok = ok && agent_db_bind_i64(st, 2, ts);
  ok = ok && agent_db_bind_text(st, 3, row.type);
  ok = ok && agent_db_bind_text(st, 4, row.data_json);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

bool AgentDb::list_edge_workflow_events(
  const std::string& workflow_id,
  int64_t after_id,
  size_t max_rows,
  std::vector<EdgeWorkflowEventRow>* out_rows_asc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_asc) out_rows_asc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)after_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_edge_workflow_events: workflow_id empty";
    return false;
  }
  if (!out_rows_asc) return false;
  if (after_id < 0) after_id = 0;
  if (max_rows == 0) max_rows = 128;
  if (max_rows > 1024) max_rows = 1024;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT id, ts_utc_ms, type, data_json
FROM edge_workflow_events
WHERE workflow_id=? AND id>?
ORDER BY id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, workflow_id);
  ok = ok && agent_db_bind_i64(st, 2, after_id);
  ok = ok && agent_db_bind_i64(st, 3, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeWorkflowEventRow row;
    row.workflow_id = workflow_id;
    row.id = sqlite3_column_int64(st, 0);
    row.ts_utc_ms = sqlite3_column_int64(st, 1);
    const unsigned char* t = sqlite3_column_text(st, 2);
    const unsigned char* d = sqlite3_column_text(st, 3);
    row.type = t ? (const char*)t : "";
    row.data_json = d ? (const char*)d : "{}";
    out_rows_asc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
  return ok;
#endif
}

bool AgentDb::insert_edge_task_event(const EdgeTaskEventRow& row, int64_t* out_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_id) *out_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.task_id.empty() || row.step_id.empty() || row.state.empty() || row.data_json.empty()) {
    if (out_error) *out_error = "insert_edge_task_event: missing required fields";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_task_events(task_id, step_id, ts_utc_ms, state, data_json) VALUES(?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.task_id);
  ok = ok && agent_db_bind_text(st, 2, row.step_id);
  ok = ok && agent_db_bind_i64(st, 3, ts);
  ok = ok && agent_db_bind_text(st, 4, row.state);
  ok = ok && agent_db_bind_text(st, 5, row.data_json);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

bool AgentDb::insert_edge_sensor_event(const EdgeSensorEventRow& row, int64_t* out_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_id) *out_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.node_id.empty() || row.event_type.empty() || row.data_json.empty()) {
    if (out_error) *out_error = "insert_edge_sensor_event: missing required fields";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_sensor_events(node_id, event_type, ts_utc_ms, confidence, data_json) VALUES(?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.node_id);
  ok = ok && agent_db_bind_text(st, 2, row.event_type);
  ok = ok && agent_db_bind_i64(st, 3, ts);
  if (sqlite3_bind_double(st, 4, row.confidence) != SQLITE_OK) ok = false;
  ok = ok && agent_db_bind_text(st, 5, row.data_json);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

bool AgentDb::find_edge_sensor_event_latest(
  const std::string& event_type,
  const std::string& node_id_or_empty,
  int64_t since_utc_ms,
  double min_confidence,
  EdgeSensorEventRow* out_row,
  bool* out_found,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_found) *out_found = false;
  if (out_row) *out_row = EdgeSensorEventRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)event_type;
  (void)node_id_or_empty;
  (void)since_utc_ms;
  (void)min_confidence;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (event_type.empty() || !out_row || !out_found) {
    if (out_error) *out_error = "find_edge_sensor_event_latest: missing event_type/output";
    return false;
  }
  const int64_t since = since_utc_ms > 0 ? since_utc_ms : 0;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT id, node_id, event_type, ts_utc_ms, confidence, data_json
FROM edge_sensor_events
WHERE event_type=?
  AND (? = '' OR node_id=?)
  AND ts_utc_ms >= ?
  AND confidence >= ?
ORDER BY ts_utc_ms DESC, id DESC
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, event_type);
  ok = ok && agent_db_bind_text(st, 2, node_id_or_empty);
  ok = ok && agent_db_bind_text(st, 3, node_id_or_empty);
  ok = ok && agent_db_bind_i64(st, 4, since);
  if (sqlite3_bind_double(st, 5, min_confidence) != SQLITE_OK) ok = false;

  if (ok && agent_db_step_row(st)) {
    out_row->id = (int64_t)sqlite3_column_int64(st, 0);
    const unsigned char* node = sqlite3_column_text(st, 1);
    const unsigned char* etype = sqlite3_column_text(st, 2);
    const unsigned char* data = sqlite3_column_text(st, 5);
    out_row->node_id = node ? (const char*)node : "";
    out_row->event_type = etype ? (const char*)etype : "";
    out_row->ts_utc_ms = (int64_t)sqlite3_column_int64(st, 3);
    out_row->confidence = sqlite3_column_double(st, 4);
    out_row->data_json = data ? (const char*)data : "";
    *out_found = true;
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}

}  // namespace agentd
