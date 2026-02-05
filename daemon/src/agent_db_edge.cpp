#include "agent_db.h"

#include <chrono>
#include <string>

namespace agentd {
namespace {

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string sqlite_err(sqlite3* db) {
  if (!db) return "sqlite: (db is null)";
  const char* msg = sqlite3_errmsg(db);
  return msg ? std::string(msg) : std::string("sqlite: unknown error");
}

static bool bind_text(sqlite3_stmt* st, int idx, const std::string& s) {
  return sqlite3_bind_text(st, idx, s.c_str(), (int)s.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

static bool bind_text_or_null(sqlite3_stmt* st, int idx, const std::string& s) {
  if (s.empty()) return sqlite3_bind_null(st, idx) == SQLITE_OK;
  return bind_text(st, idx, s);
}

static bool bind_i64(sqlite3_stmt* st, int idx, int64_t v) {
  return sqlite3_bind_int64(st, idx, (sqlite3_int64)v) == SQLITE_OK;
}

static bool step_row(sqlite3_stmt* st) {
  const int rc = sqlite3_step(st);
  return rc == SQLITE_ROW;
}

static bool step_done(sqlite3_stmt* st) {
  const int rc = sqlite3_step(st);
  return rc == SQLITE_DONE;
}
#endif

}  // namespace

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
  const int64_t now = unix_ms_now();
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
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && bind_text(st, 1, row.node_id);
  ok = ok && bind_text_or_null(st, 2, row.model);
  ok = ok && bind_text_or_null(st, 3, row.fw_git_sha);
  ok = ok && bind_text_or_null(st, 4, row.caps_sha256);
  ok = ok && bind_text_or_null(st, 5, row.manifest_json);
  ok = ok && bind_text_or_null(st, 6, row.tags_json);
  ok = ok && bind_text_or_null(st, 7, row.tools_json);
  ok = ok && bind_text_or_null(st, 8, row.hardware_presence_json);
  ok = ok && bind_text_or_null(st, 9, row.health_json);
  ok = ok && bind_i64(st, 10, hello);
  ok = ok && bind_i64(st, 11, hb);

  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
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
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, node_id);
  bool found = false;
  if (ok && step_row(st)) {
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
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
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
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_i64(st, 1, (int64_t)max_rows);
  while (ok && step_row(st)) {
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
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
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
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : unix_ms_now();

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
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.msg_id);
  ok = ok && bind_i64(st, 2, ts);
  ok = ok && bind_text(st, 3, row.type);
  ok = ok && bind_text_or_null(st, 4, row.from_id);
  ok = ok && bind_text_or_null(st, 5, row.to_id);
  ok = ok && bind_text(st, 6, row.envelope_json);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (ok && out_deduped) {
    *out_deduped = (sqlite3_changes(db_) == 0);
  }
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
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = bind_text(st, 1, msg_id);
  bool found = false;
  if (ok && step_row(st)) {
    found = true;
    const int64_t v = sqlite3_column_int64(st, 0);
    *out_processed = (v != 0);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
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
  const int64_t ts = processed_utc_ms > 0 ? processed_utc_ms : unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "UPDATE edge_inbox_messages SET processed=1, processed_utc_ms=? WHERE msg_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_i64(st, 1, ts);
  ok = ok && bind_text(st, 2, msg_id);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
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
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : unix_ms_now();

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_outbox_messages(node_id, ts_utc_ms, envelope_json) VALUES(?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.node_id);
  ok = ok && bind_i64(st, 2, ts);
  ok = ok && bind_text(st, 3, row.envelope_json);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
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
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, node_id);
  ok = ok && bind_i64(st, 2, after_outbox_id);
  ok = ok && bind_i64(st, 3, (int64_t)max_rows);
  while (ok && step_row(st)) {
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
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::upsert_edge_task(const EdgeTaskRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.task_id.empty() || row.step_id.empty()) {
    if (out_error) *out_error = "upsert_edge_task: task_id/step_id is empty";
    return false;
  }
  if (row.node_id.empty() || row.idempotency_key.empty() || row.mode.empty() || row.payload_json.empty() || row.state.empty()) {
    if (out_error) *out_error = "upsert_edge_task: missing required fields";
    return false;
  }
  const int64_t now = unix_ms_now();
  const int64_t created = row.created_utc_ms > 0 ? row.created_utc_ms : now;
  const int64_t updated = row.updated_utc_ms > 0 ? row.updated_utc_ms : now;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_tasks(
  task_id, step_id, node_id, idempotency_key, trace_id, mode, tool_name, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(task_id, step_id) DO UPDATE SET
  node_id = excluded.node_id,
  idempotency_key = excluded.idempotency_key,
  trace_id = CASE WHEN excluded.trace_id IS NOT NULL AND excluded.trace_id <> '' THEN excluded.trace_id ELSE edge_tasks.trace_id END,
  mode = CASE WHEN excluded.mode IS NOT NULL AND excluded.mode <> '' THEN excluded.mode ELSE edge_tasks.mode END,
  tool_name = CASE WHEN excluded.tool_name IS NOT NULL AND excluded.tool_name <> '' THEN excluded.tool_name ELSE edge_tasks.tool_name END,
  deadline_utc_ms = CASE WHEN excluded.deadline_utc_ms > 0 THEN excluded.deadline_utc_ms ELSE edge_tasks.deadline_utc_ms END,
  payload_json = CASE WHEN excluded.payload_json IS NOT NULL AND excluded.payload_json <> '' THEN excluded.payload_json ELSE edge_tasks.payload_json END,
  state = excluded.state,
  created_utc_ms = edge_tasks.created_utc_ms,
  updated_utc_ms = excluded.updated_utc_ms,
  result_json = CASE WHEN excluded.result_json IS NOT NULL AND excluded.result_json <> '' THEN excluded.result_json ELSE edge_tasks.result_json END,
  error = CASE WHEN excluded.error IS NOT NULL AND excluded.error <> '' THEN excluded.error ELSE edge_tasks.error END;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }

  bool ok = true;
  ok = ok && bind_text(st, 1, row.task_id);
  ok = ok && bind_text(st, 2, row.step_id);
  ok = ok && bind_text(st, 3, row.node_id);
  ok = ok && bind_text(st, 4, row.idempotency_key);
  ok = ok && bind_text_or_null(st, 5, row.trace_id);
  ok = ok && bind_text(st, 6, row.mode);
  ok = ok && bind_text_or_null(st, 7, row.tool_name);
  ok = ok && bind_i64(st, 8, row.deadline_utc_ms);
  ok = ok && bind_text(st, 9, row.payload_json);
  ok = ok && bind_text(st, 10, row.state);
  ok = ok && bind_i64(st, 11, created);
  ok = ok && bind_i64(st, 12, updated);
  ok = ok && bind_text_or_null(st, 13, row.result_json);
  ok = ok && bind_text_or_null(st, 14, row.error);

  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_edge_task(const std::string& task_id, const std::string& step_id, EdgeTaskRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeTaskRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)task_id;
  (void)step_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (task_id.empty() || step_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
FROM edge_tasks
WHERE task_id=? AND step_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, task_id);
  ok = ok && bind_text(st, 2, step_id);
  bool found = false;
  if (ok && step_row(st)) {
    found = true;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* m = sqlite3_column_text(st, 5);
    const unsigned char* tool = sqlite3_column_text(st, 6);
    const unsigned char* p = sqlite3_column_text(st, 8);
    const unsigned char* stt = sqlite3_column_text(st, 9);
    const unsigned char* r = sqlite3_column_text(st, 12);
    const unsigned char* e = sqlite3_column_text(st, 13);
    out_row->task_id = t ? (const char*)t : "";
    out_row->step_id = s ? (const char*)s : "";
    out_row->node_id = n ? (const char*)n : "";
    out_row->idempotency_key = k ? (const char*)k : "";
    out_row->trace_id = trc ? (const char*)trc : "";
    out_row->mode = m ? (const char*)m : "";
    out_row->tool_name = tool ? (const char*)tool : "";
    out_row->deadline_utc_ms = sqlite3_column_int64(st, 7);
    out_row->payload_json = p ? (const char*)p : "";
    out_row->state = stt ? (const char*)stt : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 10);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 11);
    out_row->result_json = r ? (const char*)r : "";
    out_row->error = e ? (const char*)e : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::get_edge_task_by_node_idempotency(
  const std::string& node_id,
  const std::string& idempotency_key,
  EdgeTaskRow* out_row,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeTaskRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  (void)idempotency_key;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (node_id.empty() || idempotency_key.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
FROM edge_tasks
WHERE node_id=? AND idempotency_key=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, node_id);
  ok = ok && bind_text(st, 2, idempotency_key);
  bool found = false;
  if (ok && step_row(st)) {
    found = true;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* m = sqlite3_column_text(st, 5);
    const unsigned char* tool = sqlite3_column_text(st, 6);
    const unsigned char* p = sqlite3_column_text(st, 8);
    const unsigned char* stt = sqlite3_column_text(st, 9);
    const unsigned char* r = sqlite3_column_text(st, 12);
    const unsigned char* e = sqlite3_column_text(st, 13);
    out_row->task_id = t ? (const char*)t : "";
    out_row->step_id = s ? (const char*)s : "";
    out_row->node_id = n ? (const char*)n : "";
    out_row->idempotency_key = k ? (const char*)k : "";
    out_row->trace_id = trc ? (const char*)trc : "";
    out_row->mode = m ? (const char*)m : "";
    out_row->tool_name = tool ? (const char*)tool : "";
    out_row->deadline_utc_ms = sqlite3_column_int64(st, 7);
    out_row->payload_json = p ? (const char*)p : "";
    out_row->state = stt ? (const char*)stt : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 10);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 11);
    out_row->result_json = r ? (const char*)r : "";
    out_row->error = e ? (const char*)e : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::list_edge_tasks_by_state(
  const std::string& state,
  size_t max_rows,
  std::vector<EdgeTaskRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)state;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (state.empty()) {
    if (out_error) *out_error = "list_edge_tasks_by_state: state is empty";
    return false;
  }
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
FROM edge_tasks
WHERE state=?
ORDER BY updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, state);
  ok = ok && bind_i64(st, 2, (int64_t)max_rows);
  while (ok && step_row(st)) {
    EdgeTaskRow row;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* m = sqlite3_column_text(st, 5);
    const unsigned char* tool = sqlite3_column_text(st, 6);
    const unsigned char* p = sqlite3_column_text(st, 8);
    const unsigned char* stt = sqlite3_column_text(st, 9);
    const unsigned char* r = sqlite3_column_text(st, 12);
    const unsigned char* e = sqlite3_column_text(st, 13);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.trace_id = trc ? (const char*)trc : "";
    row.mode = m ? (const char*)m : "";
    row.tool_name = tool ? (const char*)tool : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 7);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 10);
    row.updated_utc_ms = sqlite3_column_int64(st, 11);
    row.result_json = r ? (const char*)r : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::list_edge_tasks_by_trace_id(
  const std::string& trace_id,
  size_t max_rows,
  std::vector<EdgeTaskRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)trace_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (trace_id.empty()) {
    if (out_error) *out_error = "list_edge_tasks_by_trace_id: trace_id is empty";
    return false;
  }
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
FROM edge_tasks
WHERE trace_id=?
ORDER BY updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, trace_id);
  ok = ok && bind_i64(st, 2, (int64_t)max_rows);
  while (ok && step_row(st)) {
    EdgeTaskRow row;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* m = sqlite3_column_text(st, 5);
    const unsigned char* tool = sqlite3_column_text(st, 6);
    const unsigned char* p = sqlite3_column_text(st, 8);
    const unsigned char* stt = sqlite3_column_text(st, 9);
    const unsigned char* r = sqlite3_column_text(st, 12);
    const unsigned char* e = sqlite3_column_text(st, 13);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.trace_id = trc ? (const char*)trc : "";
    row.mode = m ? (const char*)m : "";
    row.tool_name = tool ? (const char*)tool : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 7);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 10);
    row.updated_utc_ms = sqlite3_column_int64(st, 11);
    row.result_json = r ? (const char*)r : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::list_edge_tasks_expired_deadline(
  int64_t now_utc_ms,
  size_t max_rows,
  std::vector<EdgeTaskRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)now_utc_ms;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  const int64_t now = now_utc_ms > 0 ? now_utc_ms : unix_ms_now();
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT task_id, step_id, node_id, idempotency_key, COALESCE(trace_id,''), mode, COALESCE(tool_name,''), deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
FROM edge_tasks
WHERE (state='QUEUED' OR state='RUNNING')
  AND deadline_utc_ms > 0
  AND deadline_utc_ms < ?
