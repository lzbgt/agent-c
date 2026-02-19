#include "run_replay_endpoint.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include "agent/json_c14n.h"

#include <json/json.h>

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
  sqlite3_stmt* st = nullptr;
  const char* sql =
    "SELECT session_id, request_json, response_json, replay_sha256, replay_sha256_alg, replay_sha256_schema, replay_error "
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

  const std::string session_id = stmt_text(st, 0);
  const std::string request_json = stmt_text(st, 1);
  const std::string response_json = stmt_text(st, 2);
  std::string replay_sha256 = stmt_text(st, 3);
  std::string replay_sha256_alg = stmt_text(st, 4);
  std::string replay_sha256_schema = stmt_text(st, 5);
  std::string replay_error = stmt_text(st, 6);
  sqlite3_finalize(st);

  if (request_json.empty() || response_json.empty()) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 409;
    o["error"] = "replay_not_available";
    o["run_id"] = (Json::Int64)run_id;
    if (!session_id.empty()) o["session_id"] = session_id;
    if (!replay_error.empty()) o["replay_error"] = replay_error;
    resp->status = 409;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value request_v;
  Json::Value response_v;
  std::string perr;
  if (!json_parse_any(request_json, &request_v, &perr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = "replay_request_parse_failed";
    o["details"] = perr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }
  perr.clear();
  if (!json_parse_any(response_json, &response_v, &perr)) {
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["rpc_status"] = 500;
    o["error"] = "replay_response_parse_failed";
    o["details"] = perr;
    resp->status = 500;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value tool_records(Json::arrayValue);
  sqlite3_stmt* st2 = nullptr;
  if (sqlite3_prepare_v2(h.db,
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
      if (replay_error.empty()) replay_error = err;
      else replay_error.append(";").append(err);
    }
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["run_id"] = (Json::Int64)run_id;
  if (!session_id.empty()) out["session_id"] = session_id;
  out["bundle"] = bundle;
  if (!replay_sha256.empty()) out["replay_sha256"] = replay_sha256;
  if (!replay_sha256_alg.empty()) out["replay_sha256_alg"] = replay_sha256_alg;
  if (!replay_sha256_schema.empty()) out["replay_sha256_schema"] = replay_sha256_schema;
  if (!replay_error.empty()) out["replay_error"] = replay_error;
  resp->status = 200;
  resp->body = json_stringify(out);
#endif
}

}  // namespace agentd
