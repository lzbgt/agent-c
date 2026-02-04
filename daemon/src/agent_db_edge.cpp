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
  manifest_json = CASE WHEN excluded.manifest_json IS NOT NULL AND excluded.manifest_json <> '' THEN excluded.manifest_json ELSE edge_nodes.manifest_json END,
  tags_json = CASE WHEN excluded.tags_json IS NOT NULL AND excluded.tags_json <> '' THEN excluded.tags_json ELSE edge_nodes.tags_json END,
  tools_json = CASE WHEN excluded.tools_json IS NOT NULL AND excluded.tools_json <> '' THEN excluded.tools_json ELSE edge_nodes.tools_json END,
  hardware_presence_json = CASE WHEN excluded.hardware_presence_json IS NOT NULL AND excluded.hardware_presence_json <> '' THEN excluded.hardware_presence_json ELSE edge_nodes.hardware_presence_json END,
  health_json = CASE WHEN excluded.health_json IS NOT NULL AND excluded.health_json <> '' THEN excluded.health_json ELSE edge_nodes.health_json END,
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

bool AgentDb::insert_edge_inbox_message(const EdgeInboxMessageRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
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
INSERT INTO edge_inbox_messages(msg_id, ts_utc_ms, type, from_id, to_id, envelope_json)
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
  return ok;
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
  task_id, step_id, node_id, idempotency_key, mode, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)
ON CONFLICT(node_id, idempotency_key) DO UPDATE SET
  task_id = edge_tasks.task_id,
  step_id = edge_tasks.step_id,
  node_id = excluded.node_id,
  idempotency_key = excluded.idempotency_key,
  mode = edge_tasks.mode,
  deadline_utc_ms = CASE WHEN excluded.deadline_utc_ms > 0 THEN excluded.deadline_utc_ms ELSE edge_tasks.deadline_utc_ms END,
  payload_json = edge_tasks.payload_json,
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
  ok = ok && bind_text(st, 5, row.mode);
  ok = ok && bind_i64(st, 6, row.deadline_utc_ms);
  ok = ok && bind_text(st, 7, row.payload_json);
  ok = ok && bind_text(st, 8, row.state);
  ok = ok && bind_i64(st, 9, created);
  ok = ok && bind_i64(st, 10, updated);
  ok = ok && bind_text_or_null(st, 11, row.result_json);
  ok = ok && bind_text_or_null(st, 12, row.error);

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
SELECT task_id, step_id, node_id, idempotency_key, mode, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
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
    const unsigned char* m = sqlite3_column_text(st, 4);
    const unsigned char* p = sqlite3_column_text(st, 6);
    const unsigned char* stt = sqlite3_column_text(st, 7);
    const unsigned char* r = sqlite3_column_text(st, 10);
    const unsigned char* e = sqlite3_column_text(st, 11);
    out_row->task_id = t ? (const char*)t : "";
    out_row->step_id = s ? (const char*)s : "";
    out_row->node_id = n ? (const char*)n : "";
    out_row->idempotency_key = k ? (const char*)k : "";
    out_row->mode = m ? (const char*)m : "";
    out_row->deadline_utc_ms = sqlite3_column_int64(st, 5);
    out_row->payload_json = p ? (const char*)p : "";
    out_row->state = stt ? (const char*)stt : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 8);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 9);
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
SELECT task_id, step_id, node_id, idempotency_key, mode, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
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
    const unsigned char* m = sqlite3_column_text(st, 4);
    const unsigned char* p = sqlite3_column_text(st, 6);
    const unsigned char* stt = sqlite3_column_text(st, 7);
    const unsigned char* r = sqlite3_column_text(st, 10);
    const unsigned char* e = sqlite3_column_text(st, 11);
    out_row->task_id = t ? (const char*)t : "";
    out_row->step_id = s ? (const char*)s : "";
    out_row->node_id = n ? (const char*)n : "";
    out_row->idempotency_key = k ? (const char*)k : "";
    out_row->mode = m ? (const char*)m : "";
    out_row->deadline_utc_ms = sqlite3_column_int64(st, 5);
    out_row->payload_json = p ? (const char*)p : "";
    out_row->state = stt ? (const char*)stt : "";
    out_row->created_utc_ms = sqlite3_column_int64(st, 8);
    out_row->updated_utc_ms = sqlite3_column_int64(st, 9);
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
SELECT task_id, step_id, node_id, idempotency_key, mode, deadline_utc_ms, payload_json, state, created_utc_ms, updated_utc_ms, result_json, error
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
    const unsigned char* m = sqlite3_column_text(st, 4);
    const unsigned char* p = sqlite3_column_text(st, 6);
    const unsigned char* stt = sqlite3_column_text(st, 7);
    const unsigned char* r = sqlite3_column_text(st, 10);
    const unsigned char* e = sqlite3_column_text(st, 11);
    row.task_id = t ? (const char*)t : "";
    row.step_id = s ? (const char*)s : "";
    row.node_id = n ? (const char*)n : "";
    row.idempotency_key = k ? (const char*)k : "";
    row.mode = m ? (const char*)m : "";
    row.deadline_utc_ms = sqlite3_column_int64(st, 5);
    row.payload_json = p ? (const char*)p : "";
    row.state = stt ? (const char*)stt : "";
    row.created_utc_ms = sqlite3_column_int64(st, 8);
    row.updated_utc_ms = sqlite3_column_int64(st, 9);
    row.result_json = r ? (const char*)r : "";
    row.error = e ? (const char*)e : "";
    out_rows_desc->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  if (!ok && out_error && out_error->empty()) *out_error = sqlite_err(db_);
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

