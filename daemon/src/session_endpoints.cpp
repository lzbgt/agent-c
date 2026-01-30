#include "session_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include "agent/agent.h"

#include "file_persistor.h"
#include "session_store.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
#include <vector>

namespace agentd {

static bool is_safe_session_id(const std::string& s) {
  if (s.empty()) return false;
  if (s.size() > 200) return false;
  // Session ids become filenames; reject path traversal and separators.
  if (s.find('/') != std::string::npos) return false;
  if (s.find('\\') != std::string::npos) return false;
  if (s == "." || s == "..") return false;
  if (s.find("..") != std::string::npos) return false;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-' || c == '.') {
      continue;
    }
    return false;
  }
  return true;
}

static std::string make_uuidish_session_id() {
  // Best-effort UUIDv4-ish without external deps. Good enough to avoid collisions in practice.
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);

  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);

  // v4 + variant.
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);

  char buf[64];
  (void)snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
}

static bool session_files_exist(const std::string& sessions_root_dir, const std::string& session_id) {
  std::error_code ec;
  const std::filesystem::path root(sessions_root_dir);
  const std::filesystem::path sess = root / (session_id + ".sess");
  const std::filesystem::path json = root / (session_id + ".json");
  if (std::filesystem::exists(sess, ec)) return true;
  ec.clear();
  if (std::filesystem::exists(json, ec)) return true;
  return false;
}

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

void handle_session_new_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  std::string requested_id;
  bool create_files = true;
  if (!req.body.empty()) {
    Json::Value body(Json::objectValue);
    std::string err;
    if (!json_parse_object(req.body, &body, &err)) {
      resp->status = 400;
      resp->body = R"({"ok":false,"error":"invalid JSON body"})";
      return;
    }
    if (body.isMember("session_id") && body["session_id"].isString()) {
      requested_id = body["session_id"].asString();
    }
    if (body.isMember("create_files") && body["create_files"].isBool()) {
      create_files = body["create_files"].asBool();
    }
  }

  std::string sid = requested_id.empty() ? make_uuidish_session_id() : requested_id;
  if (!is_safe_session_id(sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }

  // Avoid collisions when autogenerating. (requested ids are allowed to already exist)
  if (requested_id.empty()) {
    for (int i = 0; i < 8 && session_files_exist(sessions_root_dir, sid); i++) {
      sid = make_uuidish_session_id();
    }
  }

  const bool existed = session_files_exist(sessions_root_dir, sid);

  if (create_files && !existed) {
    agent_session_t* session = nullptr;
    agent_status_t st = agent_session_create(&session);
    if (st != AGENT_OK || !session) {
      resp->status = 500;
      resp->body = R"({"ok":false,"error":"failed to create session"})";
      return;
    }

    agent_persistor_t p{};
    const agent_status_t pst = agent_file_persistor_create(sessions_root_dir.c_str(), &p);
    st = (pst == AGENT_OK && p.save) ? p.save(p.ctx, sid.c_str(), session) : pst;
    agent_persistor_destroy(&p);
    agent_session_destroy(session);

    if (st != AGENT_OK) {
      resp->status = 500;
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["error"] = "failed to save session";
      out["status"] = (Json::Int64)st;
      resp->body = json_stringify(out);
      return;
    }
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = sid;
  out["created"] = !existed;
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

void handle_session_artifacts_endpoint(
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
  size_t max_bytes = 2 * 1024 * 1024;
  if (const auto mb = query_get(req.query, "max_bytes")) {
    try {
      max_bytes = (size_t)std::stoull(*mb);
    } catch (...) {
    }
  }
  size_t max_artifacts = 64;
  if (const auto ma = query_get(req.query, "max_artifacts")) {
    try {
      max_artifacts = (size_t)std::stoull(*ma);
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
  out["max_bytes"] = (Json::UInt64)max_bytes;
  out["max_artifacts"] = (Json::UInt64)max_artifacts;
  if (st != AGENT_OK) {
    out["error"] = "failed to read audit log";
    out["status"] = (Json::Int64)st;
    out["artifacts"] = Json::Value(Json::arrayValue);
    resp->body = json_stringify(out);
    return;
  }

  Json::Value artifacts(Json::arrayValue);
  Json::CharReaderBuilder rb;
  std::istringstream iss(tail);
  std::string line;

  while (std::getline(iss, line)) {
    if (artifacts.size() >= (Json::ArrayIndex)max_artifacts) break;
    if (line.empty()) continue;
    std::string errs;
    std::istringstream lss(line);
    Json::Value rec;
    if (!Json::parseFromStream(rb, lss, &rec, &errs) || !rec.isObject()) {
      continue;
    }
    const int64_t ts = rec.isMember("ts_unix_ms") && rec["ts_unix_ms"].isInt64() ? rec["ts_unix_ms"].asInt64() : 0;
    const std::string prompt = rec.isMember("prompt") && rec["prompt"].isString() ? rec["prompt"].asString() : "";

    const Json::Value evs = rec["events"];
    if (!evs.isArray()) continue;
    for (Json::ArrayIndex i = 0; i < evs.size(); i++) {
      if (artifacts.size() >= (Json::ArrayIndex)max_artifacts) break;
      const auto& ev = evs[i];
      if (!ev.isObject()) continue;
      const auto& t = ev["type"];
      if (!t.isString() || t.asString() != "artifact") continue;
      Json::Value a(Json::objectValue);
      a["ts_unix_ms"] = (Json::Int64)ts;
      if (!prompt.empty()) a["prompt"] = prompt;
      if (ev.isMember("data")) {
        a["data"] = ev["data"];
      } else {
        a["data"] = Json::Value(Json::objectValue);
      }
      artifacts.append(a);
    }
  }

  out["count"] = (Json::UInt64)artifacts.size();
  out["artifacts"] = artifacts;
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
