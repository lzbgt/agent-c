#include "db_query_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

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

void handle_db_runs_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing session_id";
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

  const bool only_errors = parse_bool_param(query_get(req.query, "only_errors"), false);
  std::optional<std::string> stop_reason = query_get(req.query, "stop_reason");
  if ((!stop_reason || stop_reason->empty()) && query_get(req.query, "reason")) {
    stop_reason = query_get(req.query, "reason");
  }
  if (stop_reason && stop_reason->empty()) stop_reason.reset();

  DbHandle h;
  std::string path;
  Json::Value o = open_db_or_error(db_or_null, &h, &path);
  if (!o.isObject() || !o.get("ok", false).asBool()) {
    const int st = o.isMember("rpc_status") && o["rpc_status"].isInt() ? o["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(o);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  // unreachable (handled by open_db_or_error)
  resp->status = 500;
  resp->body = R"({"ok":false,"error":"sqlite disabled"})";
  return;
#else
  sqlite3_stmt* st = nullptr;
  std::string sql =
    "SELECT run_id, session_id, job_id, ts_unix_ms, tools, model, ok, error, stop_reason, steps_executed, tool_calls_total, tool_calls_by_tool_json, last_error_reason, "
    "(SELECT data_json FROM events e WHERE e.run_id = runs.run_id AND e.type='error' ORDER BY e.event_id DESC LIMIT 1) AS last_error_json "
    "FROM runs WHERE session_id=?";
  if (only_errors) {
    sql += " AND ok=0";
  }
  if (stop_reason) {
    sql += " AND stop_reason=?";
  }
  sql += " ORDER BY ts_unix_ms DESC LIMIT ? OFFSET ?;";

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
  (void)sqlite3_bind_text(st, bi++, sid->c_str(), (int)sid->size(), SQLITE_TRANSIENT);
  if (stop_reason) {
    (void)sqlite3_bind_text(st, bi++, stop_reason->c_str(), (int)stop_reason->size(), SQLITE_TRANSIENT);
  }
  (void)sqlite3_bind_int64(st, bi++, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, bi++, (sqlite3_int64)offset);

  Json::Value runs(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value r(Json::objectValue);
    r["run_id"] = (Json::Int64)sqlite3_column_int64(st, 0);
    r["session_id"] = stmt_to_row(st, 1);
    r["job_id"] = stmt_to_row(st, 2);
    r["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 3);
    r["tools"] = stmt_to_row(st, 4);
    r["model"] = stmt_to_row(st, 5);
    r["ok"] = sqlite3_column_int(st, 6) ? true : false;
    r["error"] = stmt_to_row(st, 7);
    r["stop_reason"] = stmt_to_row(st, 8);
    r["steps_executed"] = (Json::Int64)sqlite3_column_int64(st, 9);
    r["tool_calls_total"] = (Json::Int64)sqlite3_column_int64(st, 10);
    const Json::Value tool_calls_by_tool_json = stmt_to_row(st, 11);
    r["tool_calls_by_tool_json"] = tool_calls_by_tool_json;
    if (tool_calls_by_tool_json.isString() && !tool_calls_by_tool_json.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_object(tool_calls_by_tool_json.asString(), &parsed, &perr)) {
        r["tool_calls_by_tool"] = parsed;
      }
    }
    r["last_error_reason"] = stmt_to_row(st, 12);
    const Json::Value last_err_json = stmt_to_row(st, 13);
    r["last_error_json"] = last_err_json;
    if (last_err_json.isString() && !last_err_json.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_object(last_err_json.asString(), &parsed, &perr)) {
        r["last_error"] = parsed;
      }
    }
    runs.append(r);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["only_errors"] = only_errors;
  out["stop_reason"] = stop_reason ? Json::Value(*stop_reason) : Json::Value(Json::nullValue);
  out["count"] = (Json::UInt64)runs.size();
  out["runs"] = runs;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_run_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto run_id_s = query_get(req.query, "run_id");
  if (!run_id_s || run_id_s->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing run_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }
  int64_t run_id = 0;
  try {
    run_id = (int64_t)std::stoll(*run_id_s);
  } catch (...) {
    run_id = 0;
  }
  if (run_id <= 0) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid run_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  const bool include_events = parse_bool_param(query_get(req.query, "include_events"), false);
  const bool include_tools = parse_bool_param(query_get(req.query, "include_tools"), false);
  const bool include_artifacts = parse_bool_param(query_get(req.query, "include_artifacts"), false);
  const bool include_ui_actions = parse_bool_param(query_get(req.query, "include_ui_actions"), false);

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
  // Run row
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT run_id, session_id, job_id, ts_unix_ms, prompt, tools, model, base_url, stream_assistant, ok, "
    "stop_reason, steps_executed, tool_calls_total, tool_calls_by_tool_json, last_error_reason, "
    "error, http_status "
    "FROM runs WHERE run_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)run_id);
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    Json::Value nf(Json::objectValue);
    nf["ok"] = false;
    nf["rpc_status"] = 404;
    nf["error"] = "run not found";
    resp->status = 404;
    resp->body = json_stringify(nf);
    return;
  }
  Json::Value run(Json::objectValue);
  run["run_id"] = (Json::Int64)sqlite3_column_int64(st, 0);
  run["session_id"] = stmt_to_row(st, 1);
  run["job_id"] = stmt_to_row(st, 2);
  run["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 3);
  run["prompt"] = stmt_to_row(st, 4);
  run["tools"] = stmt_to_row(st, 5);
  run["model"] = stmt_to_row(st, 6);
  run["base_url"] = stmt_to_row(st, 7);
  run["stream_assistant"] = sqlite3_column_int(st, 8) ? true : false;
  run["ok"] = sqlite3_column_int(st, 9) ? true : false;
  run["stop_reason"] = stmt_to_row(st, 10);
  run["steps_executed"] = (Json::Int64)sqlite3_column_int64(st, 11);
  run["tool_calls_total"] = (Json::Int64)sqlite3_column_int64(st, 12);
  const Json::Value tool_calls_by_tool_json = stmt_to_row(st, 13);
  run["tool_calls_by_tool_json"] = tool_calls_by_tool_json;
  if (tool_calls_by_tool_json.isString() && !tool_calls_by_tool_json.asString().empty()) {
    Json::Value parsed;
    std::string perr;
    if (json_parse_object(tool_calls_by_tool_json.asString(), &parsed, &perr)) {
      run["tool_calls_by_tool"] = parsed;
    }
  }
  run["last_error_reason"] = stmt_to_row(st, 14);
  run["error"] = stmt_to_row(st, 15);
  run["http_status"] = stmt_to_row(st, 16);
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["run"] = run;

  const int kMaxRows = 2000;

  if (include_events) {
    Json::Value events(Json::arrayValue);
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(h.db, "SELECT event_id, ts_unix_ms, type, data_json FROM events WHERE run_id=? ORDER BY event_id LIMIT ?;",
                           -1, &st2, nullptr) == SQLITE_OK && st2) {
      (void)sqlite3_bind_int64(st2, 1, (sqlite3_int64)run_id);
      (void)sqlite3_bind_int(st2, 2, kMaxRows);
      while (sqlite3_step(st2) == SQLITE_ROW) {
        Json::Value e(Json::objectValue);
        e["event_id"] = (Json::Int64)sqlite3_column_int64(st2, 0);
        e["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st2, 1);
        e["type"] = stmt_to_row(st2, 2);
        const Json::Value data_json_v = stmt_to_row(st2, 3);
        e["data_json"] = data_json_v;
        if (data_json_v.isString() && !data_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_object(data_json_v.asString(), &parsed, &perr)) {
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

  if (include_tools) {
    Json::Value tools(Json::arrayValue);
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(h.db,
                           "SELECT id, tool_name, tool_call_id, arguments_json, result_text, result_for_prompt_text, result_truncated_for_prompt "
                           "FROM tool_records WHERE run_id=? ORDER BY id LIMIT ?;",
                           -1, &st2, nullptr) == SQLITE_OK && st2) {
      (void)sqlite3_bind_int64(st2, 1, (sqlite3_int64)run_id);
      (void)sqlite3_bind_int(st2, 2, kMaxRows);
      while (sqlite3_step(st2) == SQLITE_ROW) {
        Json::Value tr(Json::objectValue);
        tr["id"] = (Json::Int64)sqlite3_column_int64(st2, 0);
        tr["tool_name"] = stmt_to_row(st2, 1);
        tr["tool_call_id"] = stmt_to_row(st2, 2);
        const Json::Value args_json_v = stmt_to_row(st2, 3);
        tr["arguments_json"] = args_json_v;
        if (args_json_v.isString() && !args_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(args_json_v.asString(), &parsed, &perr)) {
            tr["arguments"] = parsed;
          }
        }
        const Json::Value result_v = stmt_to_row(st2, 4);
        tr["result_text"] = result_v;
        if (result_v.isString() && !result_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_any(result_v.asString(), &parsed, &perr)) {
            tr["result"] = parsed;
          }
        }
        const Json::Value result_prompt_v = stmt_to_row(st2, 5);
        tr["result_for_prompt_text"] = result_prompt_v;
        tr["result_truncated_for_prompt"] = sqlite3_column_int(st2, 6) ? true : false;
        tools.append(tr);
      }
      sqlite3_finalize(st2);
    } else if (st2) {
      sqlite3_finalize(st2);
    }
    out["tool_records"] = tools;
  }

  if (include_artifacts) {
    Json::Value arts(Json::arrayValue);
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(h.db,
                           "SELECT id, run_id, ts_unix_ms, session_id, tool_call_id, path, kind, mime, title, autoplay, repeat, artifact_json "
                           "FROM artifacts WHERE run_id=? ORDER BY id LIMIT ?;",
                           -1, &st2, nullptr) == SQLITE_OK && st2) {
      (void)sqlite3_bind_int64(st2, 1, (sqlite3_int64)run_id);
      (void)sqlite3_bind_int(st2, 2, kMaxRows);
      while (sqlite3_step(st2) == SQLITE_ROW) {
        Json::Value a(Json::objectValue);
        a["id"] = (Json::Int64)sqlite3_column_int64(st2, 0);
        a["run_id"] = (Json::Int64)sqlite3_column_int64(st2, 1);
        a["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st2, 2);
        a["session_id"] = stmt_to_row(st2, 3);
        a["tool_call_id"] = stmt_to_row(st2, 4);
        a["path"] = stmt_to_row(st2, 5);
        a["kind"] = stmt_to_row(st2, 6);
        a["mime"] = stmt_to_row(st2, 7);
        a["title"] = stmt_to_row(st2, 8);
        a["autoplay"] = sqlite3_column_int(st2, 9) ? true : false;
        a["repeat"] = (Json::Int64)sqlite3_column_int64(st2, 10);
        const Json::Value artifact_json_v = stmt_to_row(st2, 11);
        a["artifact_json"] = artifact_json_v;
        if (artifact_json_v.isString() && !artifact_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_object(artifact_json_v.asString(), &parsed, &perr)) {
            a["artifact"] = parsed;
          }
        }
        arts.append(a);
      }
      sqlite3_finalize(st2);
    } else if (st2) {
      sqlite3_finalize(st2);
    }
    out["artifacts"] = arts;
  }

  if (include_ui_actions) {
    Json::Value rows(Json::arrayValue);
    sqlite3_stmt* st2 = nullptr;
    if (sqlite3_prepare_v2(h.db,
                           "SELECT id, run_id, ts_unix_ms, session_id, tool_call_id, type, title, message, path, mime, autoplay, repeat, action_json "
                           "FROM ui_actions WHERE run_id=? ORDER BY id LIMIT ?;",
                           -1, &st2, nullptr) == SQLITE_OK && st2) {
      (void)sqlite3_bind_int64(st2, 1, (sqlite3_int64)run_id);
      (void)sqlite3_bind_int(st2, 2, kMaxRows);
      while (sqlite3_step(st2) == SQLITE_ROW) {
        Json::Value a(Json::objectValue);
        a["id"] = (Json::Int64)sqlite3_column_int64(st2, 0);
        a["run_id"] = (Json::Int64)sqlite3_column_int64(st2, 1);
        a["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st2, 2);
        a["session_id"] = stmt_to_row(st2, 3);
        a["tool_call_id"] = stmt_to_row(st2, 4);
        a["type"] = stmt_to_row(st2, 5);
        a["title"] = stmt_to_row(st2, 6);
        a["message"] = stmt_to_row(st2, 7);
        a["path"] = stmt_to_row(st2, 8);
        a["mime"] = stmt_to_row(st2, 9);
        a["autoplay"] = sqlite3_column_int(st2, 10) ? true : false;
        a["repeat"] = (Json::Int64)sqlite3_column_int64(st2, 11);
        const Json::Value action_json_v = stmt_to_row(st2, 12);
        a["action_json"] = action_json_v;
        if (action_json_v.isString() && !action_json_v.asString().empty()) {
          Json::Value parsed;
          std::string perr;
          if (json_parse_object(action_json_v.asString(), &parsed, &perr)) {
            a["action"] = parsed;
          }
        }
        rows.append(a);
      }
      sqlite3_finalize(st2);
    } else if (st2) {
      sqlite3_finalize(st2);
    }
    out["ui_actions"] = rows;
  }

  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_artifacts_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing session_id";
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
    "SELECT id, run_id, ts_unix_ms, path, kind, mime, title, autoplay, repeat, artifact_json "
    "FROM artifacts WHERE session_id=? ORDER BY ts_unix_ms DESC LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, sid->c_str(), (int)sid->size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);

  Json::Value arts(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value a(Json::objectValue);
    a["id"] = (Json::Int64)sqlite3_column_int64(st, 0);
    a["run_id"] = (Json::Int64)sqlite3_column_int64(st, 1);
    a["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 2);
    a["path"] = stmt_to_row(st, 3);
    a["kind"] = stmt_to_row(st, 4);
    a["mime"] = stmt_to_row(st, 5);
    a["title"] = stmt_to_row(st, 6);
    a["autoplay"] = sqlite3_column_int(st, 7) ? true : false;
    a["repeat"] = (Json::Int64)sqlite3_column_int64(st, 8);
    const Json::Value artifact_json_v = stmt_to_row(st, 9);
    a["artifact_json"] = artifact_json_v;
    if (artifact_json_v.isString() && !artifact_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_object(artifact_json_v.asString(), &parsed, &perr)) {
        a["artifact"] = parsed;
      }
    }
    arts.append(a);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)arts.size();
  out["artifacts"] = arts;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_ui_actions_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing session_id";
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
    "SELECT id, run_id, ts_unix_ms, tool_call_id, type, title, message, path, mime, autoplay, repeat, action_json "
    "FROM ui_actions WHERE session_id=? ORDER BY ts_unix_ms DESC LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, sid->c_str(), (int)sid->size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value a(Json::objectValue);
    a["id"] = (Json::Int64)sqlite3_column_int64(st, 0);
    a["run_id"] = (Json::Int64)sqlite3_column_int64(st, 1);
    a["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 2);
    a["tool_call_id"] = stmt_to_row(st, 3);
    a["type"] = stmt_to_row(st, 4);
    a["title"] = stmt_to_row(st, 5);
    a["message"] = stmt_to_row(st, 6);
    a["path"] = stmt_to_row(st, 7);
    a["mime"] = stmt_to_row(st, 8);
    a["autoplay"] = sqlite3_column_int(st, 9) ? true : false;
    a["repeat"] = (Json::Int64)sqlite3_column_int64(st, 10);
    const Json::Value action_json_v = stmt_to_row(st, 11);
    a["action_json"] = action_json_v;
    if (action_json_v.isString() && !action_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_object(action_json_v.asString(), &parsed, &perr)) {
        a["action"] = parsed;
      }
    }
    rows.append(a);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)rows.size();
  out["ui_actions"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_sessions_endpoint(
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
    "SELECT session_id, created_unix_ms, updated_unix_ms "
    "FROM sessions ORDER BY updated_unix_ms DESC LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value s(Json::objectValue);
    s["session_id"] = stmt_to_row(st, 0);
    s["created_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 1);
    s["updated_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 2);
    rows.append(s);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)rows.size();
  out["sessions"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_messages_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing session_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  uint64_t limit = 50;
  uint64_t offset = 0;
  uint64_t max_content_bytes = 8192;
  uint64_t tmp = 0;
  if (parse_u64_param(query_get(req.query, "limit"), &tmp)) limit = tmp;
  if (parse_u64_param(query_get(req.query, "offset"), &tmp)) offset = tmp;
  if (parse_u64_param(query_get(req.query, "max_content_bytes"), &tmp)) max_content_bytes = tmp;
  limit = clamp_u64(limit, 1, 200);
  offset = clamp_u64(offset, 0, 1000000);
  max_content_bytes = clamp_u64(max_content_bytes, 0, 1024 * 1024);

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
    "SELECT id, idx, role, content, created_unix_ms "
    "FROM messages WHERE session_id=? ORDER BY idx LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, sid->c_str(), (int)sid->size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value m(Json::objectValue);
    m["id"] = (Json::Int64)sqlite3_column_int64(st, 0);
    m["idx"] = (Json::Int64)sqlite3_column_int64(st, 1);
    m["role"] = stmt_to_row(st, 2);
    m["created_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 4);

    const unsigned char* txt = sqlite3_column_text(st, 3);
    const int n = sqlite3_column_bytes(st, 3);
    if (!txt || n <= 0) {
      m["content"] = "";
      m["content_truncated"] = false;
      m["content_bytes"] = 0;
    } else if (max_content_bytes == 0 || (uint64_t)n <= max_content_bytes) {
      m["content"] = std::string((const char*)txt, (size_t)n);
      m["content_truncated"] = false;
      m["content_bytes"] = (Json::Int64)n;
    } else {
      m["content"] = std::string((const char*)txt, (size_t)max_content_bytes);
      m["content_truncated"] = true;
      m["content_bytes"] = (Json::Int64)n;
    }
    rows.append(m);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["max_content_bytes"] = (Json::UInt64)max_content_bytes;
  out["count"] = (Json::UInt64)rows.size();
  out["messages"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

void handle_db_client_events_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "missing session_id";
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
    "SELECT id, ts_unix_ms, type, data_json "
    "FROM client_events WHERE session_id=? ORDER BY ts_unix_ms DESC LIMIT ? OFFSET ?;";
  if (sqlite3_prepare_v2(h.db, sql, -1, &st, nullptr) != SQLITE_OK) {
    Json::Value err(Json::objectValue);
    err["ok"] = false;
    err["rpc_status"] = 500;
    err["error"] = sqlite3_errmsg(h.db);
    resp->status = 500;
    resp->body = json_stringify(err);
    return;
  }
  (void)sqlite3_bind_text(st, 1, sid->c_str(), (int)sid->size(), SQLITE_TRANSIENT);
  (void)sqlite3_bind_int64(st, 2, (sqlite3_int64)limit);
  (void)sqlite3_bind_int64(st, 3, (sqlite3_int64)offset);

  Json::Value rows(Json::arrayValue);
  while (sqlite3_step(st) == SQLITE_ROW) {
    Json::Value e(Json::objectValue);
    e["id"] = (Json::Int64)sqlite3_column_int64(st, 0);
    e["ts_unix_ms"] = (Json::Int64)sqlite3_column_int64(st, 1);
    e["type"] = stmt_to_row(st, 2);
    const Json::Value data_json_v = stmt_to_row(st, 3);
    e["data_json"] = data_json_v;
    if (data_json_v.isString() && !data_json_v.asString().empty()) {
      Json::Value parsed;
      std::string perr;
      if (json_parse_object(data_json_v.asString(), &parsed, &perr)) {
        e["data"] = parsed;
      }
    }
    rows.append(e);
  }
  sqlite3_finalize(st);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  out["limit"] = (Json::UInt64)limit;
  out["offset"] = (Json::UInt64)offset;
  out["count"] = (Json::UInt64)rows.size();
  out["client_events"] = rows;
  resp->status = 200;
  resp->body = json_stringify(out);
  return;
#endif
}

}  // namespace agentd
