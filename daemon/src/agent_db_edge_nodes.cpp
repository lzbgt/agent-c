#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <string>
#include <utility>
#include <vector>

namespace agentd {

bool AgentDb::upsert_edge_node(const EdgeNodeRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.node_id.empty()) {
    if (out_error) *out_error = "upsert_edge_node: node_id is empty";
    return false;
  }
  const int64_t now = agent_db_unix_ms_now();
  const int64_t hello = row.last_hello_utc_ms > 0 ? row.last_hello_utc_ms : 0;
  const int64_t hb = row.last_heartbeat_utc_ms > 0 ? row.last_heartbeat_utc_ms : 0;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_nodes(
  node_id, model, fw_git_sha, caps_sha256, manifest_json, tags_json, tools_json, hardware_presence_json, health_json,
  last_hello_utc_ms, last_heartbeat_utc_ms
) VALUES(?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(node_id) DO UPDATE SET
  model = CASE WHEN excluded.model IS NOT NULL AND excluded.model <> '' THEN excluded.model ELSE edge_nodes.model END,
  fw_git_sha = CASE WHEN excluded.fw_git_sha IS NOT NULL AND excluded.fw_git_sha <> '' THEN excluded.fw_git_sha ELSE edge_nodes.fw_git_sha END,
  caps_sha256 = CASE WHEN excluded.caps_sha256 IS NOT NULL AND excluded.caps_sha256 <> '' THEN excluded.caps_sha256 ELSE edge_nodes.caps_sha256 END,
  manifest_json = CASE
    WHEN excluded.manifest_json IS NOT NULL AND excluded.manifest_json <> '' THEN excluded.manifest_json
    WHEN excluded.caps_sha256 IS NOT NULL AND excluded.caps_sha256 <> '' AND excluded.caps_sha256 <> edge_nodes.caps_sha256 THEN NULL
    ELSE edge_nodes.manifest_json
  END,
  tags_json = CASE
    WHEN excluded.tags_json IS NOT NULL AND excluded.tags_json <> '' THEN excluded.tags_json
    WHEN excluded.caps_sha256 IS NOT NULL AND excluded.caps_sha256 <> '' AND excluded.caps_sha256 <> edge_nodes.caps_sha256 THEN NULL
    ELSE edge_nodes.tags_json
  END,
  tools_json = CASE
    WHEN excluded.tools_json IS NOT NULL AND excluded.tools_json <> '' THEN excluded.tools_json
    WHEN excluded.caps_sha256 IS NOT NULL AND excluded.caps_sha256 <> '' AND excluded.caps_sha256 <> edge_nodes.caps_sha256 THEN NULL
    ELSE edge_nodes.tools_json
  END,
  hardware_presence_json = CASE
    WHEN excluded.hardware_presence_json IS NOT NULL AND excluded.hardware_presence_json <> '' THEN excluded.hardware_presence_json
    WHEN excluded.caps_sha256 IS NOT NULL AND excluded.caps_sha256 <> '' AND excluded.caps_sha256 <> edge_nodes.caps_sha256 THEN NULL
    ELSE edge_nodes.hardware_presence_json
  END,
  health_json = CASE
    WHEN excluded.health_json IS NOT NULL AND excluded.health_json <> '' THEN excluded.health_json
    WHEN excluded.caps_sha256 IS NOT NULL AND excluded.caps_sha256 <> '' AND excluded.caps_sha256 <> edge_nodes.caps_sha256 THEN NULL
    ELSE edge_nodes.health_json
  END,
  last_hello_utc_ms = CASE WHEN excluded.last_hello_utc_ms IS NOT NULL AND excluded.last_hello_utc_ms > 0 THEN excluded.last_hello_utc_ms ELSE edge_nodes.last_hello_utc_ms END,
  last_heartbeat_utc_ms = CASE WHEN excluded.last_heartbeat_utc_ms IS NOT NULL AND excluded.last_heartbeat_utc_ms > 0 THEN excluded.last_heartbeat_utc_ms ELSE edge_nodes.last_heartbeat_utc_ms END;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.node_id);
  ok = ok && agent_db_bind_text_or_null(st, 2, row.model);
  ok = ok && agent_db_bind_text_or_null(st, 3, row.fw_git_sha);
  ok = ok && agent_db_bind_text_or_null(st, 4, row.caps_sha256);
  ok = ok && agent_db_bind_text_or_null(st, 5, row.manifest_json);
  ok = ok && agent_db_bind_text_or_null(st, 6, row.tags_json);
  ok = ok && agent_db_bind_text_or_null(st, 7, row.tools_json);
  ok = ok && agent_db_bind_text_or_null(st, 8, row.hardware_presence_json);
  ok = ok && agent_db_bind_text_or_null(st, 9, row.health_json);
  ok = ok && agent_db_bind_i64(st, 10, hello);
  ok = ok && agent_db_bind_i64(st, 11, hb);

  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  (void)now;
  return ok;
#endif
}

