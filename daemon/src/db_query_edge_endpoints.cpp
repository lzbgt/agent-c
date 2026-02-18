#include "db_query_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
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

struct TimeWindow {
  bool has_since = false;
  bool has_until = false;
  int64_t since = 0;
  int64_t until = 0;
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

static bool parse_i64_param(const std::optional<std::string>& v, int64_t* out) {
  if (!out) return false;
  if (!v.has_value()) return false;
  try {
    *out = (int64_t)std::stoll(*v);
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_bool_param(const std::optional<std::string>& v, bool def) {
  if (!v.has_value()) return def;
  return string_to_bool(*v);
}

static TimeWindow parse_time_window(const HttpRequest& req) {
  TimeWindow win;
  int64_t tmp = 0;
  if (parse_i64_param(query_get(req.query, "since_unix_ms"), &tmp)) {
    win.has_since = true;
    win.since = tmp;
  }
  if (parse_i64_param(query_get(req.query, "until_unix_ms"), &tmp)) {
    win.has_until = true;
    win.until = tmp;
  }
  return win;
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

#if defined(AGENT_HAVE_SQLITE3)
static void append_time_filter(const TimeWindow& win, const char* column, std::string* sql, std::vector<int64_t>* binds) {
  if (!sql || !binds || !column) return;
  if (win.has_since) {
    *sql += " AND ";
    *sql += column;
    *sql += " >= ?";
    binds->push_back(win.since);
  }
  if (win.has_until) {
    *sql += " AND ";
    *sql += column;
    *sql += " <= ?";
    binds->push_back(win.until);
  }
}

static bool bind_i64_list(sqlite3_stmt* st, const std::vector<int64_t>& binds, int start_index) {
  if (!st) return false;
  int idx = start_index;
  for (const auto& v : binds) {
    if (sqlite3_bind_int64(st, idx++, (sqlite3_int64)v) != SQLITE_OK) return false;
  }
  return true;
}

static bool query_status_counts(
  sqlite3* db,
  const std::string& table,
  const char* time_col,
  const TimeWindow& win,
  Json::Value* out_counts,
  int64_t* out_total,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_counts) *out_counts = Json::Value(Json::objectValue);
  if (out_total) *out_total = 0;
  if (!db || table.empty() || !time_col) return false;

  std::string sql = "SELECT status, COUNT(*) FROM " + table + " WHERE 1=1";
  std::vector<int64_t> binds;
  append_time_filter(win, time_col, &sql, &binds);
  sql += " GROUP BY status;";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    return false;
  }
  if (!bind_i64_list(st, binds, 1)) {
    sqlite3_finalize(st);
    if (out_error) *out_error = "failed to bind time window";
    return false;
  }

  int64_t total = 0;
  Json::Value counts(Json::objectValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* status = sqlite3_column_text(st, 0);
    const int64_t count = sqlite3_column_int64(st, 1);
    const std::string key = status ? (const char*)status : "";
    counts[key] = (Json::Int64)count;
    total += count;
  }
  sqlite3_finalize(st);
  if (out_counts) *out_counts = counts;
  if (out_total) *out_total = total;
  return true;
}

static bool query_terminal_stats(
  sqlite3* db,
  const std::string& table,
  const char* created_col,
  const char* updated_col,
  const TimeWindow& win,
  Json::Value* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = Json::Value(Json::objectValue);
  if (!db || table.empty() || !created_col || !updated_col) return false;

  std::string sql =
    "SELECT COUNT(*), AVG(" + std::string(updated_col) + " - " + created_col + "), "
    "MIN(" + std::string(updated_col) + " - " + created_col + "), "
    "MAX(" + std::string(updated_col) + " - " + created_col + ") "
    "FROM " + table + " WHERE status IN ('done','error','cancelled')";
  std::vector<int64_t> binds;
  append_time_filter(win, updated_col, &sql, &binds);
  sql += ";";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    return false;
  }
  if (!bind_i64_list(st, binds, 1)) {
    sqlite3_finalize(st);
    if (out_error) *out_error = "failed to bind time window";
    return false;
  }
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    if (out_error) *out_error = "no rows";
    return false;
  }

