#pragma once

#include <cstdint>
#include <chrono>
#include <string>

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace agentd {

#if defined(AGENT_HAVE_SQLITE3)
inline int64_t agent_db_unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

inline std::string agent_db_sqlite_err(sqlite3* db) {
  if (!db) return "sqlite: (db is null)";
  const char* msg = sqlite3_errmsg(db);
  return msg ? std::string(msg) : std::string("sqlite: unknown error");
}

inline bool agent_db_bind_text(sqlite3_stmt* st, int idx, const std::string& s) {
  return sqlite3_bind_text(st, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

inline bool agent_db_bind_text_or_null(sqlite3_stmt* st, int idx, const std::string& s) {
  if (s.empty()) return sqlite3_bind_null(st, idx) == SQLITE_OK;
  return agent_db_bind_text(st, idx, s);
}

inline bool agent_db_bind_i64(sqlite3_stmt* st, int idx, int64_t v) {
  return sqlite3_bind_int64(st, idx, (sqlite3_int64)v) == SQLITE_OK;
}

inline bool agent_db_bind_i64_or_null(sqlite3_stmt* st, int idx, int64_t v) {
  if (v <= 0) return sqlite3_bind_null(st, idx) == SQLITE_OK;
  return agent_db_bind_i64(st, idx, v);
}

inline bool agent_db_bind_i32(sqlite3_stmt* st, int idx, int v) {
  return sqlite3_bind_int(st, idx, v) == SQLITE_OK;
}

inline bool agent_db_bind_i32_or_null(sqlite3_stmt* st, int idx, int v, int unset) {
  if (v == unset) return sqlite3_bind_null(st, idx) == SQLITE_OK;
  return agent_db_bind_i32(st, idx, v);
}

inline bool agent_db_step_row(sqlite3_stmt* st) {
  const int rc = sqlite3_step(st);
  return rc == SQLITE_ROW;
}

inline bool agent_db_step_done(sqlite3_stmt* st) {
  const int rc = sqlite3_step(st);
  return rc == SQLITE_DONE;
}
#endif

}  // namespace agentd
