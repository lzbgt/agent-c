#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <string>

namespace agentd {

bool AgentDb::insert_run(const RunRow& row, int64_t* out_run_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_run_id) *out_run_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO runs(session_id, job_id, ts_unix_ms, prompt, tools, model, base_url, stream_assistant, ok, "
    "stop_reason, steps_executed, tool_calls_total, tool_calls_by_tool_json, last_error_reason, "
    "request_json, response_json, replay_sha256, replay_sha256_alg, replay_sha256_schema, replay_error, "
    "error, http_status, http_body) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  bool ok =
    agent_db_bind_text(st, 1, row.session_id) &&
    agent_db_bind_text(st, 2, row.job_id) &&
    agent_db_bind_i64(st, 3, row.ts_unix_ms) &&
    agent_db_bind_text(st, 4, row.prompt) &&
    agent_db_bind_text(st, 5, row.tools) &&
    agent_db_bind_text(st, 6, row.model) &&
    agent_db_bind_text(st, 7, row.base_url) &&
    agent_db_bind_i32(st, 8, row.stream_assistant ? 1 : 0) &&
    agent_db_bind_i32(st, 9, row.ok ? 1 : 0) &&
    agent_db_bind_text(st, 10, row.stop_reason) &&
    agent_db_bind_i64(st, 11, row.steps_executed) &&
    agent_db_bind_i64(st, 12, row.tool_calls_total) &&
    agent_db_bind_text(st, 13, row.tool_calls_by_tool_json) &&
    agent_db_bind_text(st, 14, row.last_error_reason) &&
    agent_db_bind_text_or_null(st, 15, row.request_json) &&
    agent_db_bind_text_or_null(st, 16, row.response_json) &&
    agent_db_bind_text_or_null(st, 17, row.replay_sha256) &&
    agent_db_bind_text_or_null(st, 18, row.replay_sha256_alg) &&
    agent_db_bind_text_or_null(st, 19, row.replay_sha256_schema) &&
    agent_db_bind_text_or_null(st, 20, row.replay_error) &&
    agent_db_bind_text(st, 21, row.error) &&
    agent_db_bind_i32(st, 22, (int)row.http_status) &&
    agent_db_bind_text(st, 23, row.http_body) &&
    agent_db_step_done(st);

  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  const int64_t run_id = (int64_t)sqlite3_last_insert_rowid(db_);
  if (out_run_id) *out_run_id = run_id;

  if (!exec_locked("COMMIT;", out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  return true;
#endif
}

bool AgentDb::insert_event(
  int64_t run_id,
  int64_t ts_unix_ms,
  const std::string& type,
  const std::string& data_json,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)run_id;
  (void)ts_unix_ms;
  (void)type;
  (void)data_json;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO events(run_id, ts_unix_ms, type, data_json) VALUES(?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const bool ok =
    agent_db_bind_i64(st, 1, run_id) &&
    agent_db_bind_i64(st, 2, ts_unix_ms) &&
    agent_db_bind_text(st, 3, type) &&
    agent_db_bind_text(st, 4, data_json) &&
    agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_tool_record(const ToolRecordRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO tool_records(run_id, tool_name, tool_call_id, arguments_json, result_text, result_for_prompt_text, result_truncated_for_prompt) "
    "VALUES(?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const bool ok =
    agent_db_bind_i64(st, 1, row.run_id) &&
    agent_db_bind_text(st, 2, row.tool_name) &&
    agent_db_bind_text(st, 3, row.tool_call_id) &&
    agent_db_bind_text(st, 4, row.arguments_json) &&
    agent_db_bind_text(st, 5, row.result_text) &&
    agent_db_bind_text(st, 6, row.result_for_prompt_text) &&
    agent_db_bind_i32(st, 7, row.result_truncated_for_prompt ? 1 : 0) &&
    agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_artifact(const ArtifactRow& row, int64_t* out_artifact_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_artifact_id) *out_artifact_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  (void)out_artifact_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "insert_artifact: session_id is empty";
    return false;
  }
  if (row.path.empty()) {
    if (out_error) *out_error = "insert_artifact: path is empty";
    return false;
  }
  if (row.artifact_json.empty()) {
    if (out_error) *out_error = "insert_artifact: artifact_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Ensure session row exists (for foreign key).
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    return false;
  }

  const char* sql =
    "INSERT INTO artifacts(run_id, ts_unix_ms, session_id, tool_call_id, path, kind, mime, title, autoplay, repeat, artifact_json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, row.run_id);
  ok = ok && agent_db_bind_i64(st, 2, row.ts_unix_ms);
  ok = ok && agent_db_bind_text(st, 3, row.session_id);
  // tool_call_id may be empty; store NULL in that case for cleanliness.
  if (row.tool_call_id.empty()) {
    ok = ok && (sqlite3_bind_null(st, 4) == SQLITE_OK);
  } else {
    ok = ok && agent_db_bind_text(st, 4, row.tool_call_id);
  }
  ok = ok && agent_db_bind_text(st, 5, row.path);
  ok = ok && (row.kind.empty() ? (sqlite3_bind_null(st, 6) == SQLITE_OK) : agent_db_bind_text(st, 6, row.kind));
  ok = ok && (row.mime.empty() ? (sqlite3_bind_null(st, 7) == SQLITE_OK) : agent_db_bind_text(st, 7, row.mime));
  ok = ok && (row.title.empty() ? (sqlite3_bind_null(st, 8) == SQLITE_OK) : agent_db_bind_text(st, 8, row.title));
  ok = ok && agent_db_bind_i32(st, 9, row.autoplay ? 1 : 0);
  ok = ok && agent_db_bind_i32(st, 10, row.repeat);
  ok = ok && agent_db_bind_text(st, 11, row.artifact_json);

  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!agent_db_step_done(st)) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (out_artifact_id) {
    *out_artifact_id = (int64_t)sqlite3_last_insert_rowid(db_);
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::insert_ui_action(const UiActionRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "insert_ui_action: session_id is empty";
    return false;
  }
  if (row.action_json.empty()) {
    if (out_error) *out_error = "insert_ui_action: action_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Ensure session row exists (for foreign key).
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    return false;
  }

  const char* sql =
    "INSERT INTO ui_actions(run_id, ts_unix_ms, session_id, tool_call_id, type, title, message, path, mime, autoplay, repeat, action_json) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, row.run_id);
  ok = ok && agent_db_bind_i64(st, 2, row.ts_unix_ms);
  ok = ok && agent_db_bind_text(st, 3, row.session_id);
  if (row.tool_call_id.empty()) ok = ok && (sqlite3_bind_null(st, 4) == SQLITE_OK);
  else ok = ok && agent_db_bind_text(st, 4, row.tool_call_id);
  ok = ok && (row.type.empty() ? (sqlite3_bind_null(st, 5) == SQLITE_OK) : agent_db_bind_text(st, 5, row.type));
  ok = ok && (row.title.empty() ? (sqlite3_bind_null(st, 6) == SQLITE_OK) : agent_db_bind_text(st, 6, row.title));
  ok = ok && (row.message.empty() ? (sqlite3_bind_null(st, 7) == SQLITE_OK) : agent_db_bind_text(st, 7, row.message));
  ok = ok && (row.path.empty() ? (sqlite3_bind_null(st, 8) == SQLITE_OK) : agent_db_bind_text(st, 8, row.path));
  ok = ok && (row.mime.empty() ? (sqlite3_bind_null(st, 9) == SQLITE_OK) : agent_db_bind_text(st, 9, row.mime));
  ok = ok && agent_db_bind_i32(st, 10, row.autoplay ? 1 : 0);
  ok = ok && agent_db_bind_i32(st, 11, row.repeat);
  ok = ok && agent_db_bind_text(st, 12, row.action_json);

  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!agent_db_step_done(st)) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::insert_client_event(const ClientEventRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.session_id.empty()) {
    if (out_error) *out_error = "insert_client_event: session_id is empty";
    return false;
  }
  if (row.type.empty()) {
    if (out_error) *out_error = "insert_client_event: type is empty";
    return false;
  }
  if (row.data_json.empty()) {
    if (out_error) *out_error = "insert_client_event: data_json is empty";
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Ensure session row exists (for foreign key).
  if (!upsert_session_locked(row.session_id, row.ts_unix_ms, out_error)) {
    return false;
  }

  const char* sql = "INSERT INTO client_events(ts_unix_ms, session_id, type, data_json) VALUES(?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, row.ts_unix_ms);
  ok = ok && agent_db_bind_text(st, 2, row.session_id);
  ok = ok && agent_db_bind_text(st, 3, row.type);
  ok = ok && agent_db_bind_text(st, 4, row.data_json);

  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  if (!agent_db_step_done(st)) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }

  sqlite3_finalize(st);
  return true;
#endif
}


}  // namespace agentd