  Json::Value stats(Json::objectValue);
  stats["count"] = (Json::Int64)sqlite3_column_int64(st, 0);
  stats["avg_ms"] = stmt_to_row(st, 1);
  stats["min_ms"] = stmt_to_row(st, 2);
  stats["max_ms"] = stmt_to_row(st, 3);
  sqlite3_finalize(st);
  if (out_stats) *out_stats = stats;
  return true;
}

static bool query_error_count(
  sqlite3* db,
  const std::string& table,
  const char* updated_col,
  const TimeWindow& win,
  int64_t* out_count,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_count) *out_count = 0;
  if (!db || table.empty() || !updated_col) return false;

  std::string sql = "SELECT COUNT(*) FROM " + table + " WHERE status='error'";
  std::vector<int64_t> binds;
  append_time_filter(win, updated_col, &sql, &binds);
  sql += ";";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    return false;
  }
  if (!bind_i64_list(st, binds, 1)) {
    sqlite3_finalize(st);
    if (out_error) *out_error = "failed to bind time window";
    return false;
  }
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    if (out_error) *out_error = "no rows";
    return false;
  }
  if (out_count) *out_count = (int64_t)sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return true;
}

static bool query_edge_node_stats(
  sqlite3* db,
  const TimeWindow& win,
  int64_t active_within_ms,
  Json::Value* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = Json::Value(Json::objectValue);
  if (!db) return false;

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM edge_nodes;", -1, &st, nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    return false;
  }
  int64_t total = 0;
  if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);

  Json::Value stats(Json::objectValue);
  stats["total"] = (Json::Int64)total;

  if (win.has_since || win.has_until) {
    std::string sql = "SELECT COUNT(*) FROM edge_nodes WHERE last_heartbeat_utc_ms > 0";
    std::vector<int64_t> binds;
    append_time_filter(win, "last_heartbeat_utc_ms", &sql, &binds);
    sql += ";";
    st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      if (out_error) *out_error = sqlite3_errmsg(db);
      return false;
    }
    if (!bind_i64_list(st, binds, 1)) {
      sqlite3_finalize(st);
      if (out_error) *out_error = "failed to bind time window";
      return false;
    }
    int64_t window_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) window_count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    stats["window_count"] = (Json::Int64)window_count;
  }

  if (active_within_ms > 0) {
    const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    const int64_t threshold = now_ms - active_within_ms;
    st = nullptr;
    if (sqlite3_prepare_v2(
          db,
          "SELECT COUNT(*) FROM edge_nodes WHERE last_heartbeat_utc_ms >= ?;",
          -1,
          &st,
          nullptr) != SQLITE_OK) {
      if (out_error) *out_error = sqlite3_errmsg(db);
      return false;
    }
    (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)threshold);
    int64_t active_count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) active_count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    stats["active_within_ms"] = (Json::Int64)active_within_ms;
    stats["active_count"] = (Json::Int64)active_count;
  }

  st = nullptr;
  if (sqlite3_prepare_v2(
        db,
        "SELECT MIN(last_heartbeat_utc_ms), MAX(last_heartbeat_utc_ms) FROM edge_nodes WHERE last_heartbeat_utc_ms > 0;",
        -1,
        &st,
        nullptr) != SQLITE_OK) {
    if (out_error) *out_error = sqlite3_errmsg(db);
    return false;
  }
  if (sqlite3_step(st) == SQLITE_ROW) {
    stats["last_heartbeat_min"] = stmt_to_row(st, 0);
    stats["last_heartbeat_max"] = stmt_to_row(st, 1);
  }
  sqlite3_finalize(st);

  if (out_stats) *out_stats = stats;
  return true;
}

