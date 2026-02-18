#include "agent_db.h"
#include "agent_db_sqlite.h"

#include <string>

namespace agentd {

#if defined(AGENT_HAVE_SQLITE3)
static std::string stmt_text(sqlite3_stmt* st, int idx) {
  const unsigned char* txt = sqlite3_column_text(st, idx);
  return txt ? (const char*)txt : "";
}
#endif

bool AgentDb::get_blob_manifest(const std::string& blob_id, BlobManifestRow* out_row, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_row) *out_row = BlobManifestRow{};
#if !defined(AGENT_HAVE_SQLITE3)
  (void)blob_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (blob_id.empty()) {
    if (out_error) *out_error = "get_blob_manifest: blob_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const char* sql =
    "SELECT blob_id, size_bytes, mime, sha256_hex, created_utc_ms, last_access_utc_ms, ref_count, tier, location, etag, storage_class "
    "FROM blob_manifest WHERE blob_id=? LIMIT 1;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, blob_id);
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  if (!agent_db_step_row(st)) {
    sqlite3_finalize(st);
    return false;
  }
  if (out_row) {
    out_row->blob_id = stmt_text(st, 0);
    out_row->size_bytes = sqlite3_column_int64(st, 1);
    out_row->mime = stmt_text(st, 2);
    out_row->sha256_hex = stmt_text(st, 3);
    out_row->created_utc_ms = sqlite3_column_int64(st, 4);
    out_row->last_access_utc_ms = sqlite3_column_int64(st, 5);
    out_row->ref_count = sqlite3_column_int64(st, 6);
    out_row->tier = stmt_text(st, 7);
    out_row->location = stmt_text(st, 8);
    out_row->etag = stmt_text(st, 9);
    out_row->storage_class = stmt_text(st, 10);
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::insert_blob_manifest(const BlobManifestRow& row, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)row;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (row.blob_id.empty()) {
    if (out_error) *out_error = "insert_blob_manifest: blob_id is empty";
    return false;
  }
  if (row.sha256_hex.empty()) {
    if (out_error) *out_error = "insert_blob_manifest: sha256_hex is empty";
    return false;
  }
  if (row.location.empty()) {
    if (out_error) *out_error = "insert_blob_manifest: location is empty";
    return false;
  }
  if (row.tier.empty()) {
    if (out_error) *out_error = "insert_blob_manifest: tier is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }

  const char* sql =
    "INSERT INTO blob_manifest(blob_id, size_bytes, mime, sha256_hex, created_utc_ms, last_access_utc_ms, ref_count, tier, location, etag, storage_class) "
    "VALUES(?,?,?,?,?,?,?,?,?,?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }

  bool ok = true;
  ok = ok && agent_db_bind_text(st, 1, row.blob_id);
  ok = ok && agent_db_bind_i64(st, 2, row.size_bytes);
  ok = ok && (row.mime.empty() ? (sqlite3_bind_null(st, 3) == SQLITE_OK) : agent_db_bind_text(st, 3, row.mime));
  ok = ok && agent_db_bind_text(st, 4, row.sha256_hex);
  ok = ok && agent_db_bind_i64(st, 5, row.created_utc_ms);
  ok = ok && agent_db_bind_i64(st, 6, row.last_access_utc_ms);
  ok = ok && agent_db_bind_i64(st, 7, row.ref_count);
  ok = ok && agent_db_bind_text(st, 8, row.tier);
  ok = ok && agent_db_bind_text(st, 9, row.location);
  ok = ok && (row.etag.empty() ? (sqlite3_bind_null(st, 10) == SQLITE_OK) : agent_db_bind_text(st, 10, row.etag));
  ok = ok && (row.storage_class.empty() ? (sqlite3_bind_null(st, 11) == SQLITE_OK)
                                        : agent_db_bind_text(st, 11, row.storage_class));
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

bool AgentDb::update_blob_manifest_access(const std::string& blob_id, int64_t last_access_utc_ms, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)blob_id;
  (void)last_access_utc_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (blob_id.empty()) {
    if (out_error) *out_error = "update_blob_manifest_access: blob_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const char* sql = "UPDATE blob_manifest SET last_access_utc_ms=? WHERE blob_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = agent_db_bind_i64(st, 1, last_access_utc_ms) && agent_db_bind_text(st, 2, blob_id);
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

bool AgentDb::update_blob_manifest_location(
  const std::string& blob_id,
  const std::string& mime,
  const std::string& tier,
  const std::string& location,
  const std::string& etag,
  const std::string& storage_class,
  int64_t last_access_utc_ms,
  std::string* out_error
) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)blob_id;
  (void)mime;
  (void)tier;
  (void)location;
  (void)etag;
  (void)storage_class;
  (void)last_access_utc_ms;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (blob_id.empty()) {
    if (out_error) *out_error = "update_blob_manifest_location: blob_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const char* sql =
    "UPDATE blob_manifest SET mime=?, tier=?, location=?, etag=?, storage_class=?, last_access_utc_ms=? "
    "WHERE blob_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = true;
  ok = ok && (mime.empty() ? (sqlite3_bind_null(st, 1) == SQLITE_OK) : agent_db_bind_text(st, 1, mime));
  ok = ok && agent_db_bind_text(st, 2, tier);
  ok = ok && agent_db_bind_text(st, 3, location);
  ok = ok && (etag.empty() ? (sqlite3_bind_null(st, 4) == SQLITE_OK) : agent_db_bind_text(st, 4, etag));
  ok = ok && (storage_class.empty() ? (sqlite3_bind_null(st, 5) == SQLITE_OK) : agent_db_bind_text(st, 5, storage_class));
  ok = ok && agent_db_bind_i64(st, 6, last_access_utc_ms);
  ok = ok && agent_db_bind_text(st, 7, blob_id);
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

bool AgentDb::adjust_blob_ref_count(const std::string& blob_id, int64_t delta, int64_t* out_ref_count, std::string* out_error) {
  if (out_error) out_error->clear();
  if (out_ref_count) *out_ref_count = 0;
#if !defined(AGENT_HAVE_SQLITE3)
  (void)blob_id;
  (void)delta;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (blob_id.empty()) {
    if (out_error) *out_error = "adjust_blob_ref_count: blob_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const char* sql =
    "UPDATE blob_manifest "
    "SET ref_count = CASE WHEN ref_count + ? < 0 THEN 0 ELSE ref_count + ? END "
    "WHERE blob_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = agent_db_bind_i64(st, 1, delta) && agent_db_bind_i64(st, 2, delta) && agent_db_bind_text(st, 3, blob_id);
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
  if (sqlite3_changes(db_) == 0) {
    if (out_error) *out_error = "blob not found";
    sqlite3_finalize(st);
    return false;
  }
  sqlite3_finalize(st);

  const char* sel = "SELECT ref_count FROM blob_manifest WHERE blob_id=? LIMIT 1;";
  st = nullptr;
  if (sqlite3_prepare_v2(db_, sel, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  ok = agent_db_bind_text(st, 1, blob_id);
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  if (agent_db_step_row(st) && out_ref_count) {
    *out_ref_count = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::list_blob_gc_candidates(
  int64_t created_before_utc_ms,
  size_t max_rows,
  std::vector<BlobManifestRow>* out_rows,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_rows) out_rows->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)created_before_utc_ms;
  (void)max_rows;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (!out_rows) {
    if (out_error) *out_error = "list_blob_gc_candidates: missing output";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  if (max_rows == 0) max_rows = 1000;
  if (max_rows > 10000) max_rows = 10000;

  const char* sql =
    "SELECT blob_id, size_bytes, mime, sha256_hex, created_utc_ms, last_access_utc_ms, ref_count, tier, location, etag, storage_class "
    "FROM blob_manifest "
    "WHERE ref_count <= 0 AND created_utc_ms <= ? "
    "ORDER BY created_utc_ms ASC LIMIT ?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = agent_db_bind_i64(st, 1, created_before_utc_ms);
  ok = ok && sqlite3_bind_int64(st, 2, (sqlite3_int64)max_rows) == SQLITE_OK;
  if (!ok) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    sqlite3_finalize(st);
    return false;
  }
  while (agent_db_step_row(st)) {
    BlobManifestRow row;
    row.blob_id = stmt_text(st, 0);
    row.size_bytes = sqlite3_column_int64(st, 1);
    row.mime = stmt_text(st, 2);
    row.sha256_hex = stmt_text(st, 3);
    row.created_utc_ms = sqlite3_column_int64(st, 4);
    row.last_access_utc_ms = sqlite3_column_int64(st, 5);
    row.ref_count = sqlite3_column_int64(st, 6);
    row.tier = stmt_text(st, 7);
    row.location = stmt_text(st, 8);
    row.etag = stmt_text(st, 9);
    row.storage_class = stmt_text(st, 10);
    out_rows->push_back(std::move(row));
  }
  sqlite3_finalize(st);
  return true;
#endif
}

bool AgentDb::delete_blob_manifest(const std::string& blob_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)blob_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (blob_id.empty()) {
    if (out_error) *out_error = "delete_blob_manifest: blob_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const char* sql = "DELETE FROM blob_manifest WHERE blob_id=?;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = agent_db_bind_text(st, 1, blob_id);
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

bool AgentDb::attach_blob_to_artifact(int64_t artifact_id, const std::string& blob_id, std::string* out_error) {
  if (out_error) out_error->clear();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)artifact_id;
  (void)blob_id;
  if (out_error) *out_error = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return false;
#else
  if (artifact_id <= 0) {
    if (out_error) *out_error = "attach_blob_to_artifact: invalid artifact_id";
    return false;
  }
  if (blob_id.empty()) {
    if (out_error) *out_error = "attach_blob_to_artifact: blob_id is empty";
    return false;
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) {
    if (out_error) *out_error = "db is not open";
    return false;
  }
  const char* sql = "INSERT OR IGNORE INTO artifact_blobs(artifact_id, blob_id) VALUES(?,?);";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = agent_db_sqlite_err(db_);
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool ok = agent_db_bind_i64(st, 1, artifact_id) && agent_db_bind_text(st, 2, blob_id);
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
