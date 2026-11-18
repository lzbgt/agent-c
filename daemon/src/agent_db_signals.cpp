#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <limits>
#include <memory>

namespace agentd {
#if defined(AGENT_HAVE_SQLITE3)
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement query(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* s = nullptr;
  sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr);
  return Statement(s, sqlite3_finalize);
}
std::string text(sqlite3_stmt* s, int column) {
  const auto* value = sqlite3_column_text(s, column);
  return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(s, column)) : std::string();
}
const char* columns = "signal_id,workflow_id,task_id,request_hash,spec_json,state,created_unix_ms,expires_unix_ms,resolved_unix_ms,payload_json,payload_hash,delivery_id";
void read_row(sqlite3_stmt* s, AgentDb::WorkflowSignalRow* out) {
  out->signal_id = text(s, 0); out->workflow_id = text(s, 1); out->task_id = text(s, 2);
  out->request_hash = text(s, 3); out->spec_json = text(s, 4); out->state = text(s, 5);
  out->created_unix_ms = sqlite3_column_int64(s, 6); out->expires_unix_ms = sqlite3_column_int64(s, 7);
  out->resolved_unix_ms = sqlite3_column_int64(s, 8);
  out->payload_json = text(s, 9); out->payload_hash = text(s, 10); out->delivery_id = text(s, 11);
}
int read(sqlite3* db, const std::string& workflow, const std::string& task, AgentDb::WorkflowSignalRow* out) {
  auto s = query(db, std::string("SELECT ") + columns + " FROM workflow_signals WHERE workflow_id=? AND task_id=?");
  if (!s || !agent_db_bind_text(s.get(), 1, workflow) || !agent_db_bind_text(s.get(), 2, task)) return SQLITE_ERROR;
  const int rc = sqlite3_step(s.get());
  if (rc == SQLITE_ROW) read_row(s.get(), out);
  return rc;
}
bool bind_task(sqlite3_stmt* s, int start, const AgentDb::WorkflowTaskRow& t) {
  return agent_db_bind_text(s, start, t.workflow_id) && agent_db_bind_text(s, start + 1, t.task_id) &&
    agent_db_bind_i32(s, start + 2, t.attempt) && agent_db_bind_text(s, start + 3, t.request_json);
}
bool stopped(sqlite3_stmt* s, int64_t now) {
  const auto status = text(s, 2);
  return sqlite3_column_int(s, 0) || (sqlite3_column_int64(s, 1) > 0 && sqlite3_column_int64(s, 1) <= now) ||
    (status != "queued" && status != "running");
}
}
#endif

bool AgentDb::prepare_workflow_signal(const WorkflowTaskRow& expected, const WorkflowSignalRow& def,
    int64_t timeout_ms, WorkflowSignalRow* out, bool* yielded, std::string* error) {
  if (error) error->clear();
  if (yielded) *yielded = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)expected; (void)def; (void)timeout_ms; (void)out;
  if (error) *error = "SQLite support required"; return false;