ORDER BY deadline_utc_ms ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_i64(st, 1, now);
  ok = ok && bind_i64(st, 2, (int64_t)max_rows);
  while (ok && step_row(st)) {
    EdgeTaskRow row;
    const unsigned char* t = sqlite3_column_text(st, 0);
    const unsigned char* s = sqlite3_column_text(st, 1);
    const unsigned char* n = sqlite3_column_text(st, 2);
    const unsigned char* k = sqlite3_column_text(st, 3);
    const unsigned char* trc = sqlite3_column_text(st, 4);
    const unsigned char* m = sqlite3_column_text(st, 5);
    const unsigned char* tool = sqlite3_column_text(st, 6);
    const unsigned char* p = sqlite3_column_text(st, 8);
    const unsigned char* stt = sqlite3_column_text(st, 9);
    const unsigned char* r = sqlite3_column_text(st, 12);
    const unsigned char* e = sqlite3_column_text(st, 13);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.trace_id = trc ? (const char*)trc : "";
    row.mode = m ? (const char*)m : "";
    row.tool_name = tool ? (const char*)tool : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 7);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 10);
    row.updated_utc_ms = sqlite3_column_int64(st, 11);
    row.result_json = r ? (const char*)r : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::get_edge_tool_rate_state(
  const std::string& node_id,
  const std::string& tool_name,
  EdgeToolRateStateRow* out_row,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeToolRateStateRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)node_id;
  (void)tool_name;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (node_id.empty() || tool_name.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT node_id, tool_name, window_start_utc_ms, window_count, last_call_utc_ms
FROM edge_tool_rate_state
WHERE node_id=? AND tool_name=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, node_id);
  ok = ok && bind_text(st, 2, tool_name);
  bool found = false;
  if (ok && step_row(st)) {
    found = true;
    const unsigned char* n = sqlite3_column_text(st, 0);
    const unsigned char* t = sqlite3_column_text(st, 1);
    out_row->node_id = n ? (const char*)n : "";
    out_row->tool_name = t ? (const char*)t : "";
    out_row->window_start_utc_ms = sqlite3_column_int64(st, 2);
    out_row->window_count = sqlite3_column_int(st, 3);
    out_row->last_call_utc_ms = sqlite3_column_int64(st, 4);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::upsert_edge_tool_rate_state(const EdgeToolRateStateRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.node_id.empty() || row.tool_name.empty()) {
    if (out_error) *out_error = "upsert_edge_tool_rate_state: node_id/tool_name empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_tool_rate_state(node_id, tool_name, window_start_utc_ms, window_count, last_call_utc_ms)
VALUES(?,?,?,?,?)
ON CONFLICT(node_id, tool_name) DO UPDATE SET
  window_start_utc_ms = excluded.window_start_utc_ms,
  window_count = excluded.window_count,
  last_call_utc_ms = excluded.last_call_utc_ms;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.node_id);
  ok = ok && bind_text(st, 2, row.tool_name);
  ok = ok && bind_i64(st, 3, row.window_start_utc_ms);
  ok = ok && (sqlite3_bind_int(st, 4, row.window_count) == SQLITE_OK);
  ok = ok && bind_i64(st, 5, row.last_call_utc_ms);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::upsert_edge_rule(const EdgeRuleRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.rule_id.empty() || row.event_type.empty() || row.action_json.empty()) {
    if (out_error) *out_error = "upsert_edge_rule: missing rule_id/event_type/action_json";
    return false;
  }
  const int64_t now = unix_ms_now();
  const int64_t created = row.created_utc_ms > 0 ? row.created_utc_ms : now;
  const int64_t updated = row.updated_utc_ms > 0 ? row.updated_utc_ms : now;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_rules(
  rule_id, enabled, event_type, min_confidence, cooldown_ms, last_fired_utc_ms, action_json, created_utc_ms, updated_utc_ms
) VALUES(?,?,?,?,?,?,?,?,?)
ON CONFLICT(rule_id) DO UPDATE SET
  enabled = excluded.enabled,
  event_type = excluded.event_type,
  min_confidence = excluded.min_confidence,
  cooldown_ms = excluded.cooldown_ms,
  last_fired_utc_ms = excluded.last_fired_utc_ms,
  action_json = excluded.action_json,
  updated_utc_ms = excluded.updated_utc_ms;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.rule_id);
  ok = ok && (sqlite3_bind_int(st, 2, row.enabled ? 1 : 0) == SQLITE_OK);
  ok = ok && bind_text(st, 3, row.event_type);
  ok = ok && (sqlite3_bind_double(st, 4, row.min_confidence) == SQLITE_OK);
  ok = ok && (sqlite3_bind_int(st, 5, row.cooldown_ms) == SQLITE_OK);
  ok = ok && bind_i64(st, 6, row.last_fired_utc_ms);
  ok = ok && bind_text(st, 7, row.action_json);
  ok = ok && bind_i64(st, 8, created);
  ok = ok && bind_i64(st, 9, updated);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::get_edge_rule(const std::string& rule_id, EdgeRuleRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeRuleRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)rule_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (rule_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT rule_id, enabled, event_type, min_confidence, cooldown_ms, last_fired_utc_ms, action_json, created_utc_ms, updated_utc_ms
FROM edge_rules
WHERE rule_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, rule_id);
  bool found = false;
  if (ok && step_row(st)) {
    found = true;
    const unsigned char* rid = sqlite3_column_text(st, 0);
    const unsigned char* et = sqlite3_column_text(st, 2);
    const unsigned char* aj = sqlite3_column_text(st, 6);
    out_row->rule_id = rid ? (const char*)rid : "";
    out_row->enabled = sqlite3_column_int(st, 1) != 0;
    out_row->event_type = et ? (const char*)et : "";
    out_row->min_confidence = sqlite3_column_double(st, 3);
    out_row->cooldown_ms = sqlite3_column_int(st, 4);
    out_row->last_fired_utc_ms = sqlite3_column_int64(st, 5);
    out_row->action_json = aj ? (const char*)aj : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 7);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 8);
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err(db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::delete_edge_rule(const std::string& rule_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)rule_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (rule_id.empty()) {
    if (out_error) *out_error = "delete_edge_rule: rule_id empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "DELETE FROM edge_rules WHERE rule_id=?;";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, rule_id);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_edge_rules(size_t max_rows, std::vector<EdgeRuleRow>* out_rows_desc, std::string* out_error) {
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
SELECT rule_id, enabled, event_type, min_confidence, cooldown_ms, last_fired_utc_ms, action_json, created_utc_ms, updated_utc_ms
FROM edge_rules
ORDER BY updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_i64(st, 1, (int64_t)max_rows);
  while (ok && step_row(st)) {
    EdgeRuleRow row;
    const unsigned char* rid = sqlite3_column_text(st, 0);
    const unsigned char* et = sqlite3_column_text(st, 2);
    const unsigned char* aj = sqlite3_column_text(st, 6);
    row.rule_id = rid ? (const char*)rid : "";
    row.enabled = sqlite3_column_int(st, 1) != 0;
    row.event_type = et ? (const char*)et : "";
    row.min_confidence = sqlite3_column_double(st, 3);
    row.cooldown_ms = sqlite3_column_int(st, 4);
    row.last_fired_utc_ms = sqlite3_column_int64(st, 5);
    row.action_json = aj ? (const char*)aj : "";
    row.created_utc_ms = sqlite3_column_int64(st, 7);
    row.updated_utc_ms = sqlite3_column_int64(st, 8);
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  return ok;
#endif
}

bool AgentDb::create_edge_workflow(
  const EdgeWorkflowRow& wf,
  const std::vector<EdgeWorkflowStepRow>& steps,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  (void)steps;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty() || wf.status.empty() || wf.spec_json.empty()) {
    if (out_error) *out_error = "create_edge_workflow: missing workflow_id/status/spec_json";
    return false;
  }
  if (steps.empty()) {
    if (out_error) *out_error = "create_edge_workflow: steps empty";
    return false;
  }
  const int64_t now = wf.updated_utc_ms > 0 ? wf.updated_utc_ms : unix_ms_now();
  const int64_t created = wf.created_utc_ms > 0 ? wf.created_utc_ms : now;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  char* err = nullptr;
  if (sqlite3_exec((sqlite3*)db_, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, &err) != SQLITE_OK) {
    if (out_error) *out_error = err ? err : sqlite_err((sqlite3*)db_);
    if (err) sqlite3_free(err);
    return false;
  }
  bool ok = true;
  {
    sqlite3_stmt* st = nullptr;
    const char* sql = R"SQL(
INSERT INTO edge_workflows(workflow_id, goal, status, priority, spec_json, created_utc_ms, updated_utc_ms, error)
VALUES(?,?,?,?,?,?,?,?);
)SQL";
    if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    } else {
      ok = ok && bind_text(st, 1, wf.workflow_id);
      ok = ok && bind_text_or_null(st, 2, wf.goal);
      ok = ok && bind_text(st, 3, wf.status);
      ok = ok && (sqlite3_bind_int(st, 4, wf.priority) == SQLITE_OK);
      ok = ok && bind_text(st, 5, wf.spec_json);
      ok = ok && bind_i64(st, 6, created);
      ok = ok && bind_i64(st, 7, now);
      ok = ok && bind_text_or_null(st, 8, wf.error);
      ok = ok && step_done(st);
      if (!ok && out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
      sqlite3_finalize(st);
    }
  }

  if (ok) {
    sqlite3_stmt* st = nullptr;
    const char* sql = R"SQL(
INSERT INTO edge_workflow_steps(
  workflow_id, step_id, kind, depends_on_json, target_json, payload_json, join_mode, deadline_utc_ms,
  attempt, max_attempts, next_ready_utc_ms, backoff_ms,
  state, created_utc_ms, updated_utc_ms, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);
)SQL";
    if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
      ok = false;
      if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    } else {
      for (const auto& s : steps) {
        if (s.workflow_id.empty() || s.step_id.empty() || s.kind.empty() || s.depends_on_json.empty() || s.target_json.empty() ||
            s.payload_json.empty() || s.state.empty()) {
          ok = false;
          if (out_error && out_error->empty()) *out_error = "create_edge_workflow: invalid step row";
          break;
        }
        sqlite3_reset(st);
        sqlite3_clear_bindings(st);
        const int64_t sc = s.created_utc_ms > 0 ? s.created_utc_ms : created;
        const int64_t su = s.updated_utc_ms > 0 ? s.updated_utc_ms : now;
        ok = ok && bind_text(st, 1, s.workflow_id);
        ok = ok && bind_text(st, 2, s.step_id);
        ok = ok && bind_text(st, 3, s.kind);
        ok = ok && bind_text(st, 4, s.depends_on_json);
        ok = ok && bind_text(st, 5, s.target_json);
        ok = ok && bind_text(st, 6, s.payload_json);
        ok = ok && bind_text_or_null(st, 7, s.join_mode);
        ok = ok && bind_i64(st, 8, s.deadline_utc_ms);
        ok = ok && (sqlite3_bind_int(st, 9, s.attempt) == SQLITE_OK);
        ok = ok && (sqlite3_bind_int(st, 10, s.max_attempts) == SQLITE_OK);
        ok = ok && bind_i64(st, 11, s.next_ready_utc_ms);
        ok = ok && (sqlite3_bind_int(st, 12, s.backoff_ms) == SQLITE_OK);
        ok = ok && bind_text(st, 13, s.state);
        ok = ok && bind_i64(st, 14, sc);
        ok = ok && bind_i64(st, 15, su);
        ok = ok && bind_text_or_null(st, 16, s.error);
        ok = ok && step_done(st);
        if (!ok) {
          if (out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
          break;
        }
      }
      sqlite3_finalize(st);
    }
  }

  if (ok) {
    ok = sqlite3_exec((sqlite3*)db_, "COMMIT;", nullptr, nullptr, &err) == SQLITE_OK;
    if (!ok && out_error && out_error->empty()) *out_error = err ? err : sqlite_err((sqlite3*)db_);
  } else {
    (void)sqlite3_exec((sqlite3*)db_, "ROLLBACK;", nullptr, nullptr, nullptr);
  }
  if (err) sqlite3_free(err);
  return ok;