bool AgentDb::get_edge_node(const std::string& node_id, EdgeNodeRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeNodeRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (node_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT node_id, model, fw_git_sha, caps_sha256, manifest_json, tags_json, tools_json, hardware_presence_json, health_json,
       COALESCE(last_hello_utc_ms,0), COALESCE(last_heartbeat_utc_ms,0)
FROM edge_nodes
WHERE node_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, node_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const unsigned char* nid = sqlite3_column_text(st, 0);
    const unsigned char* model = sqlite3_column_text(st, 1);
    const unsigned char* sha = sqlite3_column_text(st, 2);
    const unsigned char* caps = sqlite3_column_text(st, 3);
    const unsigned char* mj = sqlite3_column_text(st, 4);
    const unsigned char* tags = sqlite3_column_text(st, 5);
    const unsigned char* tools = sqlite3_column_text(st, 6);
    const unsigned char* hw = sqlite3_column_text(st, 7);
    const unsigned char* hj = sqlite3_column_text(st, 8);
    out_row->node_id = nid ? (const char*)nid : "";
    out_row->model = model ? (const char*)model : "";
    out_row->fw_git_sha = sha ? (const char*)sha : "";
    out_row->caps_sha256 = caps ? (const char*)caps : "";
    out_row->manifest_json = mj ? (const char*)mj : "";
    out_row->tags_json = tags ? (const char*)tags : "";
    out_row->tools_json = tools ? (const char*)tools : "";
    out_row->hardware_presence_json = hw ? (const char*)hw : "";
    out_row->health_json = hj ? (const char*)hj : "";
    out_row->last_hello_utc_ms = sqlite3_column_int64(st, 9);
    out_row->last_heartbeat_utc_ms = sqlite3_column_int64(st, 10);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_edge_nodes(size_t max_rows, std::vector<EdgeNodeRow>* out_rows_desc, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT node_id, model, fw_git_sha, caps_sha256, manifest_json, tags_json, tools_json, hardware_presence_json, health_json,
       COALESCE(last_hello_utc_ms,0), COALESCE(last_heartbeat_utc_ms,0)
FROM edge_nodes
ORDER BY COALESCE(last_heartbeat_utc_ms,0) DESC, node_id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_i64(st, 1, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeNodeRow row;
    const unsigned char* nid = sqlite3_column_text(st, 0);
    const unsigned char* model = sqlite3_column_text(st, 1);
    const unsigned char* sha = sqlite3_column_text(st, 2);
    const unsigned char* caps = sqlite3_column_text(st, 3);
    const unsigned char* mj = sqlite3_column_text(st, 4);
    const unsigned char* tags = sqlite3_column_text(st, 5);
    const unsigned char* tools = sqlite3_column_text(st, 6);
    const unsigned char* hw = sqlite3_column_text(st, 7);
    const unsigned char* hj = sqlite3_column_text(st, 8);
    row.node_id = nid ? (const char*)nid : "";
    row.model = model ? (const char*)model : "";
    row.fw_git_sha = sha ? (const char*)sha : "";
    row.caps_sha256 = caps ? (const char*)caps : "";
    row.manifest_json = mj ? (const char*)mj : "";
    row.tags_json = tags ? (const char*)tags : "";
    row.tools_json = tools ? (const char*)tools : "";
    row.hardware_presence_json = hw ? (const char*)hw : "";
    row.health_json = hj ? (const char*)hj : "";
    row.last_hello_utc_ms = sqlite3_column_int64(st, 9);
    row.last_heartbeat_utc_ms = sqlite3_column_int64(st, 10);
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::insert_edge_inbox_message(const EdgeInboxMessageRow& row, bool* out_deduped, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_deduped) *out_deduped = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  (void)out_deduped;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.msg_id.empty() || row.type.empty() || row.envelope_json.empty()) {
    if (out_error) *out_error = "insert_edge_inbox_message: missing msg_id/type/envelope_json";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : agent_db_unix_ms_now();

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT OR IGNORE INTO edge_inbox_messages(msg_id, ts_utc_ms, type, from_id, to_id, envelope_json)
VALUES(?,?,?,?,?,?);
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.msg_id);
  ok = ok && agent_db_bind_i64(st, 2, ts);
  ok = ok && agent_db_bind_text(st, 3, row.type);
  ok = ok && agent_db_bind_text_or_null(st, 4, row.from_id);
  ok = ok && agent_db_bind_text_or_null(st, 5, row.to_id);
  ok = ok && agent_db_bind_text(st, 6, row.envelope_json);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_deduped) {
    *out_deduped = (sqlite3_changes(db_) == 0);
  }
  return ok;
#endif
}

bool AgentDb::insert_edge_inbox_message_with_seq_guard(
  const EdgeInboxMessageRow& row,
  const EdgeInboxAuthSeqGuard* guard_or_null,
  bool* out_deduped,
  bool* out_seq_rejected,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_deduped) *out_deduped = false;
  if (out_seq_rejected) *out_seq_rejected = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  (void)guard_or_null;
  (void)out_deduped;
  (void)out_seq_rejected;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.msg_id.empty() || row.type.empty() || row.envelope_json.empty()) {
    if (out_error) *out_error = "insert_edge_inbox_message_with_seq_guard: missing msg_id/type/envelope_json";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : agent_db_unix_ms_now();

  if (guard_or_null) {
    if (guard_or_null->node_id.empty()) {
      if (out_error) *out_error = "insert_edge_inbox_message_with_seq_guard: missing guard.node_id";
      return false;
    }
    if (guard_or_null->seq < 0) {
      if (out_error) *out_error = "insert_edge_inbox_message_with_seq_guard: invalid guard.seq";
      return false;
    }
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  char* err = nullptr;
  if (sqlite3_exec((sqlite3*)db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &err) != SQLITE_OK) {
    if (out_error) *out_error = err ? err : agent_db_sqlite_err((sqlite3*)db_);
    if (err) sqlite3_free(err);
    return false;
  }

  bool ok = true;
  bool deduped = false;
  bool seq_rejected = false;

  // 1) Insert inbox row (dedupe by msg_id).
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = R"SQL(
INSERT OR IGNORE INTO edge_inbox_messages(msg_id, ts_utc_ms, type, from_id, to_id, envelope_json)
VALUES(?,?,?,?,?,?);
)SQL";
    if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    } else {
      ok = ok && agent_db_bind_text(st, 1, row.msg_id);
      ok = ok && agent_db_bind_i64(st, 2, ts);
      ok = ok && agent_db_bind_text(st, 3, row.type);
      ok = ok && agent_db_bind_text_or_null(st, 4, row.from_id);
      ok = ok && agent_db_bind_text_or_null(st, 5, row.to_id);
      ok = ok && agent_db_bind_text(st, 6, row.envelope_json);
      ok = ok && agent_db_step_done(st);
      if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
      sqlite3_finalize(st);
      if (ok) {
        deduped = (sqlite3_changes((sqlite3*)db_) == 0);
      }
    }
  }

  // 2) For new messages only, enforce/bump seq (anti-replay) if configured.
  if (ok && !deduped && guard_or_null) {
    sqlite3_stmt* st = nullptr;
    const char* sql = R"SQL(
INSERT INTO edge_nodes(node_id, last_auth_seq)
VALUES(?,?)
ON CONFLICT(node_id) DO UPDATE SET
  last_auth_seq = excluded.last_auth_seq
WHERE edge_nodes.last_auth_seq IS NULL OR edge_nodes.last_auth_seq < excluded.last_auth_seq;
)SQL";
    if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = agent_db_sqlite_err((sqlite3*)db_);
    } else {
      ok = ok && agent_db_bind_text(st, 1, guard_or_null->node_id);
      ok = ok && agent_db_bind_i64(st, 2, guard_or_null->seq);
      ok = ok && agent_db_step_done(st);
      if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err((sqlite3*)db_);
      sqlite3_finalize(st);
      if (ok) {
        const int n = sqlite3_changes((sqlite3*)db_);
        if (n == 0) {
          // Seq is not strictly increasing. Reject and roll back the inbox insert.
          seq_rejected = true;
        }
      }
    }
  }

  if (seq_rejected) {
    char* rerr = nullptr;
    (void)sqlite3_exec((sqlite3*)db_, "ROLLBACK;", nullptr, nullptr, &rerr);
    if (rerr) sqlite3_free(rerr);
    if (out_deduped) *out_deduped = false;
    if (out_seq_rejected) *out_seq_rejected = true;
    return true;
  }

  if (ok) {
    char* cerr = nullptr;
    if (sqlite3_exec((sqlite3*)db_, "COMMIT;", nullptr, nullptr, &cerr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = cerr ? cerr : agent_db_sqlite_err((sqlite3*)db_);
      if (cerr) sqlite3_free(cerr);
    }
  } else {
    char* rerr = nullptr;
    (void)sqlite3_exec((sqlite3*)db_, "ROLLBACK;", nullptr, nullptr, &rerr);
    if (rerr) sqlite3_free(rerr);
  }

  if (ok && out_deduped) *out_deduped = deduped;
  if (ok && out_seq_rejected) *out_seq_rejected = false;
  return ok;
#endif
}

