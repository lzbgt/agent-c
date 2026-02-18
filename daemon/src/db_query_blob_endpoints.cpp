#include "db_query_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#if defined(AGENT_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace agentd {
namespace {

struct DbHandle {
#if defined(AGENT_HAVE_SQLITE3)
  sqlite3* db = nullptr;
#endif
  ~DbHandle() {
#if defined(AGENT_HAVE_SQLITE3)
    if (db) sqlite3_close(db);
#endif
  }
};

static bool parse_u64_param(const std::optional<std::string>& v, uint64_t* out) {
  if (!out) return false;
  if (!v.has_value()) return false;
  try {
    *out = (uint64_t)std::stoull(*v);
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_bool_param(const std::optional<std::string>& v, bool def) {
  if (!v.has_value()) return def;
  return string_to_bool(*v);
}

static Json::Value open_db_or_error(const AgentDb* db_or_null, DbHandle* out, std::string* out_path) {
  Json::Value o(Json::objectValue);
  if (!db_or_null || !db_or_null->is_open()) {
    o["ok"] = false;
    o["rpc_status"] = 404;
    o["error"] = "db disabled";
    return o;
  }
  if (out_path) *out_path = db_or_null->path();
#if !defined(AGENT_HAVE_SQLITE3)
  (void)out;
  o["ok"] = false;
  o["rpc_status"] = 500;
  o["error"] = "sqlite3 support not compiled (AGENT_HAVE_SQLITE3)";
  return o;
#else
  if (!out) {
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = "internal error";
    return o;
  }
  const std::string path = db_or_null->path();
  if (path.empty()) {
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = "db path is empty";
    return o;
  }
  sqlite3* db = nullptr;
  const int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = db ? sqlite3_errmsg(db) : "sqlite open failed";
    if (db) sqlite3_close(db);
    return o;
  }
  out->db = db;
  (void)sqlite3_busy_timeout(out->db, 2000);
  o["ok"] = true;
  return o;
#endif
}

#if defined(AGENT_HAVE_SQLITE3)
static Json::Value stmt_to_row(sqlite3_stmt* st, int col) {
  if (!st) return Json::Value(Json::nullValue);
  if (sqlite3_column_type(st, col) == SQLITE_NULL) return Json::Value(Json::nullValue);
  switch (sqlite3_column_type(st, col)) {
    case SQLITE_INTEGER:
      return Json::Value((Json::Int64)sqlite3_column_int64(st, col));
    case SQLITE_FLOAT:
      return Json::Value(sqlite3_column_double(st, col));
    case SQLITE_TEXT: {
      const unsigned char* txt = sqlite3_column_text(st, col);
      return Json::Value(txt ? (const char*)txt : "");
    }
    default:
      return Json::Value(Json::nullValue);
  }
}
#endif

static uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi) {
  return std::max<uint64_t>(lo, std::min<uint64_t>(hi, v));
}

}  // namespace

