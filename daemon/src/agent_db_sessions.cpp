#include "agent_db.h"
#include "agent/multimodal_prefix.h"
#include "agent_db_sqlite.h"

namespace agentd {

bool AgentDb::load_session_messages(
  const std::string& session_id,
  std::vector<MessageRow>* out_messages,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_messages) out_messages->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
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
    "SELECT role, content, mm_json, mm_bytes, mm_truncated "
    "FROM messages WHERE session_id=? ORDER BY idx ASC;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id);
  std::vector<MessageRow> rows;
  while (ok && agent_db_step_row(st)) {
    MessageRow row;
    const unsigned char* role = sqlite3_column_text(st, 0);
    const unsigned char* content = sqlite3_column_text(st, 1);
    const unsigned char* mm_json = sqlite3_column_text(st, 2);
    const int64_t mm_bytes = sqlite3_column_int64(st, 3);
    const int64_t mm_truncated = sqlite3_column_int64(st, 4);

    row.role = role ? (const char*)role : "";
    row.content = content ? (const char*)content : "";
    row.mm_json = mm_json ? (const char*)mm_json : "";
    row.mm_bytes = mm_bytes > 0 ? mm_bytes : (row.mm_json.empty() ? 0 : (int64_t)row.mm_json.size());
    row.mm_truncated = mm_truncated > 0 ? 1 : 0;

    if (row.mm_json.empty() && !row.content.empty()) {
      const char* text = nullptr;
      size_t text_len = 0;
      const char* raw_mm = nullptr;
      size_t raw_mm_len = 0;
      const uint8_t has_mm = agent_parse_multimodal_prefix(
        row.content.c_str(),
        row.content.size(),
        &text,
        &text_len,
        &raw_mm,
        &raw_mm_len
      );
      if (has_mm && raw_mm && raw_mm_len > 0) {
        row.mm_json.assign(raw_mm, raw_mm_len);
        row.mm_bytes = (int64_t)raw_mm_len;
        row.mm_truncated = 0;
        if (text && text_len > 0) {
          row.content.assign(text, text_len);
        } else {
          row.content.clear();
        }
      }
    }
    rows.push_back(std::move(row));
    if (rows.size() > 200000) {
      ok = false;
      if (out_error) *out_error = "too many messages";
      break;
    }
  }
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_messages) *out_messages = std::move(rows);
  return ok;
#endif
}

bool AgentDb::list_sessions(std::vector<std::string>* out_session_ids_desc, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_session_ids_desc) out_session_ids_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT session_id FROM sessions ORDER BY updated_unix_ms DESC LIMIT 5000;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  std::vector<std::string> ids;
  while (agent_db_step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    if (txt) ids.push_back((const char*)txt);
  }
  sqlite3_finalize(st);
  if (out_session_ids_desc) *out_session_ids_desc = std::move(ids);
  return true;
#endif
}

bool AgentDb::upsert_session(const std::string& session_id, int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  return upsert_session_locked(session_id, now_unix_ms, out_error);
#endif
}

bool AgentDb::session_exists(const std::string& session_id, bool* out_exists, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_exists) *out_exists = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT 1 FROM sessions WHERE session_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id);
  bool exists = false;
  if (ok && agent_db_step_row(st)) {
    exists = true;
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (out_exists) *out_exists = exists;
  return ok;
#endif
}

bool AgentDb::upsert_session_locked(const std::string& session_id, int64_t now_unix_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO sessions(session_id, created_unix_ms, updated_unix_ms) VALUES(?,?,?) "
    "ON CONFLICT(session_id) DO UPDATE SET updated_unix_ms=excluded.updated_unix_ms;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const bool ok =
    agent_db_bind_text(st, 1, session_id) &&
    agent_db_bind_i64(st, 2, now_unix_ms) &&
    agent_db_bind_i64(st, 3, now_unix_ms) &&
    agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::replace_session_messages(
  const std::string& session_id,
  const std::vector<MessageRow>& messages,
  int64_t now_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)messages;
  (void)now_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  // Transaction.
  if (!exec_locked("BEGIN IMMEDIATE TRANSACTION;", out_error)) return false;

  if (!upsert_session_locked(session_id, now_unix_ms, out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE session_id=?;", -1, &del, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      (void)exec_locked("ROLLBACK;", nullptr);
      return false;
    }
    const bool ok = agent_db_bind_text(del, 1, session_id) && agent_db_step_done(del);
    if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(del);
    if (!ok) {
      (void)exec_locked("ROLLBACK;", nullptr);
      return false;
    }
  }

  sqlite3_stmt* ins = nullptr;
  const char* sql =
    "INSERT INTO messages(session_id, idx, role, content, mm_json, mm_bytes, mm_truncated, created_unix_ms) "
    "VALUES(?,?,?,?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &ins, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }

  bool ok = true;
  for (size_t i = 0; i < messages.size(); i++) {
    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    ok = ok &&
      agent_db_bind_text(ins, 1, session_id) &&
      agent_db_bind_i32(ins, 2, (int)i) &&
      agent_db_bind_text(ins, 3, messages[i].role) &&
      agent_db_bind_text(ins, 4, messages[i].content) &&
      agent_db_bind_text(ins, 5, messages[i].mm_json) &&
      agent_db_bind_i64(ins, 6, messages[i].mm_bytes) &&
      agent_db_bind_i64(ins, 7, messages[i].mm_truncated) &&
      agent_db_bind_i64(ins, 8, now_unix_ms) &&
      agent_db_step_done(ins);
    if (!ok) {
      if (out_error) *out_error = agent_db_sqlite_err(db_);
      break;
    }
  }
  sqlite3_finalize(ins);

  if (!ok) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  if (!exec_locked("COMMIT;", out_error)) {
    (void)exec_locked("ROLLBACK;", nullptr);
    return false;
  }
  return true;
#endif
}

bool AgentDb::delete_session(const std::string& session_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE session_id=?;", -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const bool ok = agent_db_bind_text(st, 1, session_id) && agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}


}  // namespace agentd
