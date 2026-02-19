#include "run_replay_endpoint.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include "agent/json_c14n.h"

#include <json/json.h>

#include <chrono>
#include <cstdint>
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

static Json::Value open_db_or_error(const AgentDb* db_or_null, DbHandle* out) {
  Json::Value o(Json::objectValue);
  if (!db_or_null || !db_or_null->is_open()) {
    o["ok"] = false;
    o["rpc_status"] = 404;
    o["error"] = "db disabled";
    return o;
  }
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
  o["ok"] = true;
  return o;
#endif
}

static std::string stmt_text(sqlite3_stmt* st, int idx) {
  const unsigned char* t = sqlite3_column_text(st, idx);
  return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
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

static int64_t now_utc_ms() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

#if defined(AGENT_HAVE_SQLITE3)
struct ReplayBundleLoad {
  bool ok = false;
  int status = 500;
  Json::Value error_body = Json::Value(Json::objectValue);
  std::string session_id;
  Json::Value bundle = Json::Value(Json::objectValue);
  std::string replay_sha256;
  std::string replay_sha256_alg;
  std::string replay_sha256_schema;
  std::string replay_error;
};

static ReplayBundleLoad replay_error(
  int status,
  const std::string& error,
  const std::string& details = std::string()
) {
  ReplayBundleLoad out;
  out.ok = false;
  out.status = status;
  Json::Value o(Json::objectValue);
  o["ok"] = false;
  o["rpc_status"] = status;
  o["error"] = error;
  if (!details.empty()) o["details"] = details;
  out.error_body = o;
  return out;
}

static ReplayBundleLoad load_replay_bundle(sqlite3* db, int64_t run_id) {
  if (!db) return replay_error(500, "internal error");

  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT session_id, request_json, response_json, replay_sha256, replay_sha256_alg, replay_sha256_schema, replay_error "
    "FROM runs WHERE run_id=? LIMIT 1;";
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
    return replay_error(500, sqlite3_errmsg(db));
  }
  (void)sqlite3_bind_int64(st, 1, (sqlite3_int64)run_id);
  const int rc = sqlite3_step(st);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(st);
    return replay_error(404, "run not found");
  }

  const std::string session_id = stmt_text(st, 0);
  const std::string request_json = stmt_text(st, 1);
  const std::string response_json = stmt_text(st, 2);
  std::string replay_sha256 = stmt_text(st, 3);
  std::string replay_sha256_alg = stmt_text(st, 4);
  std::string replay_sha256_schema = stmt_text(st, 5);
  std::string replay_error_text = stmt_text(st, 6);
  sqlite3_finalize(st);

  if (request_json.empty() || response_json.empty()) {
    ReplayBundleLoad out = replay_error(409, "replay_not_available");
    out.error_body["run_id"] = (Json::Int64)run_id;
    if (!session_id.empty()) out.error_body["session_id"] = session_id;
    if (!replay_error_text.empty()) out.error_body["replay_error"] = replay_error_text;
    return out;
  }

  Json::Value request_v;
  Json::Value response_v;
  std::string perr;
  if (!json_parse_any(request_json, &request_v, &perr)) {
    return replay_error(500, "replay_request_parse_failed", perr);
  }
  perr.clear();
  if (!json_parse_any(response_json, &response_v, &perr)) {
    return replay_error(500, "replay_response_parse_failed", perr);
  }

  Json::Value tool_records(Json::arrayValue);
  sqlite3_stmt* st2 = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT tool_name, tool_call_id, arguments_json, result_text, result_for_prompt_text, result_truncated_for_prompt "
                         "FROM tool_records WHERE run_id=? ORDER BY id;",
                         -1, &st2, nullptr) == SQLITE_OK && st2) {
    (void)sqlite3_bind_int64(st2, 1, (sqlite3_int64)run_id);
    while (sqlite3_step(st2) == SQLITE_ROW) {
      Json::Value tr(Json::objectValue);
      tr["tool_name"] = stmt_text(st2, 0);
      const std::string tool_call_id = stmt_text(st2, 1);
      if (!tool_call_id.empty()) tr["tool_call_id"] = tool_call_id;
      const std::string args_json = stmt_text(st2, 2);
      if (!args_json.empty()) tr["arguments_json"] = args_json;
      const std::string result_text = stmt_text(st2, 3);
      if (!result_text.empty()) tr["result_text"] = result_text;
      const std::string result_prompt = stmt_text(st2, 4);
      if (!result_prompt.empty()) tr["result_for_prompt_text"] = result_prompt;
      tr["result_truncated_for_prompt"] = sqlite3_column_int(st2, 5) ? true : false;
      tool_records.append(tr);
    }
    sqlite3_finalize(st2);
  } else if (st2) {
    sqlite3_finalize(st2);
  }

  Json::Value bundle(Json::objectValue);
  bundle["schema"] = "run_replay_bundle_v1";
  bundle["request"] = request_v;
  bundle["response"] = response_v;
  bundle["tool_records"] = tool_records;

  if (replay_sha256.empty()) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string bundle_json = Json::writeString(wb, bundle);
    char token[80] = {0};
    char err_buf[128] = {0};
    const agent_status_t st_hash =
      agent_json_c14n_sha256_token(bundle_json.data(), bundle_json.size(), token, err_buf, sizeof(err_buf));
    if (st_hash == AGENT_OK) {
      replay_sha256 = std::string(token);
      replay_sha256_alg = "agent_json_c14n_v1";
      replay_sha256_schema = "run_replay_bundle_v1";
    } else {
      const std::string err = err_buf[0] ? std::string(err_buf) : "replay_hash_failed";
      if (replay_error_text.empty()) replay_error_text = err;
      else replay_error_text.append(";").append(err);
    }
  }

  ReplayBundleLoad out;
  out.ok = true;
  out.status = 200;
  out.session_id = session_id;
  out.bundle = bundle;
  out.replay_sha256 = replay_sha256;
  out.replay_sha256_alg = replay_sha256_alg;
  out.replay_sha256_schema = replay_sha256_schema;
  out.replay_error = replay_error_text;
  return out;
}
#endif

}  // namespace

