#include "session_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include "session_id_util.h"

#include "agent/agent.h"

#include "file_persistor.h"
#include "session_store.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <random>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <vector>

namespace agentd {

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
  if (!session_id_is_safe(sid)) {
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
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
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

void handle_session_ui_event_endpoint(
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

  if (req.body.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing JSON body"})";
    return;
  }

  Json::Value body(Json::objectValue);
  std::string perr;
  if (!json_parse_object(req.body, &body, &perr)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid JSON body"})";
    return;
  }

  const std::string session_id = body.isMember("session_id") && body["session_id"].isString() ? body["session_id"].asString() : "";
  const std::string type = body.isMember("type") && body["type"].isString() ? body["type"].asString() : "";
  const bool append_to_session =
    body.isMember("append_to_session") && body["append_to_session"].isBool() ? body["append_to_session"].asBool() : true;

  // Optional client identity (collaboration protocol):
  // - preferred: body.client.{id,kind,instance_id}
  // - legacy: body.client_id / body.client_kind / body.client_instance_id
  std::string client_id;
  std::string client_kind;
  std::string client_instance_id;
  if (body.isMember("client") && body["client"].isObject()) {
    const auto& c = body["client"];
    if (c.isMember("id") && c["id"].isString()) client_id = c["id"].asString();
    if (c.isMember("kind") && c["kind"].isString()) client_kind = c["kind"].asString();
    if (c.isMember("instance_id") && c["instance_id"].isString()) client_instance_id = c["instance_id"].asString();
  }
  if (client_id.empty() && body.isMember("client_id") && body["client_id"].isString()) client_id = body["client_id"].asString();
  if (client_kind.empty() && body.isMember("client_kind") && body["client_kind"].isString()) client_kind = body["client_kind"].asString();
  if (client_instance_id.empty() && body.isMember("client_instance_id") && body["client_instance_id"].isString()) {
    client_instance_id = body["client_instance_id"].asString();
  }

  int64_t ts_unix_ms = 0;
  if (body.isMember("ts_unix_ms") && body["ts_unix_ms"].isInt64()) {
    ts_unix_ms = body["ts_unix_ms"].asInt64();
  } else if (body.isMember("ts_unix_ms") && body["ts_unix_ms"].isUInt64()) {
    ts_unix_ms = (int64_t)body["ts_unix_ms"].asUInt64();
  } else {
    ts_unix_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }

  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }
  if (type.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing type"})";
    return;
  }

  auto is_safe_client_field = [](const std::string& s) -> bool {
    if (s.empty()) return true;
    if (s.size() > 200) return false;
    for (unsigned char c : s) {
      if (c < 0x20) return false;
    }
    return true;
  };
  if (!is_safe_client_field(client_id) || !is_safe_client_field(client_kind) || !is_safe_client_field(client_instance_id)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid client identity"})";
    return;
  }

  Json::Value payload(Json::objectValue);
  payload["type"] = type;
  payload["ts_unix_ms"] = (Json::Int64)ts_unix_ms;
  if (!client_id.empty() || !client_kind.empty() || !client_instance_id.empty()) {
    Json::Value c(Json::objectValue);
    if (!client_id.empty()) c["id"] = client_id;
    if (!client_kind.empty()) c["kind"] = client_kind;
    if (!client_instance_id.empty()) c["instance_id"] = client_instance_id;
    payload["client"] = c;
  }
  if (body.isMember("data") && body["data"].isObject()) {
    payload["data"] = body["data"];
  } else {
    payload["data"] = Json::Value(Json::objectValue);
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string payload_json = Json::writeString(wb, payload);

  bool logged_to_client_events = false;
  agent_status_t client_events_status = AGENT_OK;

  // Always append to the session-scoped client event log (host-only).
  // This is the canonical source for the client-wait host tools, and works even when the DB is disabled.
  {
    SessionStoreConfig store_cfg;
    store_cfg.root_dir = sessions_root_dir;
    client_events_status = session_store_append_client_event_jsonl(store_cfg, session_id, payload_json);
    logged_to_client_events = (client_events_status == AGENT_OK);
  }

  bool appended = false;
  if (append_to_session) {
    agent_persistor_t p{};
    const agent_status_t pst = agent_file_persistor_create(sessions_root_dir.c_str(), &p);
    agent_session_t* session = nullptr;
    agent_status_t st = pst;
    if (pst == AGENT_OK && p.load) {
      st = p.load(p.ctx, session_id.c_str(), &session);
    }
    if (st != AGENT_OK || !session) {
      // Best-effort: create the session if it does not exist yet.
      st = agent_session_create(&session);
    }

    if (st == AGENT_OK && session) {
      std::string msg = "[client_event] ";
      msg += payload_json;
      if (msg.size() > 4096) {
        msg.resize(4096);
        msg += "…";
      }
      (void)agent_session_add_message(session, AGENT_ROLE_USER, msg.c_str());
      appended = true;

      if (pst == AGENT_OK && p.save) {
        (void)p.save(p.ctx, session_id.c_str(), session);
      }

      // Best-effort DB mirror refresh for this session.
      if (db_or_null && db_or_null->is_open()) {
        std::vector<std::pair<std::string, std::string>> msgs;
        msgs.reserve(agent_session_message_count(session));
        for (size_t i = 0; i < agent_session_message_count(session); i++) {
          agent_message_view_t v{};
          if (agent_session_get_message(session, i, &v) != AGENT_OK) continue;
          msgs.emplace_back(agent_role_to_string(v.role), std::string(v.content, v.content_len));
        }
        (void)db_or_null->replace_session_messages(session_id, msgs, ts_unix_ms, nullptr);
      }

      agent_session_destroy(session);
    }
    agent_persistor_destroy(&p);
  }

  if (db_or_null && db_or_null->is_open()) {
    AgentDb::ClientEventRow er;
    er.ts_unix_ms = ts_unix_ms;
    er.session_id = session_id;
    er.type = type;
    er.data_json = payload_json;
    (void)db_or_null->insert_client_event(er, nullptr);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = session_id;
  out["type"] = type;
  out["appended_to_session"] = appended;
  out["logged_to_client_events"] = logged_to_client_events;
  if (!logged_to_client_events) {
    out["client_events_status"] = (Json::Int64)client_events_status;
  }
  resp->status = 200;
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
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
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

  bool include_rotated = false;
  if (const auto ir = query_get(req.query, "include_rotated"); ir && !ir->empty()) {
    include_rotated = string_to_bool(*ir);
  }
  size_t max_files = 0;
  if (const auto mf = query_get(req.query, "max_files")) {
    try {
      max_files = (size_t)std::stoull(*mf);
    } catch (...) {
    }
  }
  if (max_files > 10) max_files = 10;
  if (max_files == 0) max_files = store_cfg.audit_max_files;

  std::string tail;
  const agent_status_t st = include_rotated
    ? session_store_read_audit_tail_multi(store_cfg, *sid, max_bytes, max_files, &tail)
    : session_store_read_audit_tail(store_cfg, *sid, max_bytes, &tail);
  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  out["session_id"] = *sid;
  out["include_rotated"] = include_rotated;
  out["max_files"] = (Json::UInt64)max_files;
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

void handle_session_client_events_endpoint(
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
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }

  size_t max_bytes = 1024 * 1024;
  if (const auto mb = query_get(req.query, "max_bytes")) {
    try {
      max_bytes = (size_t)std::stoull(*mb);
    } catch (...) {
    }
  }
  if (max_bytes > 4 * 1024 * 1024) max_bytes = 4 * 1024 * 1024;

  bool include_rotated = false;
  if (const auto ir = query_get(req.query, "include_rotated"); ir && !ir->empty()) {
    include_rotated = string_to_bool(*ir);
  }
  size_t max_files = 0;
  if (const auto mf = query_get(req.query, "max_files")) {
    try {
      max_files = (size_t)std::stoull(*mf);
    } catch (...) {
    }
  }
  if (max_files > 10) max_files = 10;

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = sessions_root_dir;
  if (max_files == 0) max_files = store_cfg.client_events_max_files;

  std::string tail;
  const agent_status_t st = include_rotated
    ? session_store_read_client_event_tail_multi(store_cfg, *sid, max_bytes, max_files, &tail)
    : session_store_read_client_event_tail(store_cfg, *sid, max_bytes, &tail);

  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  out["include_rotated"] = include_rotated;
  out["max_files"] = (Json::UInt64)max_files;
  if (st != AGENT_OK) {
    out["error"] = "failed to read client event log";
    out["status"] = (Json::Int64)st;
  }

  // Best-effort size metadata for troubleshooting.
  {
    std::error_code ec;
    const std::filesystem::path root(sessions_root_dir);
    const std::filesystem::path current = root / (*sid + ".client_events.jsonl");
    if (std::filesystem::exists(current, ec)) {
      ec.clear();
      out["file_size_bytes"] = (Json::UInt64)std::filesystem::file_size(current, ec);
    }
    Json::Value rotated(Json::arrayValue);
    for (size_t i = 1; i < max_files; i++) {
      ec.clear();
      const std::filesystem::path p = root / (*sid + ".client_events.jsonl." + std::to_string(i));
      if (!std::filesystem::exists(p, ec)) continue;
      ec.clear();
      Json::Value e(Json::objectValue);
      e["index"] = (Json::UInt64)i;
      e["size_bytes"] = (Json::UInt64)std::filesystem::file_size(p, ec);
      rotated.append(e);
    }
    out["rotated_files"] = rotated;
  }

  Json::Value entries(Json::arrayValue);
  if (!tail.empty()) {
    std::istringstream iss(tail);
    std::string line;
    Json::CharReaderBuilder rb;
    std::string errs;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      Json::Value obj;
      std::istringstream lss(line);
      if (!Json::parseFromStream(rb, lss, &obj, &errs)) {
        continue;
      }
      entries.append(obj);
    }
  }
  out["count"] = (Json::UInt64)entries.size();
  out["events"] = entries;
  resp->body = json_stringify(out);
}

void handle_session_clients_endpoint(
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
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }

  size_t max_bytes = 1024 * 1024;
  if (const auto mb = query_get(req.query, "max_bytes")) {
    try {
      max_bytes = (size_t)std::stoull(*mb);
    } catch (...) {
    }
  }
  if (max_bytes > 4 * 1024 * 1024) max_bytes = 4 * 1024 * 1024;

  bool include_rotated = false;
  if (const auto ir = query_get(req.query, "include_rotated"); ir && !ir->empty()) {
    include_rotated = string_to_bool(*ir);
  }
  size_t max_files = 0;
  if (const auto mf = query_get(req.query, "max_files")) {
    try {
      max_files = (size_t)std::stoull(*mf);
    } catch (...) {
    }
  }
  if (max_files > 10) max_files = 10;

  SessionStoreConfig store_cfg;
  store_cfg.root_dir = sessions_root_dir;
  if (max_files == 0) max_files = store_cfg.client_events_max_files;

  std::string tail;
  const agent_status_t st = include_rotated
    ? session_store_read_client_event_tail_multi(store_cfg, *sid, max_bytes, max_files, &tail)
    : session_store_read_client_event_tail(store_cfg, *sid, max_bytes, &tail);

  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  out["include_rotated"] = include_rotated;
  out["max_files"] = (Json::UInt64)max_files;
  if (st != AGENT_OK) {
    out["error"] = "failed to read client event log";
    out["status"] = (Json::Int64)st;
    out["clients"] = Json::Value(Json::arrayValue);
    resp->body = json_stringify(out);
    return;
  }

  struct ClientSeen {
    std::string id;
    std::string kind;
    std::string instance_id;
    int64_t ts_unix_ms = 0;
    std::string last_type;
  };
  std::unordered_map<std::string, ClientSeen> seen;

  if (!tail.empty()) {
    std::istringstream iss(tail);
    std::string line;
    Json::CharReaderBuilder rb;
    std::string errs;
    while (std::getline(iss, line)) {
      if (line.empty()) continue;
      Json::Value obj;
      std::istringstream lss(line);
      if (!Json::parseFromStream(rb, lss, &obj, &errs) || !obj.isObject()) {
        continue;
      }
      const auto& c = obj["client"];
      if (!c.isObject()) continue;
      const auto& cid = c["id"];
      if (!cid.isString() || cid.asString().empty()) continue;
      const std::string id = cid.asString();
      const std::string kind = c.isMember("kind") && c["kind"].isString() ? c["kind"].asString() : "";
      const std::string inst = c.isMember("instance_id") && c["instance_id"].isString() ? c["instance_id"].asString() : "";

      int64_t ts = 0;
      const auto& tsv = obj["ts_unix_ms"];
      if (tsv.isInt64()) ts = tsv.asInt64();
      else if (tsv.isUInt64()) ts = (int64_t)tsv.asUInt64();
      const std::string et = obj.isMember("type") && obj["type"].isString() ? obj["type"].asString() : "";

      const std::string key = id + "\n" + inst + "\n" + kind;
      auto it = seen.find(key);
      if (it == seen.end() || ts >= it->second.ts_unix_ms) {
        ClientSeen cs;
        cs.id = id;
        cs.kind = kind;
        cs.instance_id = inst;
        cs.ts_unix_ms = ts;
        cs.last_type = et;
        seen[key] = std::move(cs);
      }
    }
  }

  std::vector<ClientSeen> clients;
  clients.reserve(seen.size());
  for (auto& kv : seen) {
    clients.push_back(std::move(kv.second));
  }
  std::sort(clients.begin(), clients.end(), [](const ClientSeen& a, const ClientSeen& b) {
    return a.ts_unix_ms > b.ts_unix_ms;
  });

  Json::Value arr(Json::arrayValue);
  for (const auto& c : clients) {
    Json::Value v(Json::objectValue);
    v["id"] = c.id;
    if (!c.kind.empty()) v["kind"] = c.kind;
    if (!c.instance_id.empty()) v["instance_id"] = c.instance_id;
    v["last_ts_unix_ms"] = (Json::Int64)c.ts_unix_ms;
    if (!c.last_type.empty()) v["last_type"] = c.last_type;
    arr.append(v);
  }
  out["count"] = (Json::UInt64)arr.size();
  out["clients"] = arr;
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
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
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

  bool include_rotated = false;
  if (const auto ir = query_get(req.query, "include_rotated"); ir && !ir->empty()) {
    include_rotated = string_to_bool(*ir);
  }
  size_t max_files = 0;
  if (const auto mf = query_get(req.query, "max_files")) {
    try {
      max_files = (size_t)std::stoull(*mf);
    } catch (...) {
    }
  }
  if (max_files > 10) max_files = 10;
  if (max_files == 0) max_files = store_cfg.audit_max_files;

  std::string tail;
  const agent_status_t st = include_rotated
    ? session_store_read_audit_tail_multi(store_cfg, *sid, max_bytes, max_files, &tail)
    : session_store_read_audit_tail(store_cfg, *sid, max_bytes, &tail);

  Json::Value out(Json::objectValue);
  out["ok"] = (st == AGENT_OK);
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  out["max_artifacts"] = (Json::UInt64)max_artifacts;
  out["include_rotated"] = include_rotated;
  out["max_files"] = (Json::UInt64)max_files;
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