bool AgentDb::get_edge_inbox_message_processed(const std::string& msg_id, bool* out_processed, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_processed) *out_processed = false;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)msg_id;
  (void)out_processed;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_processed || msg_id.empty()) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = "SELECT COALESCE(processed,0) FROM edge_inbox_messages WHERE msg_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, msg_id);
  bool found = false;
  if (ok && agent_db_step_row(st)) {
    found = true;
    const int64_t v = sqlite3_column_int64(st, 0);
    *out_processed = (v != 0);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::mark_edge_inbox_message_processed(const std::string& msg_id, int64_t processed_utc_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)msg_id;
  (void)processed_utc_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (msg_id.empty()) return false;
  const int64_t ts = processed_utc_ms > 0 ? processed_utc_ms : agent_db_unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "UPDATE edge_inbox_messages SET processed=1, processed_utc_ms=? WHERE msg_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_i64(st, 1, ts);
  ok = ok && agent_db_bind_text(st, 2, msg_id);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  return sqlite3_changes(db_) > 0;
#endif
}

bool AgentDb::insert_edge_outbox_message(const EdgeOutboxMessageRow& row, int64_t* out_outbox_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_outbox_id) *out_outbox_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.node_id.empty() || row.envelope_json.empty()) {
    if (out_error) *out_error = "insert_edge_outbox_message: missing node_id/envelope_json";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : agent_db_unix_ms_now();

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_outbox_messages(node_id, ts_utc_ms, envelope_json) VALUES(?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.node_id);
  ok = ok && agent_db_bind_i64(st, 2, ts);
  ok = ok && agent_db_bind_text(st, 3, row.envelope_json);
  ok = ok && agent_db_step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_outbox_id) *out_outbox_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