static bool build_edge_task_stats(
  sqlite3* db,
  const TimeWindow& win,
  Json::Value* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = Json::Value(Json::objectValue);
  if (!db) {
    if (out_error) *out_error = "db is null";
    return false;
  }

  bool ok = true;
  Json::Value stats(Json::objectValue);
  Json::Value counts(Json::objectValue);
  int64_t total = 0;
  std::string err;

  if (!query_status_counts(db, "edge_tasks", "updated_utc_ms", win, &counts, &total, &err)) {
    stats["error"] = err.empty() ? "failed to query edge task counts" : err;
    if (out_error && out_error->empty()) *out_error = stats["error"].asString();
    ok = false;
  } else {
    stats["counts"] = counts;
    stats["total"] = (Json::Int64)total;
  }

  Json::Value terminal(Json::objectValue);
  err.clear();
  if (!query_terminal_stats(db, "edge_tasks", "created_utc_ms", "updated_utc_ms", win, &terminal, &err)) {
    stats["terminal_error"] = err.empty() ? "failed to query terminal stats" : err;
    if (out_error && out_error->empty()) *out_error = stats["terminal_error"].asString();
    ok = false;
  } else {
    stats["terminal"] = terminal;
  }

  int64_t error_count = 0;
  err.clear();
  if (!query_error_count(db, "edge_tasks", "updated_utc_ms", win, &error_count, &err)) {
    stats["error_count_error"] = err.empty() ? "failed to query error count" : err;
    if (out_error && out_error->empty()) *out_error = stats["error_count_error"].asString();
    ok = false;
  } else {
    stats["error_count"] = (Json::Int64)error_count;
    const int64_t terminal_count = terminal.isMember("count") ? terminal["count"].asInt64() : 0;
    if (terminal_count > 0) {
      stats["error_rate"] = (double)error_count / (double)terminal_count;
    } else {
      stats["error_rate"] = Json::Value(Json::nullValue);
    }
  }

  if (out_stats) *out_stats = stats;
  return ok;
}

static bool build_edge_node_stats(
  sqlite3* db,
  const TimeWindow& win,
  int64_t active_within_ms,
  Json::Value* out_stats,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_stats) *out_stats = Json::Value(Json::objectValue);
  if (!db) {
    if (out_error) *out_error = "db is null";
    return false;
  }
  Json::Value stats(Json::objectValue);
  std::string err;
  if (!query_edge_node_stats(db, win, active_within_ms, &stats, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to query edge node stats" : err;
    return false;
  }
  if (out_stats) *out_stats = stats;
  return true;
}

static int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string json_scalar_to_string(const Json::Value& v) {
  if (v.isString()) return v.asString();
  if (v.isBool()) return v.asBool() ? "true" : "false";
  if (v.isInt64()) return std::to_string(v.asInt64());
  if (v.isUInt64()) return std::to_string(v.asUInt64());
  if (v.isInt()) return std::to_string(v.asInt());
  if (v.isUInt()) return std::to_string(v.asUInt());
  if (v.isDouble()) return std::to_string(v.asDouble());
  if (v.isNull()) return "";
  return json_stringify(v);
}

