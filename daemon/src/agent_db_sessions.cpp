#include "agent_db.h"
#include <algorithm>
#include "agent/multimodal_prefix.h"
#include "agent_db_sqlite.h"

namespace agentd {

AgentDb::SessionPageResult AgentDb::page_session_messages(
    const std::string& id, int64_t before, size_t limit, int64_t expected_revision,
    std::vector<IndexedMessageRow>* rows, int64_t* revision, bool* has_more, std::string* error) {
  if (error) error->clear();
  if (rows) rows->clear();
  if (has_more) *has_more = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)id; (void)before; (void)limit; (void)expected_revision; (void)revision;
  if (error) *error = "SQLite support required";
  return SessionPageResult::Error;
#else
  if (!rows || !revision || !has_more || id.empty() || limit < 1 || limit > 200 || before < 0) {
    if (error) *error = "invalid message page";
    return SessionPageResult::Error;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ || !exec_locked("BEGIN", error)) return SessionPageResult::Error;
  sqlite3_stmt* statement = nullptr;
  const auto fail = [&](SessionPageResult result, const char* message) {
    if (statement) { sqlite3_finalize(statement); statement = nullptr; }
    exec_locked("ROLLBACK", nullptr); rows->clear();
    if (error) *error = message;
    return result;
  };
  // Existence and generation are read in the same snapshot as the bounded rows.
  const char* identity = "SELECT COALESCE(r.revision,0) FROM sessions s LEFT JOIN session_revisions r ON r.session_id=s.session_id WHERE s.session_id=?";
  if (sqlite3_prepare_v2(db_, identity, -1, &statement, nullptr) != SQLITE_OK ||
      !agent_db_bind_text(statement, 1, id)) return fail(SessionPageResult::Error, "cannot read session page identity");
  int rc = sqlite3_step(statement);
  if (rc == SQLITE_DONE) return fail(SessionPageResult::NotFound, "session not found");
  if (rc != SQLITE_ROW) return fail(SessionPageResult::Error, "cannot read session page identity");
  *revision = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement); statement = nullptr;
  if (expected_revision >= 0 && expected_revision != *revision)
    return fail(SessionPageResult::Conflict, "session revision changed; restart pagination");
  const char* sql = "SELECT idx,role,content,mm_json,mm_bytes,mm_truncated FROM messages WHERE session_id=? AND idx<? ORDER BY idx DESC LIMIT ?";
  if (sqlite3_prepare_v2(db_, sql, -1, &statement, nullptr) != SQLITE_OK ||
      !agent_db_bind_text(statement, 1, id) || !agent_db_bind_i64(statement, 2, before) ||
      !agent_db_bind_i64(statement, 3, static_cast<int64_t>(limit + 1)))
    return fail(SessionPageResult::Error, "cannot prepare message page");
  constexpr size_t max_bytes = 2 * 1024 * 1024;
  size_t bytes = 0;
  while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
    if (rows->size() == limit) { *has_more = true; break; }
    const size_t size = static_cast<size_t>(sqlite3_column_bytes(statement, 1)) +
      static_cast<size_t>(sqlite3_column_bytes(statement, 2)) + static_cast<size_t>(sqlite3_column_bytes(statement, 3));
    if (size > max_bytes - bytes) {
      if (rows->empty()) return fail(SessionPageResult::TooLarge, "message exceeds page byte limit; use the legacy session export");
      *has_more = true; break;
    }
    bytes += size;
    IndexedMessageRow item;
    item.index = sqlite3_column_int64(statement, 0);
    const auto text = [&](int column) {
      const auto* value = sqlite3_column_text(statement, column);
      return value ? std::string(reinterpret_cast<const char*>(value), sqlite3_column_bytes(statement, column)) : std::string();
    };
    auto& row = item.message;
    row.role = text(1); row.content = text(2); row.mm_json = text(3);
    row.mm_bytes = sqlite3_column_int64(statement, 4);
    row.mm_truncated = sqlite3_column_int64(statement, 5) > 0 ? 1 : 0;
    if (row.mm_json.empty() && !row.content.empty()) {
      const char *plain = nullptr, *mm = nullptr; size_t plain_size = 0, mm_size = 0;
      if (agent_parse_multimodal_prefix(row.content.data(), row.content.size(), &plain, &plain_size, &mm, &mm_size) && mm && mm_size) {
        const std::string content = plain ? std::string(plain, plain_size) : std::string();
        row.mm_json.assign(mm, mm_size); row.content = content; row.mm_bytes = mm_size; row.mm_truncated = 0;
      }
    }
    if (row.mm_bytes <= 0) row.mm_bytes = row.mm_json.size();
    rows->push_back(std::move(item));
  }
  if (rc != SQLITE_DONE && rc != SQLITE_ROW) return fail(SessionPageResult::Error, "cannot read message page");
  sqlite3_finalize(statement); statement = nullptr;
  if (!exec_locked("COMMIT", error)) return fail(SessionPageResult::Error, "cannot commit message page");
  std::reverse(rows->begin(), rows->end());
  return SessionPageResult::Ok;
#endif
}

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
