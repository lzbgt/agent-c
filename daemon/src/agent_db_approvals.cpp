#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace agentd {
namespace {

static std::string stmt_text(sqlite3_stmt* st, int col) {
  const unsigned char* txt = sqlite3_column_text(st, col);
  return txt ? std::string(reinterpret_cast<const char*>(txt)) : std::string();
}

static int64_t stmt_i64(sqlite3_stmt* st, int col) {
  return (int64_t)sqlite3_column_int64(st, col);
}

static int stmt_i32(sqlite3_stmt* st, int col) {
  return sqlite3_column_int(st, col);
}

}  // namespace

bool AgentDb::insert_approval_request(const ApprovalRequestRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.approval_id.empty()) {
    if (out_error) *out_error = "insert_approval_request: approval_id is empty";
    return false;
  }
  if (row.tool_name.empty()) {
    if (out_error) *out_error = "insert_approval_request: tool_name is empty";
    return false;
  }
  const int64_t created = row.created_unix_ms > 0 ? row.created_unix_ms : agent_db_unix_ms_now();
  const int64_t expires = row.expires_unix_ms > 0 ? row.expires_unix_ms : 0;
  const std::string status = row.status.empty() ? "pending" : row.status;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO approval_requests(
  approval_id, run_id, trace_id, session_id, job_id, team_id,
  tool_name, tool_call_id, tool_args_hash, required_approvals, role_constraints_json,
  require_distinct_roles, status, created_unix_ms, expires_unix_ms, decision_reason
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.approval_id);
  ok = ok && agent_db_bind_i64_or_null(st, 2, row.run_id);
  ok = ok && agent_db_bind_text_or_null(st, 3, row.trace_id);
  ok = ok && agent_db_bind_text_or_null(st, 4, row.session_id);
  ok = ok && agent_db_bind_text_or_null(st, 5, row.job_id);
  ok = ok && agent_db_bind_text_or_null(st, 6, row.team_id);
  ok = ok && agent_db_bind_text(st, 7, row.tool_name);
  ok = ok && agent_db_bind_text_or_null(st, 8, row.tool_call_id);
  ok = ok && agent_db_bind_text_or_null(st, 9, row.tool_args_hash);
  ok = ok && agent_db_bind_i32(st, 10, std::max(0, row.required_approvals));
  ok = ok && agent_db_bind_text_or_null(st, 11, row.role_constraints_json);
  ok = ok && agent_db_bind_i32(st, 12, row.require_distinct_roles ? 1 : 0);
  ok = ok && agent_db_bind_text(st, 13, status);
  ok = ok && agent_db_bind_i64(st, 14, created);
  ok = ok && agent_db_bind_i64_or_null(st, 15, expires);
  ok = ok && agent_db_bind_text_or_null(st, 16, row.decision_reason);

  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) {
    *out_error = agent_db_sqlite_err(db_);
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_approval_request(const std::string& approval_id, ApprovalRequestRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = ApprovalRequestRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)approval_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (approval_id.empty()) {
    if (out_error) *out_error = "get_approval_request: approval_id is empty";
    return false;
  }
  if (!out_row) {
    if (out_error) *out_error = "get_approval_request: out_row is null";
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
  approval_id, COALESCE(run_id,0), COALESCE(trace_id,''), COALESCE(session_id,''), COALESCE(job_id,''), COALESCE(team_id,''),
  COALESCE(tool_name,''), COALESCE(tool_call_id,''), COALESCE(tool_args_hash,''), COALESCE(required_approvals,0),
  COALESCE(role_constraints_json,''), COALESCE(require_distinct_roles,0), COALESCE(status,''), COALESCE(created_unix_ms,0),
  COALESCE(expires_unix_ms,0), COALESCE(decision_reason,'')
FROM approval_requests
WHERE approval_id=?
LIMIT 1;
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, approval_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    out_row->approval_id = stmt_text(st, 0);
    out_row->run_id = stmt_i64(st, 1);
    out_row->trace_id = stmt_text(st, 2);
    out_row->session_id = stmt_text(st, 3);
    out_row->job_id = stmt_text(st, 4);
    out_row->team_id = stmt_text(st, 5);
    out_row->tool_name = stmt_text(st, 6);
    out_row->tool_call_id = stmt_text(st, 7);
    out_row->tool_args_hash = stmt_text(st, 8);
    out_row->required_approvals = stmt_i32(st, 9);
    out_row->role_constraints_json = stmt_text(st, 10);
    out_row->require_distinct_roles = stmt_i32(st, 11) != 0;
    out_row->status = stmt_text(st, 12);
    out_row->created_unix_ms = stmt_i64(st, 13);
    out_row->expires_unix_ms = stmt_i64(st, 14);
    out_row->decision_reason = stmt_text(st, 15);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_approval_requests(
  const ApprovalListFilter& filter,
  std::vector<ApprovalRequestRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)filter;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) {
    if (out_error) *out_error = "list_approval_requests: out_rows_desc is null";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  std::string sql = R"SQL(
SELECT
  approval_id, COALESCE(run_id,0), COALESCE(trace_id,''), COALESCE(session_id,''), COALESCE(job_id,''), COALESCE(team_id,''),
  COALESCE(tool_name,''), COALESCE(tool_call_id,''), COALESCE(tool_args_hash,''), COALESCE(required_approvals,0),
  COALESCE(role_constraints_json,''), COALESCE(require_distinct_roles,0), COALESCE(status,''), COALESCE(created_unix_ms,0),
  COALESCE(expires_unix_ms,0), COALESCE(decision_reason,'')
FROM approval_requests
WHERE 1=1
  )SQL";

  std::vector<std::function<bool(sqlite3_stmt*, int)>> binders;
  if (!filter.status.empty()) {
    sql += " AND status=?";
    binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_text(st, idx, filter.status); });
  }
  if (!filter.team_id.empty()) {
    sql += " AND team_id=?";
    binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_text(st, idx, filter.team_id); });
  }
  if (!filter.trace_id.empty()) {
    sql += " AND trace_id=?";
    binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_text(st, idx, filter.trace_id); });
  }
  if (!filter.job_id.empty()) {
    sql += " AND job_id=?";
    binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_text(st, idx, filter.job_id); });
  }
  if (!filter.tool_name.empty()) {
    sql += " AND tool_name=?";
    binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_text(st, idx, filter.tool_name); });
  }
  if (filter.run_id > 0) {
    sql += " AND run_id=?";
    binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_i64(st, idx, filter.run_id); });
  }

  sql += " ORDER BY created_unix_ms DESC";
  sql += " LIMIT ?";
  const size_t lim = filter.limit > 0 ? filter.limit : 100;
  binders.emplace_back([&](sqlite3_stmt* st, int idx) { return agent_db_bind_i64(st, idx, (int64_t)lim); });

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  int bind_idx = 1;
  for (const auto& bind : binders) {
    if (!bind(st, bind_idx++)) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  while (agent_db_step_row(st)) {
    ApprovalRequestRow row;
    row.approval_id = stmt_text(st, 0);
    row.run_id = stmt_i64(st, 1);
    row.trace_id = stmt_text(st, 2);
    row.session_id = stmt_text(st, 3);
    row.job_id = stmt_text(st, 4);
    row.team_id = stmt_text(st, 5);
    row.tool_name = stmt_text(st, 6);
    row.tool_call_id = stmt_text(st, 7);
    row.tool_args_hash = stmt_text(st, 8);
    row.required_approvals = stmt_i32(st, 9);
    row.role_constraints_json = stmt_text(st, 10);
    row.require_distinct_roles = stmt_i32(st, 11) != 0;
    row.status = stmt_text(st, 12);
    row.created_unix_ms = stmt_i64(st, 13);
    row.expires_unix_ms = stmt_i64(st, 14);
    row.decision_reason = stmt_text(st, 15);
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::update_approval_status(
  const std::string& approval_id,
  const std::string& status,
  const std::string& decision_reason,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)approval_id;
  (void)status;
  (void)decision_reason;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (approval_id.empty()) {
    if (out_error) *out_error = "update_approval_status: approval_id is empty";
    return false;
  }
  if (status.empty()) {
    if (out_error) *out_error = "update_approval_status: status is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE approval_requests
SET status=?, decision_reason=?
WHERE approval_id=?;
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, status);
  ok = ok && agent_db_bind_text_or_null(st, 2, decision_reason);
  ok = ok && agent_db_bind_text(st, 3, approval_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) {
    *out_error = agent_db_sqlite_err(db_);
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_approval_decision(const ApprovalDecisionRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.approval_id.empty()) {
    if (out_error) *out_error = "insert_approval_decision: approval_id is empty";
    return false;
  }
  if (row.member_id.empty()) {
    if (out_error) *out_error = "insert_approval_decision: member_id is empty";
    return false;
  }
  if (row.decision.empty()) {
    if (out_error) *out_error = "insert_approval_decision: decision is empty";
    return false;
  }

  const int64_t ts = row.decision_unix_ms > 0 ? row.decision_unix_ms : agent_db_unix_ms_now();

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO approval_decisions(
  approval_id, member_id, member_role, decision, decision_unix_ms, note
) VALUES(?,?,?,?,?,?);
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.approval_id);
  ok = ok && agent_db_bind_text(st, 2, row.member_id);
  ok = ok && agent_db_bind_text_or_null(st, 3, row.member_role);
  ok = ok && agent_db_bind_text(st, 4, row.decision);
  ok = ok && agent_db_bind_i64(st, 5, ts);
  ok = ok && agent_db_bind_text_or_null(st, 6, row.note);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) {
    *out_error = agent_db_sqlite_err(db_);
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_approval_decisions(
  const std::string& approval_id,
  std::vector<ApprovalDecisionRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)approval_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (approval_id.empty()) {
    if (out_error) *out_error = "list_approval_decisions: approval_id is empty";
    return false;
  }
  if (!out_rows_desc) {
    if (out_error) *out_error = "list_approval_decisions: out_rows_desc is null";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT id, approval_id, member_id, COALESCE(member_role,''), decision, decision_unix_ms, COALESCE(note,'')
FROM approval_decisions
WHERE approval_id=?
ORDER BY id ASC;
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, approval_id);
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  while (agent_db_step_row(st)) {
    ApprovalDecisionRow row;
    row.id = stmt_i64(st, 0);
    row.approval_id = stmt_text(st, 1);
    row.member_id = stmt_text(st, 2);
    row.member_role = stmt_text(st, 3);
    row.decision = stmt_text(st, 4);
    row.decision_unix_ms = stmt_i64(st, 5);
    row.note = stmt_text(st, 6);
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::backfill_approval_run_id(const std::string& trace_id, int64_t run_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)trace_id;
  (void)run_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (trace_id.empty() || run_id <= 0) {
    return true;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
UPDATE approval_requests
SET run_id=?
WHERE trace_id=? AND (run_id IS NULL OR run_id <= 0);
  )SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, run_id);
  ok = ok && agent_db_bind_text(st, 2, trace_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) {
    *out_error = agent_db_sqlite_err(db_);
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

}  // namespace agentd