#endif
}

bool AgentDb::get_edge_workflow(const std::string& workflow_id, EdgeWorkflowRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = EdgeWorkflowRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty() || !out_row) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT workflow_id, COALESCE(goal,''), status, priority, spec_json, created_utc_ms, updated_utc_ms, COALESCE(error,'')
FROM edge_workflows
WHERE workflow_id=?
LIMIT 1;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, workflow_id);
  bool found = false;
  if (ok && step_row(st)) {
    found = true;
    const unsigned char* wid = sqlite3_column_text(st, 0);
    const unsigned char* goal = sqlite3_column_text(st, 1);
    const unsigned char* stt = sqlite3_column_text(st, 2);
    const unsigned char* spec = sqlite3_column_text(st, 4);
    const unsigned char* errt = sqlite3_column_text(st, 7);
    out_row->workflow_id = wid ? (const char*)wid : "";
    out_row->goal = goal ? (const char*)goal : "";
    out_row->status = stt ? (const char*)stt : "";
    out_row->priority = sqlite3_column_int(st, 3);
    out_row->spec_json = spec ? (const char*)spec : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 5);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 6);
    out_row->error = errt ? (const char*)errt : "";
  }
  sqlite3_finalize(st);
  if (!ok) {
    if (out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  return found;
#endif
}

bool AgentDb::upsert_edge_workflow(const EdgeWorkflowRow& wf, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)wf;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (wf.workflow_id.empty() || wf.status.empty() || wf.spec_json.empty()) {
    if (out_error) *out_error = "upsert_edge_workflow: missing workflow_id/status/spec_json";
    return false;
  }
  const int64_t now = wf.updated_utc_ms > 0 ? wf.updated_utc_ms : unix_ms_now();
  const int64_t created = wf.created_utc_ms > 0 ? wf.created_utc_ms : now;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_workflows(workflow_id, goal, status, priority, spec_json, created_utc_ms, updated_utc_ms, error)
VALUES(?,?,?,?,?,?,?,?)
ON CONFLICT(workflow_id) DO UPDATE SET
  goal = excluded.goal,
  status = excluded.status,
  priority = excluded.priority,
  spec_json = excluded.spec_json,
  updated_utc_ms = excluded.updated_utc_ms,
  error = excluded.error;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, wf.workflow_id);
  ok = ok && bind_text_or_null(st, 2, wf.goal);
  ok = ok && bind_text(st, 3, wf.status);
  ok = ok && (sqlite3_bind_int(st, 4, wf.priority) == SQLITE_OK);
  ok = ok && bind_text(st, 5, wf.spec_json);
  ok = ok && bind_i64(st, 6, created);
  ok = ok && bind_i64(st, 7, now);
  ok = ok && bind_text_or_null(st, 8, wf.error);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::list_edge_workflows_by_status(
  const std::string& status,
  size_t max_rows,
  std::vector<EdgeWorkflowRow>* out_rows_desc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_desc) out_rows_desc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)status;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows_desc) return false;
  if (status.empty()) {
    if (out_error) *out_error = "list_edge_workflows_by_status: status empty";
    return false;
  }
  if (max_rows == 0) max_rows = 64;
  if (max_rows > 512) max_rows = 512;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT workflow_id, COALESCE(goal,''), status, priority, spec_json, created_utc_ms, updated_utc_ms, COALESCE(error,'')
