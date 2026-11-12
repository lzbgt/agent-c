#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <limits>
#include <memory>

namespace agentd {
#if defined(AGENT_HAVE_SQLITE3)
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
const char* columns = "effect_id,workflow_id,task_id,request_hash,mode,state,revision,attempt,created_unix_ms,updated_unix_ms,receipt_json,reconcile_url";
const char* metadata_columns = "effect_id,workflow_id,task_id,request_hash,mode,state,revision,attempt,created_unix_ms,updated_unix_ms,'',reconcile_url";
void read_row(sqlite3_stmt* stmt, AgentDb::WorkflowEffectRow* out) {
  out->effect_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  out->workflow_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  out->task_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  out->request_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  out->mode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  out->state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  out->revision = sqlite3_column_int64(stmt, 6);
  out->attempt = sqlite3_column_int64(stmt, 7);
  out->created_unix_ms = sqlite3_column_int64(stmt, 8);
  out->updated_unix_ms = sqlite3_column_int64(stmt, 9);
  out->receipt_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
  out->reconcile_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
}
}
#endif

bool AgentDb::get_workflow_effect(const std::string& effect_id, WorkflowEffectRow* out, bool* found, std::string* error) {
  if (error) error->clear();
  if (found) *found = false;
  if (out) *out = WorkflowEffectRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)effect_id;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out || !found || effect_id.empty()) {
    if (error) *error = "invalid effect lookup or unavailable database";
    return false;
  }
  sqlite3_stmt* raw = nullptr;
  const auto sql = std::string("SELECT ") + columns + " FROM workflow_effects WHERE effect_id=?";
  if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw, sqlite3_finalize);
  if (!agent_db_bind_text(raw, 1, effect_id)) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  const int rc = sqlite3_step(raw);
  if (rc == SQLITE_ROW) { read_row(raw, out); *found = true; return true; }
  if (rc == SQLITE_DONE) return true;
  if (error) *error = agent_db_sqlite_err(db_);
  return false;
#endif
}