void handle_run_replay_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  int64_t run_id = 0;
  if (!parse_i64_param(query_get(req.query, "run_id"), &run_id) || run_id <= 0) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid run_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  DbHandle h;
  Json::Value db_open = open_db_or_error(db_or_null, &h);
  if (!db_open.isObject() || !db_open.get("ok", false).asBool()) {
    const int st = db_open.isMember("rpc_status") && db_open["rpc_status"].isInt() ? db_open["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(db_open);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = json_error_body("sqlite disabled");
  return;
#else
  ReplayBundleLoad load = load_replay_bundle(h.db, run_id);
  if (!load.ok) {
    resp->status = load.status;
    resp->body = json_stringify(load.error_body);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["run_id"] = (Json::Int64)run_id;
  if (!load.session_id.empty()) out["session_id"] = load.session_id;
  out["bundle"] = load.bundle;
  if (!load.replay_sha256.empty()) out["replay_sha256"] = load.replay_sha256;
  if (!load.replay_sha256_alg.empty()) out["replay_sha256_alg"] = load.replay_sha256_alg;
  if (!load.replay_sha256_schema.empty()) out["replay_sha256_schema"] = load.replay_sha256_schema;
  if (!load.replay_error.empty()) out["replay_error"] = load.replay_error;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

void handle_run_attestation_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  int64_t run_id = 0;
  if (!parse_i64_param(query_get(req.query, "run_id"), &run_id) || run_id <= 0) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 400;
    o["error"] = "invalid run_id";
    resp->status = 400;
    resp->body = json_stringify(o);
    return;
  }

  DbHandle h;
  Json::Value db_open = open_db_or_error(db_or_null, &h);
  if (!db_open.isObject() || !db_open.get("ok", false).asBool()) {
    const int st = db_open.isMember("rpc_status") && db_open["rpc_status"].isInt() ? db_open["rpc_status"].asInt() : 500;
    resp->status = st;
    resp->body = json_stringify(db_open);
    return;
  }

#if !defined(AGENT_HAVE_SQLITE3)
  resp->status = 500;
  resp->body = json_error_body("sqlite disabled");
  return;
#else
  ReplayBundleLoad load = load_replay_bundle(h.db, run_id);
  if (!load.ok) {
    resp->status = load.status;
    resp->body = json_stringify(load.error_body);
    return;
  }

  Json::Value att(Json::objectValue);
  att["schema"] = "run_attestation_bundle_v1";
  att["created_utc_ms"] = (Json::Int64)now_utc_ms();
  att["replay_sha256"] = load.replay_sha256;
  att["replay_sha256_alg"] = load.replay_sha256_alg;
  att["replay_sha256_schema"] = load.replay_sha256_schema;
  att["run_id"] = (Json::Int64)run_id;
  if (!load.session_id.empty()) att["session_id"] = load.session_id;

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["run_id"] = (Json::Int64)run_id;
  if (!load.session_id.empty()) out["session_id"] = load.session_id;
  out["attestation"] = att;
  if (!load.replay_error.empty()) out["replay_error"] = load.replay_error;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

}  // namespace agentd