FROM edge_workflows
WHERE status=?
ORDER BY priority DESC, updated_utc_ms DESC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, status);
  ok = ok && bind_i64(st, 2, (int64_t)max_rows);
  while (ok && step_row(st)) {
    EdgeWorkflowRow row;
    const unsigned char* wid = sqlite3_column_text(st, 0);
    const unsigned char* goal = sqlite3_column_text(st, 1);
    const unsigned char* stt = sqlite3_column_text(st, 2);
    const unsigned char* spec = sqlite3_column_text(st, 4);
    const unsigned char* errt = sqlite3_column_text(st, 7);
    row.workflow_id = wid ? (const char*)wid : "";
    row.goal = goal ? (const char*)goal : "";
    row.status = stt ? (const char*)stt : "";
    row.priority = sqlite3_column_int(st, 3);
    row.spec_json = spec ? (const char*)spec : "";
    row.created_utc_ms = sqlite3_column_int64(st, 5);
    row.updated_utc_ms = sqlite3_column_int64(st, 6);
    row.error = errt ? (const char*)errt : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
  return ok;
#endif
}

bool AgentDb::list_edge_workflow_steps(const std::string& workflow_id, std::vector<EdgeWorkflowStepRow>* out_rows, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_rows) out_rows->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_edge_workflow_steps: workflow_id empty";
    return false;
  }
  if (!out_rows) return false;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT step_id, kind, depends_on_json, target_json, payload_json, COALESCE(join_mode,''), deadline_utc_ms,
  attempt, max_attempts, next_ready_utc_ms, backoff_ms,
  state, created_utc_ms, updated_utc_ms, COALESCE(error,'')