#else
  if (!out || !yielded || expected.workflow_id != def.workflow_id || expected.task_id != def.task_id ||
      expected.attempt < 1 || def.signal_id.empty() || def.request_hash.empty() || def.spec_json.empty() ||
      def.spec_json.size() > 16384 || timeout_ms < 0 || timeout_ms > 2592000000LL) {
    if (error) *error = "invalid signal definition"; return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN IMMEDIATE", error)) return false;
  const auto fail = [&](const std::string& message) { exec_locked("ROLLBACK", nullptr); if (error) *error = message; return false; };
  const auto now = agent_db_unix_ms_now();
  {
    auto s = query(db_, "SELECT w.cancel_requested,COALESCE(w.deadline_unix_ms,0),w.status FROM workflows w JOIN workflow_tasks t ON t.workflow_id=w.workflow_id WHERE t.workflow_id=? AND t.task_id=? AND t.attempt=? AND t.request_json=? AND t.status='running'");
    if (!s || !bind_task(s.get(), 1, expected) || sqlite3_step(s.get()) != SQLITE_ROW) return fail("stale signal attempt");
    if (stopped(s.get(), now)) {
      exec_locked("ROLLBACK", nullptr); *out = def; out->state = "stopped"; return true;
    }
  }
  {
    auto s = query(db_, "INSERT INTO workflow_signals(signal_id,workflow_id,task_id,request_hash,spec_json,state,created_unix_ms,expires_unix_ms) VALUES(?,?,?,?,?,'waiting',?,?) ON CONFLICT(workflow_id,task_id) DO NOTHING");
    const auto expires = timeout_ms > 0 ? now + timeout_ms : 0;
    if (!s || !agent_db_bind_text(s.get(), 1, def.signal_id) || !agent_db_bind_text(s.get(), 2, def.workflow_id) ||
        !agent_db_bind_text(s.get(), 3, def.task_id) || !agent_db_bind_text(s.get(), 4, def.request_hash) ||
        !agent_db_bind_text(s.get(), 5, def.spec_json) || !agent_db_bind_i64(s.get(), 6, now) ||
        !agent_db_bind_i64(s.get(), 7, expires) || !agent_db_step_done(s.get())) return fail("cannot persist signal");
  }
  if (read(db_, def.workflow_id, def.task_id, out) != SQLITE_ROW ||
      out->signal_id != def.signal_id || out->request_hash != def.request_hash || out->spec_json != def.spec_json) return fail("signal identity changed");
  if (out->state == "waiting" && out->expires_unix_ms > 0 && out->expires_unix_ms <= now) {
    auto s = query(db_, "UPDATE workflow_signals SET state='expired',resolved_unix_ms=? WHERE signal_id=? AND state='waiting'");
    if (!s || !agent_db_bind_i64(s.get(), 1, now) || !agent_db_bind_text(s.get(), 2, out->signal_id) ||
        !agent_db_step_done(s.get()) || sqlite3_changes(db_) != 1) return fail("cannot expire signal");
    out->state = "expired"; out->resolved_unix_ms = now;
  }
  if (out->state == "waiting") {
    auto s = query(db_, "UPDATE workflow_tasks SET status='queued',attempt=MAX(0,attempt-1),ready_unix_ms=?,updated_unix_ms=? WHERE workflow_id=? AND task_id=? AND attempt=? AND request_json=? AND status='running' AND NOT EXISTS(SELECT 1 FROM workflow_budget_reservations r WHERE r.workflow_id=workflow_tasks.workflow_id AND r.task_id=workflow_tasks.task_id AND r.attempt=workflow_tasks.attempt AND r.state='reserved')");
    const auto ready = out->expires_unix_ms > 0 ? out->expires_unix_ms : std::numeric_limits<int64_t>::max();
    if (!s || !agent_db_bind_i64(s.get(), 1, ready) || !agent_db_bind_i64(s.get(), 2, now) ||
        !bind_task(s.get(), 3, expected) || !agent_db_step_done(s.get()) || sqlite3_changes(db_) != 1) return fail("cannot yield signal task");
    *yielded = true;
  }
  if (!exec_locked("COMMIT", error)) { *yielded = false; return fail("cannot commit signal wait"); }
  return true;
#endif
}

bool AgentDb::get_workflow_signal(const std::string& workflow, const std::string& task,
    WorkflowSignalRow* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow; (void)task; (void)out;
  if (error) *error = "SQLite support required"; return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out) { if (error) *error = "db not available"; return false; }
  const int rc = read(db_, workflow, task, out);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE && error) *error = agent_db_sqlite_err(db_);
  return rc == SQLITE_ROW;
#endif
}

bool AgentDb::list_workflow_signals(const std::string& workflow, std::vector<WorkflowSignalRow>* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow; (void)out;
  if (error) *error = "SQLite support required"; return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out) { if (error) *error = "db not available"; return false; }
  out->clear();
  auto s = query(db_, std::string("SELECT ") + columns + " FROM workflow_signals WHERE workflow_id=? ORDER BY created_unix_ms,task_id");
  if (!s || !agent_db_bind_text(s.get(), 1, workflow)) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  int rc;
  while ((rc = sqlite3_step(s.get())) == SQLITE_ROW) {
    WorkflowSignalRow row; read_row(s.get(), &row); out->push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  return true;
#endif
}

AgentDb::WorkflowSignalResolution AgentDb::resolve_workflow_signal(const WorkflowSignalRow& expected,
    const std::string& payload, const std::string& payload_hash,
    const std::string& delivery, WorkflowSignalRow* out, std::string* error) {
  using Result = WorkflowSignalResolution;
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)expected; (void)payload; (void)payload_hash; (void)delivery; (void)out;
  if (error) *error = "SQLite support required"; return Result::StorageError;
