#include "session_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include "agent/agent.h"

#include "file_persistor.h"
#include "session_store.h"

#include <json/json.h>

#include <sstream>
#include <vector>

namespace agentd {

void handle_sessions_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = sessions_root_dir;

  agent_persistor_t p{};
  const agent_status_t pst = agent_file_persistor_create(store_cfg.root_dir.c_str(), &p);
  std::vector<std::string> ids;
  agent_status_t st = pst;
  if (pst == AGENT_OK && p.list) {
    auto sink = [](void* vctx, const char* id) {
      auto* vec = static_cast<std::vector<std::string>*>(vctx);
      vec->push_back(id ? id : "");
    };
    st = p.list(p.ctx, sink, &ids);
  }
  agent_persistor_destroy(&p);

  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  if (st != AGENT_OK) {
    out["error"] = "failed to list sessions";
    out["status"] = (Json::Int64)st;
  }
  Json::Value arr(Json::arrayValue);
  for (const auto& s : ids) {
    arr.append(s);
  }
  out["sessions"] = arr;
  resp->body = json_stringify(out);
}

void handle_session_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing session_id"})";
    return;
  }

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = sessions_root_dir;

  agent_session_t* session = nullptr;
  agent_persistor_t p{};
  const agent_status_t pst = agent_file_persistor_create(store_cfg.root_dir.c_str(), &p);
  const agent_status_t st = (pst == AGENT_OK) ? p.load(p.ctx, sid->c_str(), &session) : pst;
  agent_persistor_destroy(&p);
  if (st != AGENT_OK || !session) {
    resp->status = 500;
    Json::Value out(Json::objectValue);
    out["ok"] = false;
    out["error"] = "failed to load session";
    out["status"] = (Json::Int64)st;
    resp->body = json_stringify(out);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  Json::Value msgs(Json::arrayValue);
  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t v{};
    if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
    Json::Value m(Json::objectValue);
    m["role"] = agent_role_to_string(v.role);
    m["content"] = std::string(v.content, v.content_len);
    msgs.append(m);
  }
  out["messages"] = msgs;
  agent_session_destroy(session);
  resp->body = json_stringify(out);
}

void handle_session_audit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing session_id"})";
    return;
  }
  size_t max_bytes = 1024 * 1024;
  if (const auto mb = query_get(req.query, "max_bytes")) {
    try {
      max_bytes = (size_t)std::stoull(*mb);
    } catch (...) {
    }
  }

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = sessions_root_dir;

  std::string tail;
  const agent_status_t st = session_store_read_audit_tail(store_cfg, *sid, max_bytes, &tail);
  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  out["session_id"] = *sid;
  if (st != AGENT_OK) {
    out["error"] = "failed to read audit log";
    out["status"] = (Json::Int64)st;
  }
  // Parse JSONL into entries (best-effort).
  Json::Value entries(Json::arrayValue);
  std::istringstream iss(tail);
  std::string line;
  Json::CharReaderBuilder rb;
  while (std::getline(iss, line)) {
    if (line.empty()) continue;
    std::string errs;
    std::istringstream lss(line);
    Json::Value v;
    if (Json::parseFromStream(rb, lss, &v, &errs) && v.isObject()) {
      entries.append(v);
    }
  }
  out["entries"] = entries;
  resp->body = json_stringify(out);
}

void handle_session_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing session_id"})";
    return;
  }

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = sessions_root_dir;

  agent_persistor_t p{};
  const agent_status_t pst = agent_file_persistor_create(store_cfg.root_dir.c_str(), &p);
  const agent_status_t st = (pst == AGENT_OK) ? p.del(p.ctx, sid->c_str()) : pst;
  agent_persistor_destroy(&p);
  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  out["session_id"] = *sid;
  if (st != AGENT_OK) {
    out["error"] = "failed to delete session";
    out["status"] = (Json::Int64)st;
  }

  // Best-effort DB mirror cleanup (only if DB is enabled).
  if (db_or_null && db_or_null->is_open()) {
    std::string db_err;
    (void)db_or_null->delete_session(*sid, &db_err);
  }

  resp->body = json_stringify(out);
}

}  // namespace agentd