FROM edge_workflow_steps
WHERE workflow_id=?
ORDER BY step_id ASC;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, workflow_id);
  while (ok && step_row(st)) {
    EdgeWorkflowStepRow row;
    row.workflow_id = workflow_id;
    const unsigned char* sid = sqlite3_column_text(st, 0);
    const unsigned char* kind = sqlite3_column_text(st, 1);
    const unsigned char* deps = sqlite3_column_text(st, 2);
    const unsigned char* tgt = sqlite3_column_text(st, 3);
    const unsigned char* pay = sqlite3_column_text(st, 4);
    const unsigned char* jm = sqlite3_column_text(st, 5);
    const unsigned char* stt = sqlite3_column_text(st, 11);
    const unsigned char* errt = sqlite3_column_text(st, 14);
    row.step_id = sid ? (const char*)sid : "";
    row.kind = kind ? (const char*)kind : "";
    row.depends_on_json = deps ? (const char*)deps : "[]";
    row.target_json = tgt ? (const char*)tgt : "{}";
    row.payload_json = pay ? (const char*)pay : "{}";
    row.join_mode = jm ? (const char*)jm : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 6);
    row.attempt = sqlite3_column_int(st, 7);
    row.max_attempts = sqlite3_column_int(st, 8);
    row.next_ready_utc_ms = sqlite3_column_int64(st, 9);
    row.backoff_ms = sqlite3_column_int(st, 10);
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 12);
    row.updated_utc_ms = sqlite3_column_int64(st, 13);
    row.error = errt ? (const char*)errt : "";
    out_rows->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
  return ok;