void handle_db_blobs_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  uint64_t limit = 50;
  uint64_t offset = 0;
  uint64_t tmp = 0;
  if (parse_u64_param(query_get(req.query, "limit"), &tmp)) limit = tmp;
  if (parse_u64_param(query_get(req.query, "offset"), &tmp)) offset = tmp;
  limit = clamp_u64(limit, 1, 200);
  offset = clamp_u64(offset, 0, 1000000);

  std::optional<std::string> tier = query_get(req.query, "tier");
  if (tier && tier->empty()) tier.reset();
  if (tier && *tier != "local" && *tier != "object" && *tier != "archive") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid tier (expected local|object|archive)";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  uint64_t min_ref_count = 0;
  bool have_min_ref = false;
  if (parse_u64_param(query_get(req.query, "min_ref_count"), &tmp)) {
    min_ref_count = tmp;
    have_min_ref = true;
  }

  uint64_t min_size_bytes = 0;
  bool have_min_size = false;
  if (parse_u64_param(query_get(req.query, "min_size_bytes"), &tmp)) {
    min_size_bytes = tmp;
    have_min_size = true;
  }

  uint64_t max_size_bytes = 0;
  bool have_max_size = false;
  if (parse_u64_param(query_get(req.query, "max_size_bytes"), &tmp)) {
    max_size_bytes = tmp;
    have_max_size = true;
  }

  std::string order = query_get(req.query, "order").value_or("created");
  if (order.empty()) order = "created";
  std::string order_col = "created_utc_ms";
  if (order == "created" || order == "created_utc_ms") {
    order_col = "created_utc_ms";
  } else if (order == "last_access" || order == "last_access_utc_ms") {
    order_col = "last_access_utc_ms";
  } else if (order == "size" || order == "size_bytes") {
    order_col = "size_bytes";
  } else if (order == "ref_count") {
    order_col = "ref_count";
  } else {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid order (created|last_access|size|ref_count)";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  std::string dir = query_get(req.query, "dir").value_or("desc");
  if (dir.empty()) dir = "desc";
  for (auto& c : dir) c = (char)std::tolower((unsigned char)c);
  if (dir != "asc" && dir != "desc") {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid dir (asc|desc)";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  DbHandle h;
  Json::Value o = open_db_or_error(db_or_null, &h, nullptr);
  if (!o.isObject() || !o.get("ok", false).asBool()) {
    const int st = o.isMember("rpc_status") && o["rpc_status"].isInt() ? o["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(o);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = R"({"ok":false,"error":"sqlite disabled"})";
  return;
#else
  std::string sql =
    "SELECT blob_id, size_bytes, mime, sha256_hex, created_utc_ms, last_access_utc_ms, ref_count, tier, location, etag, storage_class "
    "FROM blob_manifest";
  std::vector<std::string> where;
  if (tier) where.push_back("tier=?");
  if (have_min_ref) where.push_back("ref_count>=?");
  if (have_min_size) where.push_back("size_bytes>=?");
  if (have_max_size) where.push_back("size_bytes<=?");
  if (!where.empty()) {
    sql += " WHERE ";
    for (size_t i = 0; i < where.size(); i++) {
      if (i > 0) sql += " AND ";
      sql += where[i];
    }
  }
  sql += " ORDER BY " + order_col + " " + (dir == "asc" ? "ASC" : "DESC") + " LIMIT ? OFFSET ?;";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(h.db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  int bind_idx = 1;
  if (tier) {
    (void)sqlite3_bind_text(st, bind_idx++, tier->c_str(), (int)tier->size(), SQLITE_TRANSIENT);
  }
  if (have_min_ref) {
    (void)sqlite3_bind_int64(st, bind_idx++, (sqlite3_int64)min_ref_count);
  }
  if (have_min_size) {
    (void)sqlite3_bind_int64(st, bind_idx++, (sqlite3_int64)min_size_bytes);
  }
  if (have_max_size) {
    (void)sqlite3_bind_int64(st, bind_idx++, (sqlite3_int64)max_size_bytes);
  }
  (void)sqlite3_bind_int64(st, bind_idx++, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, bind_idx++, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value b(Json::objectValue);
    b["blob_id"] = stmt_to_row(st, 0);
    b["size_bytes"] = (Json::Int64)sqlite3_column_int64(st, 1);
    b["mime"] = stmt_to_row(st, 2);
    b["sha256_hex"] = stmt_to_row(st, 3);
    b["created_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 4);
    b["last_access_utc_ms"] = stmt_to_row(st, 5);
    b["ref_count"] = (Json::Int64)sqlite3_column_int64(st, 6);
    b["tier"] = stmt_to_row(st, 7);
    b["location"] = stmt_to_row(st, 8);
    b["etag"] = stmt_to_row(st, 9);
    b["storage_class"] = stmt_to_row(st, 10);
    rows.append(b);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)rows.size();
  out["order"] = order;
  out["dir"] = dir;
  out["tier"] = tier ? Json::Value(*tier) : Json::Value(Json::nullValue);
  out["min_ref_count"] = have_min_ref ? Json::Value((Json::UInt64)min_ref_count) : Json::Value(Json::nullValue);
  out["min_size_bytes"] = have_min_size ? Json::Value((Json::UInt64)min_size_bytes) : Json::Value(Json::nullValue);
  out["max_size_bytes"] = have_max_size ? Json::Value((Json::UInt64)max_size_bytes) : Json::Value(Json::nullValue);
  out["blobs"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_blob_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto blob_id = query_get(req.query, "blob_id");
  if (!blob_id || blob_id->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing blob_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  const bool include_artifacts = parse_bool_param(query_get(req.query, "include_artifacts"), false);
  uint64_t artifacts_limit = 50;
  uint64_t artifacts_offset = 0;
  uint64_t tmp = 0;
  if (parse_u64_param(query_get(req.query, "artifacts_limit"), &tmp)) artifacts_limit = tmp;
  if (parse_u64_param(query_get(req.query, "artifacts_offset"), &tmp)) artifacts_offset = tmp;
  artifacts_limit = clamp_u64(artifacts_limit, 1, 200);
  artifacts_offset = clamp_u64(artifacts_offset, 0, 1000000);

  DbHandle h;
  Json::Value o = open_db_or_error(db_or_null, &h, nullptr);
  if (!o.isObject() || !o.get("ok", false).asBool()) {
    const int st = o.isMember("rpc_status") && o["rpc_status"].isInt() ? o["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(o);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = R"({"ok":false,"error":"sqlite disabled"})";
  return;
#else
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT blob_id, size_bytes, mime, sha256_hex, created_utc_ms, last_access_utc_ms, ref_count, tier, location, etag, storage_class "
    "FROM blob_manifest WHERE blob_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, blob_id->c_str(), (int)blob_id->size(), SQLITE_TRANSIENT);
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    Json::Value nf(Json::objectValue);
    nf["ok"] = false;
    nf["rpc_status"] = 404;
    nf["error"] = "blob not found";
    resp->status = 404;
    resp->body = json_stringify(nf);
    return;
  }
  Json::Value blob(Json::objectValue);
  blob["blob_id"] = stmt_to_row(st, 0);
  blob["size_bytes"] = (Json::Int64)sqlite3_column_int64(st, 1);
  blob["mime"] = stmt_to_row(st, 2);
  blob["sha256_hex"] = stmt_to_row(st, 3);
  blob["created_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 4);
  blob["last_access_utc_ms"] = stmt_to_row(st, 5);
  blob["ref_count"] = (Json::Int64)sqlite3_column_int64(st, 6);
  blob["tier"] = stmt_to_row(st, 7);
  blob["location"] = stmt_to_row(st, 8);
  blob["etag"] = stmt_to_row(st, 9);
  blob["storage_class"] = stmt_to_row(st, 10);
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["blob_id"] = *blob_id;
  out["blob"] = blob;

  if (include_artifacts) {
    sqlite3_stmt* st2 = nullptr;
    const char* sql2 =
      "SELECT artifacts.id, artifacts.run_id, artifacts.session_id, artifacts.ts_unix_ms, artifacts.path, artifacts.kind, "
      "artifacts.mime, artifacts.title "
      "FROM artifact_blobs "
      "JOIN artifacts ON artifact_blobs.artifact_id = artifacts.id "
      "WHERE artifact_blobs.blob_id=? "
      "ORDER BY artifacts.ts_unix_ms DESC LIMIT ? OFFSET ?;";
    if (sqlite3_prepare_v2(h.db, sql2, -1, &st2, nullptr) != SQLITE_OK) {
      Json::Value err(Json::objectValue);
      err["ok"] = false;
      err["rpc_status"] = 500;
      err["error"] = sqlite3_errmsg(h.db);
      resp->status = 500;
      resp->body = json_stringify(err);
      return;
    }
    (void)sqlite3_bind_text(st2, 1, blob_id->c_str(), (int)blob_id->size(), SQLITE_TRANSIENT);
    (void)sqlite3_bind_int64(st2, 2, (sqlite3_int64)artifacts_limit);
    (void)sqlite3_bind_int64(st2, 3, (sqlite3_int64)artifacts_offset);

    Json::Value arts(Json::arrayValue);
    while (sqlite3_step(st2) == SQLITE_ROW) {
      Json::Value a(Json::objectValue);
      a["artifact_id"] = (Json::Int64)sqlite3_column_int64(st2, 0);
      a["run_id"] = (Json::Int64)sqlite3_column_int64(st2, 1);
      a["session_id"] = stmt_to_row(st2, 2);
      a["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st2, 3);
      a["path"] = stmt_to_row(st2, 4);
      a["kind"] = stmt_to_row(st2, 5);
      a["mime"] = stmt_to_row(st2, 6);
      a["title"] = stmt_to_row(st2, 7);
      arts.append(a);
    }
    sqlite3_finalize(st2);
    out["artifacts_limit"] = (Json::UInt64)artifacts_limit;
    out["artifacts_offset"] = (Json::UInt64)artifacts_offset;
    out["artifacts_count"] = (Json::UInt64)arts.size();
    out["artifacts"] = arts;
  }

  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_blob_analytics_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  uint64_t top_mime_limit = 10;
  uint64_t tmp = 0;
  if (parse_u64_param(query_get(req.query, "top_mime_limit"), &tmp)) top_mime_limit = tmp;
  top_mime_limit = clamp_u64(top_mime_limit, 0, 50);

  DbHandle h;
  Json::Value o = open_db_or_error(db_or_null, &h, nullptr);
  if (!o.isObject() || !o.get("ok", false).asBool()) {
    const int st = o.isMember("rpc_status") && o["rpc_status"].isInt() ? o["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(o);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = R"({"ok":false,"error":"sqlite disabled"})";
  return;
#else
  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["top_mime_limit"] = (Json::UInt64)top_mime_limit;

  {
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT COUNT(*), COALESCE(SUM(size_bytes),0), COALESCE(SUM(ref_count),0) FROM blob_manifest;";
    if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) == SQLITE_OK) {
      if (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value totals(Json::objectValue);
        totals["blob_count"] = (Json::Int64)sqlite3_column_int64(st, 0);
        totals["total_bytes"] = (Json::Int64)sqlite3_column_int64(st, 1);
        totals["total_refs"] = (Json::Int64)sqlite3_column_int64(st, 2);
        out["totals"] = totals;
      }
      sqlite3_finalize(st);
    }
  }

  {
    sqlite3_stmt* st = nullptr;
    const char* sql =
      "SELECT tier, COUNT(*), COALESCE(SUM(size_bytes),0), COALESCE(SUM(ref_count),0) "
      "FROM blob_manifest GROUP BY tier ORDER BY tier;";
    Json::Value rows(Json::arrayValue);
    if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value r(Json::objectValue);
        r["tier"] = stmt_to_row(st, 0);
        r["blob_count"] = (Json::Int64)sqlite3_column_int64(st, 1);
        r["total_bytes"] = (Json::Int64)sqlite3_column_int64(st, 2);
        r["total_refs"] = (Json::Int64)sqlite3_column_int64(st, 3);
        rows.append(r);
      }
      sqlite3_finalize(st);
    }
    out["by_tier"] = rows;
  }

  if (top_mime_limit > 0) {
    sqlite3_stmt* st = nullptr;
    const char* sql =
      "SELECT mime, COUNT(*), COALESCE(SUM(size_bytes),0) "
      "FROM blob_manifest WHERE mime IS NOT NULL AND mime != '' "
      "GROUP BY mime ORDER BY SUM(size_bytes) DESC LIMIT ?;";
    Json::Value rows(Json::arrayValue);
    if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) == SQLITE_OK) {
      (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)top_mime_limit);
      while (sqlite3_step(st) == SQLITE_ROW) {
        Json::Value r(Json::objectValue);
        r["mime"] = stmt_to_row(st, 0);
        r["blob_count"] = (Json::Int64)sqlite3_column_int64(st, 1);
        r["total_bytes"] = (Json::Int64)sqlite3_column_int64(st, 2);
        rows.append(r);
      }
      sqlite3_finalize(st);
    }
    out["top_mime"] = rows;
  }

  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

}  // namespace agentd