bool AgentDb::prepare_workflow_effect(const WorkflowEffectRow& def, WorkflowEffectRow* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)def; (void)out;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out || def.effect_id.empty() || def.workflow_id.empty() || def.task_id.empty() ||
      def.request_hash.empty() || (def.mode != "guarded" && def.mode != "idempotent")) {
    if (error) *error = "invalid effect definition or unavailable database";
    return false;
  }
  sqlite3_stmt* raw = nullptr;
  const char* sql = "INSERT INTO workflow_effects(effect_id,workflow_id,task_id,request_hash,mode,state,created_unix_ms,updated_unix_ms,reconcile_url) VALUES(?,?,?,?,?,'prepared',?,?,?) ON CONFLICT DO NOTHING";
  if (sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  {
    Statement stmt(raw, sqlite3_finalize);
    const auto now = agent_db_unix_ms_now();
    if (!(agent_db_bind_text(raw,1,def.effect_id) && agent_db_bind_text(raw,2,def.workflow_id) &&
          agent_db_bind_text(raw,3,def.task_id) && agent_db_bind_text(raw,4,def.request_hash) &&
          agent_db_bind_text(raw,5,def.mode) && agent_db_bind_i64(raw,6,now) && agent_db_bind_i64(raw,7,now) && agent_db_bind_text(raw,8,def.reconcile_url) && agent_db_step_done(raw))) {
      if (error) *error = agent_db_sqlite_err(db_);
      return false;
    }
  }
  const auto select = std::string("SELECT ") + columns + " FROM workflow_effects WHERE effect_id=?";
  if (sqlite3_prepare_v2(db_, select.c_str(), -1, &raw, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw, sqlite3_finalize);
  if (!agent_db_bind_text(raw,1,def.effect_id) || sqlite3_step(raw) != SQLITE_ROW) { if (error) *error = "effect identity conflicts with an existing action"; return false; }
  read_row(raw, out);
  if (out->workflow_id != def.workflow_id || out->task_id != def.task_id || out->request_hash != def.request_hash || out->mode != def.mode) {
    if (error) *error = "effect identity/request mismatch; refusing a changed write";
    return false;
  }
  return true;
#endif
}

bool AgentDb::transition_workflow_effect(const WorkflowEffectRow& expected, const std::string& state,
                                        const std::string& receipt, WorkflowEffectRow* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)expected; (void)state; (void)receipt; (void)out;
  if (error) *error = "SQLite support required";
  return false;
#else
  const bool dispatch = state == "dispatching" &&
    (expected.state == "prepared" || (expected.mode == "idempotent" && (expected.state == "unknown" || expected.state == "dispatching" || expected.state == "completed")));
  const bool settle = (state == "completed" || state == "unknown") && expected.state == "dispatching";
  const bool reconcile = (state == "prepared" || state == "completed") && (expected.state == "unknown" || expected.state == "dispatching");
  if (!out || (!dispatch && !settle && !reconcile) || receipt.size() > 32 * 1024 * 1024 ||
      expected.revision == std::numeric_limits<int64_t>::max() || expected.attempt == std::numeric_limits<int64_t>::max()) {
    if (error) *error = "invalid effect transition or receipt size";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) { if (error) *error = "database unavailable"; return false; }
  sqlite3_stmt* raw = nullptr;
  const char* sql = "UPDATE workflow_effects SET state=?,revision=revision+1,attempt=attempt+?,updated_unix_ms=?,receipt_json=? WHERE effect_id=? AND revision=? AND state=? AND mode=? AND request_hash=? AND (?=0 OR EXISTS(SELECT 1 FROM workflows w WHERE w.workflow_id=workflow_effects.workflow_id AND w.cancel_requested=0 AND (COALESCE(w.deadline_unix_ms,0)=0 OR w.deadline_unix_ms>?)))";
  if (sqlite3_prepare_v2(db_,sql,-1,&raw,nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw,sqlite3_finalize);
  const auto now = agent_db_unix_ms_now();
  const bool ok = agent_db_bind_text(raw,1,state) && agent_db_bind_i64(raw,2,dispatch ? 1 : 0) &&
    agent_db_bind_i64(raw,3,now) && agent_db_bind_text(raw,4,receipt) && agent_db_bind_text(raw,5,expected.effect_id) &&
    agent_db_bind_i64(raw,6,expected.revision) && agent_db_bind_text(raw,7,expected.state) &&
    agent_db_bind_text(raw,8,expected.mode) && agent_db_bind_text(raw,9,expected.request_hash) &&
    agent_db_bind_i32(raw,10,dispatch ? 1 : 0) && agent_db_bind_i64(raw,11,now) && agent_db_step_done(raw);
  if (!ok || sqlite3_changes(db_) != 1) { if (error) *error = ok ? "effect revision conflict; stale attempt rejected" : agent_db_sqlite_err(db_); return false; }
  *out = expected;
  out->state = state; out->revision++; out->attempt += dispatch ? 1 : 0;
  out->updated_unix_ms = now; out->receipt_json = receipt;
  return true;
#endif
}

bool AgentDb::list_workflow_effects(const std::string& workflow_id, std::vector<WorkflowEffectRow>* out, std::string* error) {
  if (error) error->clear();
  if (out) out->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out) { if (error) *error = "database unavailable"; return false; }
  sqlite3_stmt* raw = nullptr;
  const auto sql = std::string("SELECT ") + metadata_columns + " FROM workflow_effects WHERE workflow_id=? ORDER BY task_id LIMIT 128";
  if (sqlite3_prepare_v2(db_,sql.c_str(),-1,&raw,nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  Statement stmt(raw,sqlite3_finalize);
  if (!agent_db_bind_text(raw,1,workflow_id)) return false;
  int rc;
  while ((rc = sqlite3_step(raw)) == SQLITE_ROW) { WorkflowEffectRow row; read_row(raw,&row); out->push_back(std::move(row)); }
  if (rc != SQLITE_DONE) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  return true;
#endif
}

bool AgentDb::requeue_workflow_effect(const WorkflowTaskRow& task, const WorkflowEffectRow& effect,
                                     const std::string& resolver, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task; (void)effect; (void)resolver;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || task.status != "error" || task.attempt >= 1000000 ||
      (effect.state != "unknown" && effect.state != "dispatching") ||
      effect.workflow_id != task.workflow_id || effect.task_id != task.task_id || resolver.empty()) {
    if (error) *error = "only a failed uncertain effect with a receipt endpoint can be reconciled";
    return false;
  }
  if (!exec_locked("BEGIN IMMEDIATE",error)) return false;
  const auto rollback = [&]() { exec_locked("ROLLBACK",nullptr); return false; };
  const auto now = agent_db_unix_ms_now();
  auto update = [&](const std::string& sql,const auto& bind) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_,sql.c_str(),-1,&raw,nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
    Statement stmt(raw,sqlite3_finalize);
    if (!bind(raw) || !agent_db_step_done(raw) || sqlite3_changes(db_) != 1) {
      if (error) *error = "reconciliation conflict, expired deadline, or owner cancellation";
      return false;
    }
    return true;
  };
  if (!update("UPDATE workflows SET status='queued',error='',result_json=NULL,updated_unix_ms=? WHERE workflow_id=? AND cancel_requested=0 AND (COALESCE(deadline_unix_ms,0)=0 OR deadline_unix_ms>?)",
      [&](auto* s) { return agent_db_bind_i64(s,1,now) && agent_db_bind_text(s,2,task.workflow_id) && agent_db_bind_i64(s,3,now); })) return rollback();
  if (!update("UPDATE workflow_tasks SET status='queued',ready_unix_ms=?,finished_unix_ms=NULL,error='',max_attempts=MAX(max_attempts,attempt+1),updated_unix_ms=? WHERE workflow_id=? AND task_id=? AND status='error' AND attempt=? AND request_json=?",
      [&](auto* s) { return agent_db_bind_i64(s,1,now) && agent_db_bind_i64(s,2,now) && agent_db_bind_text(s,3,task.workflow_id) && agent_db_bind_text(s,4,task.task_id) && agent_db_bind_i32(s,5,task.attempt) && agent_db_bind_text(s,6,task.request_json); })) return rollback();
  if (!update("UPDATE workflow_effects SET reconcile_url=?,revision=revision+1,updated_unix_ms=? WHERE effect_id=? AND revision=? AND state=?",
      [&](auto* s) { return agent_db_bind_text(s,1,resolver) && agent_db_bind_i64(s,2,now) && agent_db_bind_text(s,3,effect.effect_id) && agent_db_bind_i64(s,4,effect.revision) && agent_db_bind_text(s,5,effect.state); })) return rollback();
  if (!exec_locked("COMMIT",error)) return rollback();
  return true;
#endif
}

