#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <memory>

namespace agentd {
#if defined(AGENT_HAVE_SQLITE3)
namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
bool bind_identity(sqlite3_stmt* s, const AgentDb::EdgeTaskRow& row, int first = 1) {
  return agent_db_bind_text(s,first,row.task_id) && agent_db_bind_text(s,first+1,row.step_id) &&
    agent_db_bind_text(s,first+2,row.node_id) && agent_db_bind_text(s,first+3,row.idempotency_key) &&
    agent_db_bind_text(s,first+4,row.payload_json);
}
}
#endif

bool AgentDb::record_edge_task_receipt(const EdgeTaskRow& row, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || row.result_json.empty() || row.result_json.size() > 32*1024*1024 ||
      row.result_sha256.size() != 71 || row.result_sha256.rfind("sha256:",0) != 0) {
    if (error) *error = "invalid device receipt";
    return false;
  }
  if (!exec_locked("BEGIN IMMEDIATE",error)) return false;
  const auto rollback = [&]() { exec_locked("ROLLBACK",nullptr); return false; };
  sqlite3_stmt* raw = nullptr;
  const char* insert = R"SQL(
INSERT INTO edge_task_receipts(task_id,step_id,node_id,idempotency_key,payload_json,result_json,result_sha256,attest_json,received_utc_ms)
SELECT task_id,step_id,node_id,idempotency_key,payload_json,?,?,?,?
FROM edge_tasks WHERE task_id=? AND step_id=? AND node_id=? AND idempotency_key=? AND payload_json=?
ON CONFLICT(task_id,step_id) DO NOTHING;
)SQL";
  if (sqlite3_prepare_v2(db_,insert,-1,&raw,nullptr) != SQLITE_OK) {
    if (error) *error = agent_db_sqlite_err(db_); return rollback();
  }
  {
    Statement stmt(raw,sqlite3_finalize);
    if (!(agent_db_bind_text(raw,1,row.result_json) && agent_db_bind_text(raw,2,row.result_sha256) &&
          agent_db_bind_text(raw,3,row.attest_json) && agent_db_bind_i64(raw,4,agent_db_unix_ms_now()) &&
          bind_identity(raw,row,5) && agent_db_step_done(raw))) {
      if (error) *error = agent_db_sqlite_err(db_); return rollback();
    }
  }
  const char* check = "SELECT 1 FROM edge_task_receipts WHERE task_id=? AND step_id=? AND node_id=? AND idempotency_key=? AND payload_json=? AND result_json=? AND result_sha256=?";
  if (sqlite3_prepare_v2(db_,check,-1,&raw,nullptr) != SQLITE_OK) {
    if (error) *error = agent_db_sqlite_err(db_); return rollback();
  }
  {
    Statement stmt(raw,sqlite3_finalize);
    if (!(bind_identity(raw,row) && agent_db_bind_text(raw,6,row.result_json) &&
          agent_db_bind_text(raw,7,row.result_sha256) && sqlite3_step(raw)==SQLITE_ROW)) {
      if (error) *error = "device receipt conflicts with the assignment or an earlier receipt";
      return rollback();
    }
  }
  if (!exec_locked("COMMIT",error)) return rollback();
  return true;
#endif
}

bool AgentDb::get_edge_task_receipt(const EdgeTaskRow& assignment, EdgeTaskRow* out, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)assignment; (void)out;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !out) return false;
  sqlite3_stmt* raw = nullptr;
  const char* query = "SELECT result_json,result_sha256,attest_json,received_utc_ms FROM edge_task_receipts WHERE task_id=? AND step_id=? AND node_id=? AND idempotency_key=? AND payload_json=?";
  if (sqlite3_prepare_v2(db_,query,-1,&raw,nullptr) != SQLITE_OK) {
    if (error) *error = agent_db_sqlite_err(db_); return false;
  }
  Statement stmt(raw,sqlite3_finalize);
  if (!bind_identity(raw,assignment)) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  const int rc = sqlite3_step(raw);
  if (rc != SQLITE_ROW) { if (rc != SQLITE_DONE && error) *error = agent_db_sqlite_err(db_); return false; }
  *out = assignment;
  out->state = "SUCCEEDED";
  out->error.clear();
  out->result_json = reinterpret_cast<const char*>(sqlite3_column_text(raw,0));
  out->result_sha256 = reinterpret_cast<const char*>(sqlite3_column_text(raw,1));
  out->attest_json = reinterpret_cast<const char*>(sqlite3_column_text(raw,2));
  out->updated_utc_ms = sqlite3_column_int64(raw,3);
  return true;