static std::string csv_escape(const std::string& v) {
  if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
  std::string out = "\"";
  out.reserve(v.size() + 2);
  for (char c : v) {
    if (c == '"') out += "\"\"";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static void append_csv_row(
  std::string* out,
  const std::string& section,
  const std::string& metric,
  const std::string& key,
  const std::string& value
) {
  if (!out) return;
  out->append(csv_escape(section));
  out->push_back(',');
  out->append(csv_escape(metric));
  out->push_back(',');
  out->append(csv_escape(key));
  out->push_back(',');
  out->append(csv_escape(value));
  out->push_back('\n');
}

static void append_edge_task_csv(const Json::Value& stats, std::string* out) {
  if (!out) return;
  if (stats.isObject() && stats.isMember("error")) {
    append_csv_row(out, "edge_tasks", "error", "", json_scalar_to_string(stats["error"]));
  }
  const Json::Value counts = stats.get("counts", Json::Value(Json::objectValue));
  if (counts.isObject()) {
    const auto names = counts.getMemberNames();
    for (const auto& name : names) {
      append_csv_row(out, "edge_tasks", "status_count", name, json_scalar_to_string(counts[name]));
    }
  }
  if (stats.isMember("total")) {
    append_csv_row(out, "edge_tasks", "total", "", json_scalar_to_string(stats["total"]));
  }
  const Json::Value terminal = stats.get("terminal", Json::Value(Json::objectValue));
  if (terminal.isObject()) {
    if (terminal.isMember("count")) {
      append_csv_row(out, "edge_tasks", "terminal_count", "", json_scalar_to_string(terminal["count"]));
    }
    if (terminal.isMember("avg_ms")) {
      append_csv_row(out, "edge_tasks", "terminal_avg_ms", "", json_scalar_to_string(terminal["avg_ms"]));
    }
    if (terminal.isMember("min_ms")) {
      append_csv_row(out, "edge_tasks", "terminal_min_ms", "", json_scalar_to_string(terminal["min_ms"]));
    }
    if (terminal.isMember("max_ms")) {
      append_csv_row(out, "edge_tasks", "terminal_max_ms", "", json_scalar_to_string(terminal["max_ms"]));
    }
  }
  if (stats.isMember("error_count")) {
    append_csv_row(out, "edge_tasks", "error_count", "", json_scalar_to_string(stats["error_count"]));
  }
  if (stats.isMember("error_rate")) {
    append_csv_row(out, "edge_tasks", "error_rate", "", json_scalar_to_string(stats["error_rate"]));
  }
  if (stats.isMember("terminal_error")) {
    append_csv_row(out, "edge_tasks", "terminal_error", "", json_scalar_to_string(stats["terminal_error"]));
  }
  if (stats.isMember("error_count_error")) {
    append_csv_row(out, "edge_tasks", "error_count_error", "", json_scalar_to_string(stats["error_count_error"]));
  }
}

static void append_edge_node_csv(const Json::Value& stats, std::string* out) {
  if (!out) return;
  if (stats.isObject() && stats.isMember("error")) {
    append_csv_row(out, "edge_nodes", "error", "", json_scalar_to_string(stats["error"]));
  }
  const std::vector<std::string> keys = {
    "total",
    "window_count",
    "active_within_ms",
    "active_count",
    "last_heartbeat_min",
    "last_heartbeat_max",
  };
  for (const auto& key : keys) {
    if (stats.isMember(key)) {
      append_csv_row(out, "edge_nodes", key, "", json_scalar_to_string(stats[key]));
    }
  }
}
#endif

}  // namespace

void handle_db_edge_workflows_endpoint(
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

  const bool include_spec = parse_bool_param(query_get(req.query, "include_spec"), false);
  std::optional<std::string> status = query_get(req.query, "status");
  if (status && status->empty()) status.reset();

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
  std::string sql =
    "SELECT workflow_id, goal, status, priority, spec_json, created_utc_ms, updated_utc_ms, error "
    "FROM edge_workflows";
  if (status) sql += " WHERE status=?";
  sql += " ORDER BY updated_utc_ms DESC LIMIT ? OFFSET ?;";

  if (sqlite3_prepare_v2(h.db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  int bi = 1;
  if (status) {
    (void)sqlite3_bind_text(st, bi++, status->c_str(), (int)status->size(), SQLITE_TRANSIENT);
  }
  (void)sqlite3_bind_int64(st, bi++, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, bi++, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value r(Json::objectValue);
    r["workflow_id"] = stmt_to_row(st, 0);
    r["goal"] = stmt_to_row(st, 1);
    r["status"] = stmt_to_row(st, 2);
    r["priority"] = stmt_to_row(st, 3);
    const Json::Value spec_json_v = stmt_to_row(st, 4);
    if (include_spec) {
      r["spec_json"] = spec_json_v;
      if (spec_json_v.isString() && !spec_json_v.asString().empty()) {
        Json::Value parsed;
        std::string perr;
        if (json_parse_any(spec_json_v.asString(), &parsed, &perr)) {
          r["spec"] = parsed;
        }
      }
    }
    r["created_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 5);
    r["updated_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 6);
    r["error"] = stmt_to_row(st, 7);
    rows.append(r);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["status"] = status ? Json::Value(*status) : Json::Value(Json::nullValue);
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["include_spec"] = include_spec;
  out["count"] = (Json::UInt64)rows.size();
  out["edge_workflows"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_edge_workflow_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto workflow_id = query_get(req.query, "workflow_id");
  if (!workflow_id || workflow_id->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing workflow_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  const bool include_steps = parse_bool_param(query_get(req.query, "include_steps"), false);
  const bool include_events = parse_bool_param(query_get(req.query, "include_events"), false);

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
    "SELECT workflow_id, goal, status, priority, spec_json, created_utc_ms, updated_utc_ms, error "
    "FROM edge_workflows WHERE workflow_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, workflow_id->c_str(), (int)workflow_id->size(), SQLITE_TRANSIENT);
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    Json::Value nf(Json::objectValue);
    nf["ok"] = false;
    nf["rpc_status"] = 404;
    nf["error"] = "edge workflow not found";
    resp->status = 404;
    resp->body = json_stringify(nf);
    return;
  }

  Json::Value row(Json::objectValue);
  row["workflow_id"] = stmt_to_row(st, 0);
  row["goal"] = stmt_to_row(st, 1);
  row["status"] = stmt_to_row(st, 2);
  row["priority"] = stmt_to_row(st, 3);
  const Json::Value spec_json_v = stmt_to_row(st, 4);
  row["spec_json"] = spec_json_v;
  if (spec_json_v.isString() && !spec_json_v.asString().empty()) {
    Json::Value parsed;
    std::string perr;
    if (json_parse_any(spec_json_v.asString(), &parsed, &perr)) {
      row["spec"] = parsed;
    }
  }
  row["created_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 5);
  row["updated_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 6);
  row["error"] = stmt_to_row(st, 7);
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["edge_workflow"] = row;

  const int kMaxRows = 2000;

  if (include_steps) {
    Json::Value steps(Json::arrayValue);
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(
          h.db,
          "SELECT workflow_id, step_id, kind, depends_on_json, target_json, payload_json, join_mode, deadline_utc_ms, "
          "state, created_utc_ms, updated_utc_ms, error, attempt, max_attempts, next_ready_utc_ms, backoff_ms "
          "FROM edge_workflow_steps WHERE workflow_id=? ORDER BY updated_utc_ms DESC LIMIT ?;",
          -1,
          &st2,
          nullptr) == SQLITE_OK && st2) {
      (void)sqlite3_bind_text(st2, 1, workflow_id->c_str(), (int)workflow_id->size(), SQLITE_TRANSIENT);
      (void)sqlite3_bind_int(st2, 2, kMaxRows);
      while (sqlite3_step(st2) == SQLITE_ROW) {
        Json::Value s(Json::objectValue);
        int col = 0;
        s["workflow_id"] = stmt_to_row(st2, col++);
        s["step_id"] = stmt_to_row(st2, col++);
        s["kind"] = stmt_to_row(st2, col++);
        const Json::Value depends_json_v = stmt_to_row(st2, col++);
        s["depends_on_json"] = depends_json_v;
        if (depends_json_v.isString() && !depends_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(depends_json_v.asString(), &parsed, &perr)) {
            s["depends_on"] = parsed;
          }
        }
        const Json::Value target_json_v = stmt_to_row(st2, col++);
        s["target_json"] = target_json_v;
        if (target_json_v.isString() && !target_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(target_json_v.asString(), &parsed, &perr)) {
            s["target"] = parsed;
          }
        }
        const Json::Value payload_json_v = stmt_to_row(st2, col++);
        s["payload_json"] = payload_json_v;
        if (payload_json_v.isString() && !payload_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(payload_json_v.asString(), &parsed, &perr)) {
            s["payload"] = parsed;
          }
        }
        s["join_mode"] = stmt_to_row(st2, col++);
        s["deadline_utc_ms"] = stmt_to_row(st2, col++);
        s["state"] = stmt_to_row(st2, col++);
        s["created_utc_ms"] = (Json::Int64)sqlite3_column_int64(st2, col++);
        s["updated_utc_ms"] = (Json::Int64)sqlite3_column_int64(st2, col++);
        s["error"] = stmt_to_row(st2, col++);
        s["attempt"] = (Json::Int64)sqlite3_column_int64(st2, col++);
        s["max_attempts"] = (Json::Int64)sqlite3_column_int64(st2, col++);
        s["next_ready_utc_ms"] = (Json::Int64)sqlite3_column_int64(st2, col++);
        s["backoff_ms"] = (Json::Int64)sqlite3_column_int64(st2, col++);
        steps.append(s);
      }
      sqlite3_finalize(st2);
    } else if (st2) {
      sqlite3_finalize(st2);
    }
    out["steps"] = steps;
  }

  if (include_events) {
    Json::Value events(Json::arrayValue);
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(
          h.db,
          "SELECT id, workflow_id, ts_utc_ms, type, data_json "
          "FROM edge_workflow_events WHERE workflow_id=? ORDER BY id LIMIT ?;",
          -1,
          &st2,
          nullptr) == SQLITE_OK && st2) {
      (void)sqlite3_bind_text(st2, 1, workflow_id->c_str(), (int)workflow_id->size(), SQLITE_TRANSIENT);
      (void)sqlite3_bind_int(st2, 2, kMaxRows);
      while (sqlite3_step(st2) == SQLITE_ROW) {
        Json::Value e(Json::objectValue);
        e["id"] = (Json::Int64)sqlite3_column_int64(st2, 0);
        e["workflow_id"] = stmt_to_row(st2, 1);
        e["ts_utc_ms"] = (Json::Int64)sqlite3_column_int64(st2, 2);
        e["type"] = stmt_to_row(st2, 3);
        const Json::Value data_json_v = stmt_to_row(st2, 4);
        e["data_json"] = data_json_v;
        if (data_json_v.isString() && !data_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(data_json_v.asString(), &parsed, &perr)) {
            e["data"] = parsed;
          }
        }
        events.append(e);
      }
      sqlite3_finalize(st2);
    } else if (st2) {
      sqlite3_finalize(st2);
    }
    out["events"] = events;
  }

  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_edge_workflow_steps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto workflow_id = query_get(req.query, "workflow_id");
  if (!workflow_id || workflow_id->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing workflow_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  uint64_t limit = 50;
  uint64_t offset = 0;
  uint64_t tmp = 0;
  if (parse_u64_param(query_get(req.query, "limit"), &tmp)) limit = tmp;
  if (parse_u64_param(query_get(req.query, "offset"), &tmp)) offset = tmp;
  limit = clamp_u64(limit, 1, 200);
  offset = clamp_u64(offset, 0, 1000000);

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
    "SELECT workflow_id, step_id, kind, depends_on_json, target_json, payload_json, join_mode, deadline_utc_ms, "
    "state, created_utc_ms, updated_utc_ms, error, attempt, max_attempts, next_ready_utc_ms, backoff_ms "
    "FROM edge_workflow_steps WHERE workflow_id=? ORDER BY updated_utc_ms DESC LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, workflow_id->c_str(), (int)workflow_id->size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value s(Json::objectValue);
    int col = 0;
    s["workflow_id"] = stmt_to_row(st, col++);
    s["step_id"] = stmt_to_row(st, col++);
    s["kind"] = stmt_to_row(st, col++);
    const Json::Value depends_json_v = stmt_to_row(st, col++);
    s["depends_on_json"] = depends_json_v;
    if (depends_json_v.isString() && !depends_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_any(depends_json_v.asString(), &parsed, &perr)) {
        s["depends_on"] = parsed;
      }
    }
    const Json::Value target_json_v = stmt_to_row(st, col++);
    s["target_json"] = target_json_v;
    if (target_json_v.isString() && !target_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_any(target_json_v.asString(), &parsed, &perr)) {
        s["target"] = parsed;
      }
    }
    const Json::Value payload_json_v = stmt_to_row(st, col++);
    s["payload_json"] = payload_json_v;
    if (payload_json_v.isString() && !payload_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_any(payload_json_v.asString(), &parsed, &perr)) {
        s["payload"] = parsed;
      }
    }
    s["join_mode"] = stmt_to_row(st, col++);
    s["deadline_utc_ms"] = stmt_to_row(st, col++);
    s["state"] = stmt_to_row(st, col++);
    s["created_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, col++);
    s["updated_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, col++);
    s["error"] = stmt_to_row(st, col++);
    s["attempt"] = (Json::Int64)sqlite3_column_int64(st, col++);
    s["max_attempts"] = (Json::Int64)sqlite3_column_int64(st, col++);
    s["next_ready_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, col++);
    s["backoff_ms"] = (Json::Int64)sqlite3_column_int64(st, col++);
    rows.append(s);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["workflow_id"] = *workflow_id;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)rows.size();
  out["edge_workflow_steps"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_edge_workflow_events_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto workflow_id = query_get(req.query, "workflow_id");
  if (!workflow_id || workflow_id->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing workflow_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  uint64_t limit = 50;
  uint64_t offset = 0;
  uint64_t tmp = 0;
  if (parse_u64_param(query_get(req.query, "limit"), &tmp)) limit = tmp;
  if (parse_u64_param(query_get(req.query, "offset"), &tmp)) offset = tmp;
  limit = clamp_u64(limit, 1, 200);
  offset = clamp_u64(offset, 0, 1000000);

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
    "SELECT id, workflow_id, ts_utc_ms, type, data_json "
    "FROM edge_workflow_events WHERE workflow_id=? ORDER BY id LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, workflow_id->c_str(), (int)workflow_id->size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value e(Json::objectValue);
    e["id"] = (Json::Int64)sqlite3_column_int64(st, 0);
    e["workflow_id"] = stmt_to_row(st, 1);
    e["ts_utc_ms"] = (Json::Int64)sqlite3_column_int64(st, 2);
    e["type"] = stmt_to_row(st, 3);
    const Json::Value data_json_v = stmt_to_row(st, 4);
    e["data_json"] = data_json_v;
    if (data_json_v.isString() && !data_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_any(data_json_v.asString(), &parsed, &perr)) {
        e["data"] = parsed;
      }
    }
    rows.append(e);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["workflow_id"] = *workflow_id;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)rows.size();
  out["edge_workflow_events"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_workflow_analytics_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto scope_param = query_get(req.query, "scope");
  std::string scope = (scope_param && !scope_param->empty()) ? *scope_param : "all";
  if (scope != "all" && scope != "durable" && scope != "edge") {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid scope (expected all|durable|edge)\"}";
    return;
  }

  const TimeWindow win = parse_time_window(req);

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
  out["scope"] = scope;
  out["since_unix_ms"] = win.has_since ? Json::Value((Json::Int64)win.since) : Json::Value(Json::nullValue);
  out["until_unix_ms"] = win.has_until ? Json::Value((Json::Int64)win.until) : Json::Value(Json::nullValue);

  auto build_stats = [&](const std::string& table, const char* created_col, const char* updated_col) -> Json::Value {
    Json::Value stats(Json::objectValue);
    Json::Value counts(Json::objectValue);
    int64_t total = 0;
    std::string err;
    if (!query_status_counts(h.db, table, updated_col, win, &counts, &total, &err)) {
      stats["error"] = err.empty() ? "failed to query status counts" : err;
      return stats;
    }
    stats["counts"] = counts;
    stats["total"] = (Json::Int64)total;

    Json::Value terminal(Json::objectValue);
    err.clear();
    if (!query_terminal_stats(h.db, table, created_col, updated_col, win, &terminal, &err)) {
      stats["terminal_error"] = err.empty() ? "failed to query terminal stats" : err;
    } else {
      stats["terminal"] = terminal;
    }

    int64_t error_count = 0;
    err.clear();
    if (!query_error_count(h.db, table, updated_col, win, &error_count, &err)) {
      stats["error_count_error"] = err.empty() ? "failed to query error count" : err;
    } else {
      stats["error_count"] = (Json::Int64)error_count;
      const int64_t terminal_count = terminal.isMember("count") ? terminal["count"].asInt64() : 0;
      if (terminal_count > 0) {
        stats["error_rate"] = (double)error_count / (double)terminal_count;
      } else {
        stats["error_rate"] = Json::Value(Json::nullValue);
      }
    }
    return stats;
  };

  if (scope == "all" || scope == "durable") {
    out["durable"] = build_stats("workflows", "created_unix_ms", "updated_unix_ms");
  }
  if (scope == "all" || scope == "edge") {
    out["edge"] = build_stats("edge_workflows", "created_utc_ms", "updated_utc_ms");
  }

  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_edge_analytics_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  int64_t active_within_ms = 0;
  (void)parse_i64_param(query_get(req.query, "active_within_ms"), &active_within_ms);
  if (active_within_ms < 0) active_within_ms = 0;

  const TimeWindow win = parse_time_window(req);

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
  out["since_unix_ms"] = win.has_since ? Json::Value((Json::Int64)win.since) : Json::Value(Json::nullValue);
  out["until_unix_ms"] = win.has_until ? Json::Value((Json::Int64)win.until) : Json::Value(Json::nullValue);
  out["active_within_ms"] = active_within_ms > 0 ? Json::Value((Json::Int64)active_within_ms) : Json::Value(Json::nullValue);

  Json::Value task_stats(Json::objectValue);
  Json::Value node_stats(Json::objectValue);
  std::string err;
  (void)build_edge_task_stats(h.db, win, &task_stats, &err);
  out["edge_tasks"] = task_stats;
  err.clear();
  if (!build_edge_node_stats(h.db, win, active_within_ms, &node_stats, &err)) {
    node_stats["error"] = err.empty() ? "failed to query edge node stats" : err;
  }
  out["edge_nodes"] = node_stats;

  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_edge_analytics_export_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  if (!daemon_require_auth(cfg, req, resp)) return;

  const std::string format = query_get(req.query, "format").value_or("json");
  const std::string scope = query_get(req.query, "scope").value_or("all");
  if (format != "json" && format != "csv") {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"invalid format (use json or csv)"})";
    return;
  }
  if (scope != "all" && scope != "edge_tasks" && scope != "edge_nodes") {
    resp->status = 400;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = R"({"ok":false,"error":"invalid scope (use all, edge_tasks, or edge_nodes)"})";
    return;
  }

  int64_t active_within_ms = 0;
  (void)parse_i64_param(query_get(req.query, "active_within_ms"), &active_within_ms);
  if (active_within_ms < 0) active_within_ms = 0;
  const TimeWindow win = parse_time_window(req);

  DbHandle h;
  Json::Value o = open_db_or_error(db_or_null, &h, nullptr);
  if (!o.isObject() || !o.get("ok", false).asBool()) {
    const int st = o.isMember("rpc_status") && o["rpc_status"].isInt() ? o["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->headers["Content-Type"] = "application/json; charset=utf-8";
    resp->body = json_stringify(o);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  resp->body = R"({"ok":false,"error":"sqlite disabled"})";
  return;
#else
  Json::Value task_stats(Json::objectValue);
  Json::Value node_stats(Json::objectValue);
  std::string err;
  if (scope == "all" || scope == "edge_tasks") {
    (void)build_edge_task_stats(h.db, win, &task_stats, &err);
  }
  err.clear();
  if (scope == "all" || scope == "edge_nodes") {
    if (!build_edge_node_stats(h.db, win, active_within_ms, &node_stats, &err)) {
      node_stats["error"] = err.empty() ? "failed to query edge node stats" : err;
    }
  }

  const int64_t generated_ms = now_utc_ms();

  if (format == "csv") {
    resp->status = 200;
    resp->headers["Content-Type"] = "text/csv; charset=utf-8";
    resp->headers["Content-Disposition"] = "attachment; filename=\"edge_analytics_export.csv\"";
    std::string out;
    out.reserve(2048);
    out.append("section,metric,key,value\n");
    append_csv_row(&out, "meta", "generated_utc_ms", "", std::to_string(generated_ms));
    if (win.has_since) append_csv_row(&out, "meta", "since_unix_ms", "", std::to_string(win.since));
    if (win.has_until) append_csv_row(&out, "meta", "until_unix_ms", "", std::to_string(win.until));
    if (active_within_ms > 0) {
      append_csv_row(&out, "meta", "active_within_ms", "", std::to_string(active_within_ms));
    }
    if (scope == "all" || scope == "edge_tasks") {
      append_edge_task_csv(task_stats, &out);
    }
    if (scope == "all" || scope == "edge_nodes") {
      append_edge_node_csv(node_stats, &out);
    }
    resp->body = std::move(out);
    return;
  }

  resp->status = 200;
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["generated_utc_ms"] = (Json::Int64)generated_ms;
  out["since_unix_ms"] = win.has_since ? Json::Value((Json::Int64)win.since) : Json::Value(Json::nullValue);
  out["until_unix_ms"] = win.has_until ? Json::Value((Json::Int64)win.until) : Json::Value(Json::nullValue);
  out["active_within_ms"] = active_within_ms > 0 ? Json::Value((Json::Int64)active_within_ms) : Json::Value(Json::nullValue);
  out["scope"] = scope;
  if (scope == "all" || scope == "edge_tasks") out["edge_tasks"] = task_stats;
  if (scope == "all" || scope == "edge_nodes") out["edge_nodes"] = node_stats;
  resp->body = json_stringify(out);
  return;
#endif
}

}  // namespace agentd