bool AgentDb::update_workflow_from_task_snapshot(const WorkflowRow& expected, const std::vector<WorkflowTaskRow>& tasks,
                                                const WorkflowRow& updated, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)expected; (void)tasks; (void)updated;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || updated.workflow_id != expected.workflow_id || !exec_locked("BEGIN IMMEDIATE",error)) return false;
  auto conflict = [&]() {
    if (error && error->empty()) *error = "workflow/task snapshot changed before finalization";
    exec_locked("ROLLBACK",nullptr); return false;
  };
  sqlite3_stmt* raw = nullptr;
  const char* check_sql = "SELECT status,attempt,COALESCE(error,''),COALESCE(result_json,'') FROM workflow_tasks WHERE workflow_id=? AND task_id=?";
  if (sqlite3_prepare_v2(db_,check_sql,-1,&raw,nullptr) != SQLITE_OK) return conflict();
  {
    Statement stmt(raw,sqlite3_finalize);
    for (const auto& task : tasks) {
      sqlite3_reset(raw); sqlite3_clear_bindings(raw);
      if (!agent_db_bind_text(raw,1,expected.workflow_id) || !agent_db_bind_text(raw,2,task.task_id) || sqlite3_step(raw) != SQLITE_ROW ||
          task.status != reinterpret_cast<const char*>(sqlite3_column_text(raw,0)) || task.attempt != sqlite3_column_int(raw,1) ||
          task.error != reinterpret_cast<const char*>(sqlite3_column_text(raw,2)) || task.result_json != reinterpret_cast<const char*>(sqlite3_column_text(raw,3))) return conflict();
    }
  }
  const char* sql = "UPDATE workflows SET status=?,error=?,result_json=?,updated_unix_ms=? WHERE workflow_id=? AND status=? AND cancel_requested=? AND (SELECT COUNT(*) FROM workflow_tasks WHERE workflow_id=?)=?";
  if (sqlite3_prepare_v2(db_,sql,-1,&raw,nullptr) != SQLITE_OK) return conflict();
  {
    Statement stmt(raw,sqlite3_finalize);
    if (!(agent_db_bind_text(raw,1,updated.status) && agent_db_bind_text(raw,2,updated.error) && agent_db_bind_text(raw,3,updated.result_json) &&
          agent_db_bind_i64(raw,4,updated.updated_unix_ms) && agent_db_bind_text(raw,5,expected.workflow_id) && agent_db_bind_text(raw,6,expected.status) &&
          agent_db_bind_i32(raw,7,expected.cancel_requested ? 1 : 0) && agent_db_bind_text(raw,8,expected.workflow_id) &&
          agent_db_bind_i64(raw,9,static_cast<int64_t>(tasks.size())) && agent_db_step_done(raw) && sqlite3_changes(db_) == 1)) return conflict();
  }
  if (!exec_locked("COMMIT",error)) return conflict();
  return true;
#endif
}
}  // namespace agentd