bool AgentDb::list_edge_outbox_messages(
  const std::string& node_id,
  int64_t after_outbox_id,
  size_t max_rows,
  std::vector<EdgeOutboxMessageRow>* out_rows_asc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_asc) out_rows_asc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  (void)after_outbox_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (node_id.empty()) {
    if (out_error) *out_error = "list_edge_outbox_messages: node_id is empty";
    return false;
  }
  if (!out_rows_asc) return false;
  if (max_rows == 0) max_rows = 128;
  if (max_rows > 2048) max_rows = 2048;
  if (after_outbox_id < 0) after_outbox_id = 0;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT outbox_id, node_id, ts_utc_ms, envelope_json
FROM edge_outbox_messages
WHERE node_id=? AND outbox_id>?
ORDER BY outbox_id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, node_id);
  ok = ok && agent_db_bind_i64(st, 2, after_outbox_id);
  ok = ok && agent_db_bind_i64(st, 3, (int64_t)max_rows);
  while (ok && agent_db_step_row(st)) {
    EdgeOutboxMessageRow row;
    row.outbox_id = sqlite3_column_int64(st, 0);
    const unsigned char* nid = sqlite3_column_text(st, 1);
    row.node_id = nid ? (const char*)nid : "";
    row.ts_utc_ms = sqlite3_column_int64(st, 2);
    const unsigned char* env = sqlite3_column_text(st, 3);
    row.envelope_json = env ? (const char*)env : "";
    out_rows_asc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = agent_db_sqlite_err(db_);
  return ok;
#endif
}


}  // namespace agentd