#else
  if (!out || payload.empty() || payload.size() > 16384 || payload_hash.empty() || payload_hash.size() > 80 ||
      delivery.empty() || delivery.size() > 128) return Result::Conflict;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN IMMEDIATE", error)) return Result::StorageError;
  const auto stop = [&](Result result, const std::string& message) {
    exec_locked("ROLLBACK", nullptr); if (error) *error = message; return result;
  };
  const auto commit = [&](Result result) {
    if (!exec_locked("COMMIT", error)) return stop(Result::StorageError, "cannot commit signal receipt");
    return result;
  };
  const int rc = read(db_, expected.workflow_id, expected.task_id, out);
  if (rc == SQLITE_DONE) return stop(Result::NotFound, "signal not found");
  if (rc != SQLITE_ROW) return stop(Result::StorageError, "cannot read signal");
  if (out->signal_id != expected.signal_id || out->request_hash != expected.request_hash ||
      out->spec_json != expected.spec_json) return stop(Result::Conflict, "signal definition changed");
  // Replays acknowledge an already recorded receipt, never wake or un-cancel work.
  if (out->state == "resolved") {
    if (out->delivery_id == delivery && out->payload_json == payload && out->payload_hash == payload_hash)
      return commit(Result::Replayed);
    return stop(Result::Conflict, "signal already recorded");
  }
  if (out->state == "expired") return stop(Result::Expired, "signal expired");
  const auto now = agent_db_unix_ms_now();
  {
    auto s = query(db_, "SELECT w.cancel_requested,COALESCE(w.deadline_unix_ms,0),w.status,t.status FROM workflows w JOIN workflow_tasks t ON t.workflow_id=w.workflow_id WHERE t.workflow_id=? AND t.task_id=?");
    if (!s || !agent_db_bind_text(s.get(), 1, out->workflow_id) || !agent_db_bind_text(s.get(), 2, out->task_id) ||
        sqlite3_step(s.get()) != SQLITE_ROW) return stop(Result::NotFound, "signal task not found");
    if (stopped(s.get(), now) || text(s.get(), 3) != "queued") return stop(Result::Stopped, "signal is not accepting input");
  }
  if (out->expires_unix_ms > 0 && out->expires_unix_ms <= now) {
    auto s = query(db_, "UPDATE workflow_signals SET state='expired',resolved_unix_ms=? WHERE signal_id=? AND state='waiting'");
    if (!s || !agent_db_bind_i64(s.get(), 1, now) || !agent_db_bind_text(s.get(), 2, out->signal_id) ||
        !agent_db_step_done(s.get()) || sqlite3_changes(db_) != 1) return stop(Result::StorageError, "cannot expire signal");
    out->state = "expired"; out->resolved_unix_ms = now;
    return commit(Result::Expired);
  }
  {
    auto s = query(db_, "UPDATE workflow_signals SET state='resolved',resolved_unix_ms=?,payload_json=?,payload_hash=?,delivery_id=? WHERE signal_id=? AND state='waiting'");
    if (!s || !agent_db_bind_i64(s.get(), 1, now) || !agent_db_bind_text(s.get(), 2, payload) ||
        !agent_db_bind_text(s.get(), 3, payload_hash) || !agent_db_bind_text(s.get(), 4, delivery) ||
        !agent_db_bind_text(s.get(), 5, out->signal_id) ||
        !agent_db_step_done(s.get()) || sqlite3_changes(db_) != 1) return stop(Result::StorageError, "cannot record signal");
  }
  {
    auto s = query(db_, "UPDATE workflow_tasks SET ready_unix_ms=?,updated_unix_ms=? WHERE workflow_id=? AND task_id=? AND status='queued'");
    if (!s || !agent_db_bind_i64(s.get(), 1, now) || !agent_db_bind_i64(s.get(), 2, now) ||
        !agent_db_bind_text(s.get(), 3, out->workflow_id) || !agent_db_bind_text(s.get(), 4, out->task_id) ||
        !agent_db_step_done(s.get()) || sqlite3_changes(db_) != 1) return stop(Result::StorageError, "cannot wake signal task");
  }
  out->state = "resolved"; out->resolved_unix_ms = now; out->payload_json = payload;
  out->payload_hash = payload_hash; out->delivery_id = delivery;
  return commit(Result::Recorded);
#endif
}
}  // namespace agentd