#endif
}

bool AgentDb::requeue_workflow_device_receipt(const WorkflowTaskRow& task, const EdgeTaskRow& assignment, std::string* error) {
  if (error) error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task; (void)assignment;
  if (error) *error = "SQLite support required";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || task.status != "error" || task.attempt >= 1000000 ||
      task.workflow_id != assignment.task_id || task.task_id != assignment.step_id) {
    if (error) *error = "only a failed device workflow task can be reconciled";
    return false;
  }
  if (!exec_locked("BEGIN IMMEDIATE",error)) return false;
  const auto rollback = [&]() { exec_locked("ROLLBACK",nullptr); return false; };
  sqlite3_stmt* raw = nullptr;
  const char* receipt = R"SQL(
SELECT 1 FROM edge_task_receipts r JOIN edge_tasks t ON r.task_id=t.task_id AND r.step_id=t.step_id
WHERE r.task_id=? AND r.step_id=? AND r.node_id=? AND r.idempotency_key=? AND r.payload_json=?
AND r.node_id=t.node_id AND r.idempotency_key=t.idempotency_key AND r.payload_json=t.payload_json
AND t.state IN ('TIMED_OUT','CANCELED','SUCCEEDED');
)SQL";
  if (sqlite3_prepare_v2(db_,receipt,-1,&raw,nullptr) != SQLITE_OK) {
    if (error) *error = agent_db_sqlite_err(db_); return rollback();
  }
  {
    Statement stmt(raw,sqlite3_finalize);
    if (!bind_identity(raw,assignment) || sqlite3_step(raw)!=SQLITE_ROW) {
      if (error) *error = "no matching validated device receipt is recorded";
      return rollback();
    }
  }
  const auto now = agent_db_unix_ms_now();
  auto update = [&](const char* sql, const auto& bind) {
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_,sql,-1,&s,nullptr) != SQLITE_OK) { if (error) *error=agent_db_sqlite_err(db_); return false; }
    Statement stmt(s,sqlite3_finalize);
    if (!bind(s) || !agent_db_step_done(s) || sqlite3_changes(db_)!=1) {
      if (error) *error = "reconciliation conflict, expired deadline, or owner cancellation";
      return false;
    }
    return true;
  };
  if (!update("UPDATE workflows SET status='queued',error='',result_json=NULL,updated_unix_ms=? WHERE workflow_id=? AND cancel_requested=0 AND status IN ('error','running','queued') AND (COALESCE(deadline_unix_ms,0)=0 OR deadline_unix_ms>?)",
      [&](auto* s) { return agent_db_bind_i64(s,1,now) && agent_db_bind_text(s,2,task.workflow_id) && agent_db_bind_i64(s,3,now); })) return rollback();
  if (!update("UPDATE workflow_tasks SET status='queued',ready_unix_ms=?,finished_unix_ms=NULL,error='',max_attempts=MAX(max_attempts,attempt+1),updated_unix_ms=? WHERE workflow_id=? AND task_id=? AND status='error' AND attempt=? AND request_json=? AND COALESCE(result_json,'')=?",
      [&](auto* s) { return agent_db_bind_i64(s,1,now) && agent_db_bind_i64(s,2,now) && agent_db_bind_text(s,3,task.workflow_id) &&
        agent_db_bind_text(s,4,task.task_id) && agent_db_bind_i32(s,5,task.attempt) &&
        agent_db_bind_text(s,6,task.request_json) && agent_db_bind_text(s,7,task.result_json); })) return rollback();
  if (!exec_locked("COMMIT",error)) return rollback();
  return true;
#endif
}
}  // namespace agentd