#endif
}

bool AgentDb::upsert_edge_workflow_step(const EdgeWorkflowStepRow& step, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)step;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (step.workflow_id.empty() || step.step_id.empty() || step.kind.empty() || step.depends_on_json.empty() || step.target_json.empty() ||
      step.payload_json.empty() || step.state.empty()) {
    if (out_error) *out_error = "upsert_edge_workflow_step: missing required fields";
    return false;
  }
  const int64_t now = step.updated_utc_ms > 0 ? step.updated_utc_ms : unix_ms_now();
  const int64_t created = step.created_utc_ms > 0 ? step.created_utc_ms : now;
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
INSERT INTO edge_workflow_steps(
  workflow_id, step_id, kind, depends_on_json, target_json, payload_json, join_mode, deadline_utc_ms,
  attempt, max_attempts, next_ready_utc_ms, backoff_ms,
  state, created_utc_ms, updated_utc_ms, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(workflow_id, step_id) DO UPDATE SET
  state = excluded.state,
  attempt = excluded.attempt,
  max_attempts = excluded.max_attempts,
  next_ready_utc_ms = excluded.next_ready_utc_ms,
  backoff_ms = excluded.backoff_ms,
  updated_utc_ms = excluded.updated_utc_ms,
  error = excluded.error;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, step.workflow_id);
  ok = ok && bind_text(st, 2, step.step_id);
  ok = ok && bind_text(st, 3, step.kind);
  ok = ok && bind_text(st, 4, step.depends_on_json);
  ok = ok && bind_text(st, 5, step.target_json);
  ok = ok && bind_text(st, 6, step.payload_json);
  ok = ok && bind_text_or_null(st, 7, step.join_mode);
  ok = ok && bind_i64(st, 8, step.deadline_utc_ms);
  ok = ok && (sqlite3_bind_int(st, 9, step.attempt) == SQLITE_OK);
  ok = ok && (sqlite3_bind_int(st, 10, step.max_attempts) == SQLITE_OK);
  ok = ok && bind_i64(st, 11, step.next_ready_utc_ms);
  ok = ok && (sqlite3_bind_int(st, 12, step.backoff_ms) == SQLITE_OK);
  ok = ok && bind_text(st, 13, step.state);
  ok = ok && bind_i64(st, 14, created);
  ok = ok && bind_i64(st, 15, now);
  ok = ok && bind_text_or_null(st, 16, step.error);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
  sqlite3_finalize(st);
  return ok;
