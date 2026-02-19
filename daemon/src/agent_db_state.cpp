#include "agent_db.h"
#include "agent_db_sqlite.h"

namespace agentd {

bool AgentDb::get_scene_state(
  const std::string& session_id,
  std::string* out_scene_json,
  int64_t* out_updated_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_scene_json) out_scene_json->clear();
  if (out_updated_unix_ms) *out_updated_unix_ms = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (session_id.empty()) {
    if (out_error) *out_error = "get_scene_state: session_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT updated_unix_ms, scene_json FROM scene_states WHERE session_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, session_id);
  if (ok && agent_db_step_row(st)) {
    if (out_updated_unix_ms) *out_updated_unix_ms = sqlite3_column_int64(st, 0);
    const unsigned char* txt = sqlite3_column_text(st, 1);
    if (out_scene_json && txt) *out_scene_json = (const char*)txt;
  } else {
    // Missing row is not an error; treat as empty scene.
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::put_scene_state(
  const std::string& session_id,
  const std::string& scene_json,
  int64_t updated_unix_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)session_id;
  (void)scene_json;
  (void)updated_unix_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (session_id.empty()) {
    if (out_error) *out_error = "put_scene_state: session_id is empty";
    return false;
  }
  if (scene_json.empty()) {
    if (out_error) *out_error = "put_scene_state: scene_json is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  // Ensure parent session row exists (FK target).
  if (!upsert_session_locked(session_id, updated_unix_ms > 0 ? updated_unix_ms : agent_db_unix_ms_now(), out_error)) {
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "INSERT INTO scene_states(session_id, updated_unix_ms, scene_json) "
    "VALUES(?,?,?) "
    "ON CONFLICT(session_id) DO UPDATE SET updated_unix_ms=excluded.updated_unix_ms, scene_json=excluded.scene_json;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, session_id);
  ok = ok && agent_db_bind_i64(st, 2, updated_unix_ms > 0 ? updated_unix_ms : agent_db_unix_ms_now());
  ok = ok && agent_db_bind_text(st, 3, scene_json);
  if (ok) {
    const int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) {
      ok = false;
      if (out_error) *out_error = agent_db_sqlite_err(db_);
    }
  }
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::meta_get(const std::string& key, std::string* out_value, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_value) out_value->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)key;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT value FROM meta WHERE key=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, key);
  std::string v;
  if (ok && agent_db_step_row(st)) {
    const unsigned char* txt = sqlite3_column_text(st, 0);
    if (txt) v = (const char*)txt;
  } else {
    // Missing key is not an error.
    ok = true;
  }
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (out_value) *out_value = v;
  return ok;
#endif
}

bool AgentDb::meta_set(const std::string& key, const std::string& value, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)key;
  (void)value;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  const bool ok = agent_db_bind_text(st, 1, key) && agent_db_bind_text(st, 2, value) && agent_db_step_done(st);
  if (!ok && out_error) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

}  // namespace agentd
