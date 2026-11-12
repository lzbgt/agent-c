#include "agent_db.h"
#include "agent/multimodal_prefix.h"
#include "agent_db_sqlite.h"

namespace agentd {

bool AgentDb::load_session_messages(const std::string& session_id, std::vector<MessageRow>* messages,
                                    std::string* error, int64_t* revision) {
  if (error) error->clear();
  if (revision) *revision = 0;
  std::lock_guard<std::mutex> lock(mu_);
  // Snapshot messages and their generation in one read transaction. The mutex
  // alone would not fence a second database connection/process.
  if (!revision) return load_session_messages_locked(session_id, messages, error);
  if (!db_ || !exec_locked("BEGIN", error)) return false;
  if (!session_revision_locked(session_id, revision, error) ||
      !load_session_messages_locked(session_id, messages, error) || !exec_locked("COMMIT", error)) {
    exec_locked("ROLLBACK", nullptr); return false;
  }
  return true;
}

bool AgentDb::session_revision_locked(const std::string& id, int64_t* revision, std::string* error) {
  if (revision) *revision = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id; if (error) *error = "SQLite support required"; return false;
#else
  if (!db_ || !revision || id.empty()) { if (error) *error = "invalid session snapshot"; return false; }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "SELECT revision FROM session_revisions WHERE session_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = agent_db_sqlite_err(db_); return false;
  }
  const bool bound = agent_db_bind_text(stmt, 1, id);
  const int rc = bound ? sqlite3_step(stmt) : SQLITE_ERROR;
  if (rc == SQLITE_ROW) *revision = sqlite3_column_int64(stmt, 0);
  const bool ok = (rc == SQLITE_DONE || rc == SQLITE_ROW) && *revision >= 0 && *revision < 9007199254740991LL;
  if (!ok && error) *error = "session revision unavailable or exhausted";
  sqlite3_finalize(stmt); return ok;
#endif
}
bool AgentDb::session_matches_locked(const SessionFence& expected, std::string* error) {
  int64_t revision = 0;
  if (expected.revision < 0 || !session_revision_locked(expected.session_id, &revision, error)) return false;
  if (revision != expected.revision) { if (error) *error = "session revision conflict; newer conversation preserved"; return false; }
  return true;
}
bool AgentDb::session_matches(const SessionFence& expected, std::string* error) {
  if (error) error->clear();
  std::lock_guard<std::mutex> lock(mu_);
  return session_matches_locked(expected, error);
}
bool AgentDb::bump_session_revision_locked(const std::string& id, std::string* error) {
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id; if (error) *error = "SQLite support required"; return false;
#else
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "INSERT INTO session_revisions(session_id,revision) VALUES(?,1) "
    "ON CONFLICT(session_id) DO UPDATE SET revision=revision+1 WHERE revision<9007199254740990";
  if (!db_ || sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = "cannot prepare session generation"; return false;
  }
  const bool ok = agent_db_bind_text(stmt, 1, id) && agent_db_step_done(stmt) && sqlite3_changes(db_) == 1;
  if (!ok && error) *error = "cannot advance session generation";
  sqlite3_finalize(stmt); return ok;
#endif
}

bool AgentDb::load_session_messages_locked(
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
  int rc = SQLITE_DONE;
  while (ok && (rc = sqlite3_step(st)) == SQLITE_ROW) {
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
  if (ok && rc != SQLITE_DONE) ok = false;
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

bool AgentDb::replace_session_messages(const std::string& id, const std::vector<MessageRow>& messages,
                                       int64_t now, std::string* error, const SessionFence* expected) {
  if (error) error->clear();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || (expected && expected->session_id != id) || !exec_locked("BEGIN IMMEDIATE", error)) return false;
  if ((expected && !session_matches_locked(*expected, error)) ||
      !replace_session_messages_locked(id, messages, now, error) || !exec_locked("COMMIT", error)) {
    exec_locked("ROLLBACK", nullptr); return false;
  }
  return true;
}

bool AgentDb::replace_session_messages_locked(const std::string& id, const std::vector<MessageRow>& messages,
                                              int64_t now, std::string* error) {
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id; (void)messages; (void)now; if (error) *error = "SQLite support required"; return false;
#else
  if (!upsert_session_locked(id, now, error)) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE session_id=?", -1, &stmt, nullptr) != SQLITE_OK) {
    if (error) *error = agent_db_sqlite_err(db_); return false;
  }
  bool ok = agent_db_bind_text(stmt, 1, id) && agent_db_step_done(stmt);
  if (!ok && error) *error = agent_db_sqlite_err(db_);
  sqlite3_finalize(stmt); if (!ok) return false;
  const char* sql = "INSERT INTO messages(session_id,idx,role,content,mm_json,mm_bytes,mm_truncated,created_unix_ms) VALUES(?,?,?,?,?,?,?,?)";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) { if (error) *error = agent_db_sqlite_err(db_); return false; }
  for (size_t i = 0; ok && i < messages.size(); ++i) {
    sqlite3_reset(stmt); sqlite3_clear_bindings(stmt);
    const auto& message = messages[i];
    ok = agent_db_bind_text(stmt,1,id) && agent_db_bind_i64(stmt,2,static_cast<int64_t>(i)) &&
      agent_db_bind_text(stmt,3,message.role) && agent_db_bind_text(stmt,4,message.content) &&
      agent_db_bind_text(stmt,5,message.mm_json) && agent_db_bind_i64(stmt,6,message.mm_bytes) &&
      agent_db_bind_i64(stmt,7,message.mm_truncated) && agent_db_bind_i64(stmt,8,now) && agent_db_step_done(stmt);
  }
  if (!ok && error) *error = agent_db_sqlite_err(db_);
  sqlite3_finalize(stmt);
  return ok && bump_session_revision_locked(id, error);
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
  if (!exec_locked("BEGIN IMMEDIATE", out_error)) return false;
  auto rollback = [&]() { exec_locked("ROLLBACK", nullptr); return false; };
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE session_id=?;", -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return rollback();
  }
  const bool ok = agent_db_bind_text(st, 1, session_id) && agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok || !bump_session_revision_locked(session_id, out_error) || !exec_locked("COMMIT", out_error)) return rollback();
  return true;
#endif
}


}  // namespace agentd