#endif
}

bool AgentDb::insert_edge_workflow_event(const EdgeWorkflowEventRow& row, int64_t* out_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_id) *out_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.workflow_id.empty() || row.type.empty() || row.data_json.empty()) {
    if (out_error) *out_error = "insert_edge_workflow_event: missing required fields";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_workflow_events(workflow_id, ts_utc_ms, type, data_json) VALUES(?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.workflow_id);
  ok = ok && bind_i64(st, 2, ts);
  ok = ok && bind_text(st, 3, row.type);
  ok = ok && bind_text(st, 4, row.data_json);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

bool AgentDb::list_edge_workflow_events(
  const std::string& workflow_id,
  int64_t after_id,
  size_t max_rows,
  std::vector<EdgeWorkflowEventRow>* out_rows_asc,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows_asc) out_rows_asc->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)workflow_id;
  (void)after_id;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (workflow_id.empty()) {
    if (out_error) *out_error = "list_edge_workflow_events: workflow_id empty";
    return false;
  }
  if (!out_rows_asc) return false;
  if (after_id < 0) after_id = 0;
  if (max_rows == 0) max_rows = 128;
  if (max_rows > 1024) max_rows = 1024;

  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = R"SQL(
SELECT id, ts_utc_ms, type, data_json
FROM edge_workflow_events
WHERE workflow_id=? AND id>?
ORDER BY id ASC
LIMIT ?;
)SQL";
  if (sqlite3_prepare_v2((sqlite3*)db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err((sqlite3*)db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, workflow_id);
  ok = ok && bind_i64(st, 2, after_id);
  ok = ok && bind_i64(st, 3, (int64_t)max_rows);
  while (ok && step_row(st)) {
    EdgeWorkflowEventRow row;
    row.workflow_id = workflow_id;
    row.id = sqlite3_column_int64(st, 0);
    row.ts_utc_ms = sqlite3_column_int64(st, 1);
    const unsigned char* t = sqlite3_column_text(st, 2);
    const unsigned char* d = sqlite3_column_text(st, 3);
    row.type = t ? (const char*)t : "";
    row.data_json = d ? (const char*)d : "{}";
    out_rows_asc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err((sqlite3*)db_);
  return ok;
#endif
}

bool AgentDb::insert_edge_task_event(const EdgeTaskEventRow& row, int64_t* out_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_id) *out_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.task_id.empty() || row.step_id.empty() || row.state.empty() || row.data_json.empty()) {
    if (out_error) *out_error = "insert_edge_task_event: missing required fields";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_task_events(task_id, step_id, ts_utc_ms, state, data_json) VALUES(?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.task_id);
  ok = ok && bind_text(st, 2, row.step_id);
  ok = ok && bind_i64(st, 3, ts);
  ok = ok && bind_text(st, 4, row.state);
  ok = ok && bind_text(st, 5, row.data_json);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

bool AgentDb::insert_edge_sensor_event(const EdgeSensorEventRow& row, int64_t* out_id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_id) *out_id = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.node_id.empty() || row.event_type.empty() || row.data_json.empty()) {
    if (out_error) *out_error = "insert_edge_sensor_event: missing required fields";
    return false;
  }
  const int64_t ts = row.ts_utc_ms > 0 ? row.ts_utc_ms : unix_ms_now();
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  sqlite3_stmt* st = nullptr;
  const char* sql = "INSERT INTO edge_sensor_events(node_id, event_type, ts_utc_ms, confidence, data_json) VALUES(?,?,?,?,?);";
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite_err(db_);
    return false;
  }
  bool ok = true;
  ok = ok && bind_text(st, 1, row.node_id);
  ok = ok && bind_text(st, 2, row.event_type);
  ok = ok && bind_i64(st, 3, ts);
  if (sqlite3_bind_double(st, 4, row.confidence) != SQLITE_OK) ok = false;
  ok = ok && bind_text(st, 5, row.data_json);
  ok = ok && step_done(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
  sqlite3_finalize(st);
  if (!ok) return false;
  if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
  return true;
#endif
}

}  // namespace agentd
