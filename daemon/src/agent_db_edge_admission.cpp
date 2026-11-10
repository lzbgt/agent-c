#include "agent_db.h"
#include "agent_db_sqlite.h"
#include <algorithm>
#include <memory>

namespace agentd {
bool AgentDb::admit_edge_task_assignment(
    const EdgeTaskRow &task, const EdgeOutboxMessageRow &message,
    const EdgeTaskEventRow &event, int max_per_minute, int cooldown_ms,
    int64_t *out_outbox_id, bool *out_deduped, int *out_http_status,
    std::string *error) {
  if (error)
    error->clear();
  if (out_outbox_id)
    *out_outbox_id = 0;
  if (out_deduped)
    *out_deduped = false;
  if (out_http_status)
    *out_http_status = 500;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task;
  (void)message;
  (void)event;
  (void)max_per_minute;
  (void)cooldown_ms;
  if (error)
    *error = "SQLite support required";
  return false;
#else
  const auto reject = [&](int code, const std::string &reason) {
    if (out_http_status)
      *out_http_status = code;
    if (error)
      *error = reason;
    return false;
  };
  if (task.task_id.empty() || task.step_id.empty() || task.node_id.empty() ||
      task.idempotency_key.empty() || task.payload_json.empty() ||
      task.state != "QUEUED" || task.deadline_utc_ms <= 0 ||
      (task.mode != "invoke" && task.mode != "agent") ||
      message.node_id != task.node_id || message.envelope_json.empty() ||
      event.task_id != task.task_id || event.step_id != task.step_id ||
      event.state != "QUEUED" || max_per_minute < 0 || cooldown_ms < 0)
    return reject(400, "invalid device assignment admission");
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN IMMEDIATE", error))
    return false;
  const auto rollback = [&](int code, const std::string &reason) {
    exec_locked("ROLLBACK", nullptr);
    return reject(code, reason);
  };
  using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
  const auto statement = [&](const char *sql) {
    sqlite3_stmt *raw = nullptr;
    sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
    return Statement(raw, sqlite3_finalize);
  };
  const auto sql_error = [&]() {
    const auto detail = agent_db_sqlite_err(db_);
    return rollback(500, detail);
  };
  const auto text = [](sqlite3_stmt *stmt, int column) {
    const auto *p = sqlite3_column_text(stmt, column);
    return p ? std::string(reinterpret_cast<const char *>(p)) : std::string();
  };
  {
    auto s = statement("SELECT node_id,idempotency_key,mode,payload_json FROM "
                       "edge_tasks WHERE task_id=? AND step_id=?");
    if (!s || !agent_db_bind_text(s.get(), 1, task.task_id) ||
        !agent_db_bind_text(s.get(), 2, task.step_id))
      return sql_error();
    const int state = sqlite3_step(s.get());
    if (state == SQLITE_ROW) {
      if (text(s.get(), 0) != task.node_id ||
          text(s.get(), 1) != task.idempotency_key ||
          text(s.get(), 2) != task.mode ||
          text(s.get(), 3) != task.payload_json)
        return rollback(409, "device assignment identity/payload conflict; "
                             "original operation preserved");
      if (!exec_locked("COMMIT", error))
        return rollback(500, "cannot settle duplicate assignment lookup");
      if (out_deduped)
        *out_deduped = true;
      if (out_http_status)
        *out_http_status = 200;
      return true;
    }
    if (state != SQLITE_DONE)
      return sql_error();
  }
  {
    auto s = statement(
        "SELECT 1 FROM edge_tasks WHERE node_id=? AND idempotency_key=?");
    if (!s || !agent_db_bind_text(s.get(), 1, task.node_id) ||
        !agent_db_bind_text(s.get(), 2, task.idempotency_key))
      return sql_error();
    const int state = sqlite3_step(s.get());
    if (state == SQLITE_ROW)
      return rollback(
          409, "idempotency_key collision with different task_id/step_id");
    if (state != SQLITE_DONE)
      return sql_error();
  }
  const auto now = agent_db_unix_ms_now();
  if (task.deadline_utc_ms <= now)
    return rollback(409, "device assignment deadline has expired");
  // A workflow stop/deadline observed before admission cannot publish a new
  // command.
  {
    auto s = statement(
        "SELECT w.cancel_requested,COALESCE(w.deadline_unix_ms,0) FROM "
        "workflows w JOIN workflow_tasks t ON t.workflow_id=w.workflow_id "
        "WHERE w.workflow_id=? AND t.task_id=?");
    if (!s || !agent_db_bind_text(s.get(), 1, task.task_id) ||
        !agent_db_bind_text(s.get(), 2, task.step_id))
      return sql_error();
    const int state = sqlite3_step(s.get());
    if (state == SQLITE_ROW && (sqlite3_column_int(s.get(), 0) != 0 ||
                                (sqlite3_column_int64(s.get(), 1) > 0 &&
                                 sqlite3_column_int64(s.get(), 1) <= now)))
      return rollback(409,
                      "workflow stopped or expired before device admission");
    if (state != SQLITE_ROW && state != SQLITE_DONE)
      return sql_error();
  }
  if (!task.resource_lock.empty()) {
    auto s =
        statement("SELECT 1 FROM edge_tasks WHERE node_id=? AND "
                  "resource_lock=? AND state IN ('QUEUED','RUNNING') LIMIT 1");
    if (!s || !agent_db_bind_text(s.get(), 1, task.node_id) ||
        !agent_db_bind_text(s.get(), 2, task.resource_lock))
      return sql_error();
    const int state = sqlite3_step(s.get());
    if (state == SQLITE_ROW)
      return rollback(429, "resource_locked: " + task.resource_lock);
    if (state != SQLITE_DONE)
      return sql_error();
  }
  EdgeToolRateStateRow rate;
  const bool limited = max_per_minute > 0 || cooldown_ms > 0;
  if (limited) {
    rate.node_id = task.node_id;
    rate.tool_name = task.tool_name;
    auto s =
        statement("SELECT window_start_utc_ms,window_count,last_call_utc_ms "
                  "FROM edge_tool_rate_state WHERE node_id=? AND tool_name=?");
    if (!s || !agent_db_bind_text(s.get(), 1, rate.node_id) ||
        !agent_db_bind_text(s.get(), 2, rate.tool_name))
      return sql_error();
    const int state = sqlite3_step(s.get());
    if (state == SQLITE_ROW) {
      rate.window_start_utc_ms = sqlite3_column_int64(s.get(), 0);
      rate.window_count = sqlite3_column_int(s.get(), 1);
      rate.last_call_utc_ms = sqlite3_column_int64(s.get(), 2);
    } else if (state != SQLITE_DONE)
      return sql_error();
    if (now < rate.last_call_utc_ms || now < rate.window_start_utc_ms)
      return rollback(429, "rate_limited: clock moved backwards");
    if (cooldown_ms > 0 && rate.last_call_utc_ms > 0 &&
        now - rate.last_call_utc_ms < cooldown_ms)
      return rollback(429, "rate_limited: cooldown");
    if (rate.window_start_utc_ms <= 0 ||
        now - rate.window_start_utc_ms >= 60000) {
      rate.window_start_utc_ms = now;
      rate.window_count = 0;
    }
    if (max_per_minute > 0 && rate.window_count >= max_per_minute)
      return rollback(429, "rate_limited: max_per_minute");
    rate.window_count = std::min(rate.window_count, INT32_MAX - 1) + 1;
    rate.last_call_utc_ms = now;
  }
  {
    auto s = statement(
        "INSERT INTO "
        "edge_tasks(task_id,step_id,node_id,idempotency_key,mode,deadline_utc_"
        "ms,payload_json,state,created_utc_ms,updated_utc_ms,trace_id,resource_"
        "lock,tool_name) VALUES(?,?,?,?,?,?,?,'QUEUED',?,?,?,?,?)");
    if (!s || !agent_db_bind_text(s.get(), 1, task.task_id) ||
        !agent_db_bind_text(s.get(), 2, task.step_id) ||
        !agent_db_bind_text(s.get(), 3, task.node_id) ||
        !agent_db_bind_text(s.get(), 4, task.idempotency_key) ||
        !agent_db_bind_text(s.get(), 5, task.mode) ||
        !agent_db_bind_i64(s.get(), 6, task.deadline_utc_ms) ||
        !agent_db_bind_text(s.get(), 7, task.payload_json) ||
        !agent_db_bind_i64(s.get(), 8, now) ||
        !agent_db_bind_i64(s.get(), 9, now) ||
        !agent_db_bind_text_or_null(s.get(), 10, task.trace_id) ||
        !agent_db_bind_text_or_null(s.get(), 11, task.resource_lock) ||
        !agent_db_bind_text_or_null(s.get(), 12, task.tool_name) ||
        !agent_db_step_done(s.get()))
      return sql_error();
  }
  int64_t outbox_id = 0;
  {
    auto s = statement(
        "INSERT INTO edge_outbox_messages(node_id,ts_utc_ms,envelope_json) "
        "VALUES(?,?,?)");
    if (!s || !agent_db_bind_text(s.get(), 1, message.node_id) ||
        !agent_db_bind_i64(s.get(), 2, now) ||
        !agent_db_bind_text(s.get(), 3, message.envelope_json) ||
        !agent_db_step_done(s.get()))
      return sql_error();
    outbox_id = sqlite3_last_insert_rowid(db_);
  }
  {
    auto s = statement("INSERT INTO "
                       "edge_task_events(task_id,step_id,ts_utc_ms,state,data_"
                       "json) VALUES(?,?,?,'QUEUED',?)");
    if (!s || !agent_db_bind_text(s.get(), 1, task.task_id) ||
        !agent_db_bind_text(s.get(), 2, task.step_id) ||
        !agent_db_bind_i64(s.get(), 3, now) ||
        !agent_db_bind_text(s.get(), 4, event.data_json) ||
        !agent_db_step_done(s.get()))
      return sql_error();
  }
  if (limited) {
    auto s = statement(
        "INSERT INTO "
        "edge_tool_rate_state(node_id,tool_name,window_start_utc_ms,window_"
        "count,last_call_utc_ms) VALUES(?,?,?,?,?) ON "
        "CONFLICT(node_id,tool_name) DO UPDATE SET "
        "window_start_utc_ms=excluded.window_start_utc_ms,window_count="
        "excluded.window_count,last_call_utc_ms=excluded.last_call_utc_ms");
    if (!s || !agent_db_bind_text(s.get(), 1, rate.node_id) ||
        !agent_db_bind_text(s.get(), 2, rate.tool_name) ||
        !agent_db_bind_i64(s.get(), 3, rate.window_start_utc_ms) ||
        !agent_db_bind_i32(s.get(), 4, rate.window_count) ||
        !agent_db_bind_i64(s.get(), 5, rate.last_call_utc_ms) ||
        !agent_db_step_done(s.get()))
      return sql_error();
  }
  if (!exec_locked("COMMIT", error))
    return rollback(500, "device assignment transaction failed");
  if (out_outbox_id)
    *out_outbox_id = outbox_id;
  if (out_http_status)
    *out_http_status = 200;
  return true;
#endif
}
} // namespace agentd
