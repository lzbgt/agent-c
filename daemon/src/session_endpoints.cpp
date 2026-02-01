#include "session_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include "session_id_util.h"
#include "scene_store.h"

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

void handle_sessions_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  if (!db || !db->is_open()) {
    out["error"] = "db not available";
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  std::vector<std::string> ids;
  std::string err;
  if (!db->list_sessions(&ids, &err)) {
    out["error"] = err.empty() ? "failed to list sessions" : err;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  out["ok"] = true;
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
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  std::string requested_id;
  bool create_session = true;
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
      // Legacy name kept: now means "create session rows in DB".
      create_session = body["create_files"].asBool();
    }
  }

  std::string sid = requested_id.empty() ? make_uuidish_session_id() : requested_id;
  if (!session_id_is_safe(sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }

  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

  bool existed = false;
  {
    std::string err;
    if (!db->session_exists(sid, &existed, &err)) {
      resp->status = 500;
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["error"] = err.empty() ? "failed to query session existence" : err;
      resp->body = json_stringify(out);
      return;
    }
  }

  // Avoid collisions when autogenerating. (requested ids are allowed to already exist)
  if (requested_id.empty()) {
    for (int i = 0; i < 12 && existed; i++) {
      sid = make_uuidish_session_id();
      std::string err;
      if (!db->session_exists(sid, &existed, &err)) break;
    }
  }

  if (create_session && !existed) {
    std::string err;
    const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<std::pair<std::string, std::string>> empty;
    if (!db->replace_session_messages(sid, empty, now_ms, &err)) {
      resp->status = 500;
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["error"] = err.empty() ? "failed to create session" : err;
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
  AgentDb* db,
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

  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

  bool exists = false;
  {
    std::string err;
    if (!db->session_exists(*sid, &exists, &err)) {
      resp->status = 500;
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["error"] = err.empty() ? "failed to query session" : err;
      resp->body = json_stringify(out);
      return;
    }
  }
  if (!exists) {
    resp->status = 404;
    resp->body = R"({"ok":false,"error":"session not found"})";
    return;
  }

  std::vector<std::pair<std::string, std::string>> msgs;
  {
    std::string err;
    if (!db->load_session_messages(*sid, &msgs, &err)) {
      resp->status = 500;
      Json::Value out(Json::objectValue);
      out["ok"] = false;
      out["error"] = err.empty() ? "failed to load session" : err;
      resp->body = json_stringify(out);
      return;
    }
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = *sid;
  Json::Value arr(Json::arrayValue);
  for (const auto& rc : msgs) {
    Json::Value mv(Json::objectValue);
    mv["role"] = rc.first;
    mv["content"] = rc.second;
    arr.append(mv);
  }
  out["messages"] = arr;
  resp->body = json_stringify(out);
}

void handle_session_ui_event_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

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

  bool appended = false;
  if (append_to_session) {
    std::vector<std::pair<std::string, std::string>> msgs;
    std::string err;
    (void)db->load_session_messages(session_id, &msgs, &err); // missing session => empty

    std::string msg = "[client_event] ";
    msg += payload_json;
    if (msg.size() > 4096) {
      msg.resize(4096);
      msg += "…";
    }
    msgs.emplace_back("user", msg);
    appended = true;

    err.clear();
    if (!db->replace_session_messages(session_id, msgs, ts_unix_ms, &err)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = err.empty() ? "failed to append to session" : err;
      resp->body = json_stringify(o);
      return;
    }
  }

  {
    AgentDb::ClientEventRow er;
    er.ts_unix_ms = ts_unix_ms;
    er.session_id = session_id;
    er.type = type;
    er.data_json = payload_json;
    std::string err;
    (void)db->insert_client_event(er, &err);
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = session_id;
  out["type"] = type;
  out["appended_to_session"] = appended;
  out["logged_to_client_events"] = true;
  resp->status = 200;
  resp->body = json_stringify(out);
}

void handle_session_audit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

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

  std::vector<std::string> recs_desc;
  std::string err;
  const bool ok = db->read_audit_records_tail(*sid, max_bytes, /*max_records=*/0, &recs_desc, &err);
  Json::Value out(Json::objectValue);
  out["ok"] = ok;
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  if (!ok) {
    out["error"] = err.empty() ? "failed to read audit records" : err;
  }
  // Parse stored JSON objects into entries (best-effort).
  Json::Value entries(Json::arrayValue);
  Json::CharReaderBuilder rb;
  for (const auto& s : recs_desc) {
    if (s.empty()) continue;
    std::string perr;
    std::istringstream iss(s);
    Json::Value v;
    if (Json::parseFromStream(rb, iss, &v, &perr) && v.isObject()) entries.append(v);
  }
  out["entries"] = entries;
  resp->body = json_stringify(out);
}

void handle_session_client_events_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

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

  std::string tail;
  std::string err;
  const bool ok_tail = db->read_client_events_tail_jsonl(*sid, max_bytes, /*max_events=*/0, &tail, &err);

  Json::Value out(Json::objectValue);
  out["ok"] = ok_tail;
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  if (!ok_tail) {
    out["error"] = err.empty() ? "failed to read client event log" : err;
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
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

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
  (void)include_rotated;

  std::string tail;
  std::string terr;
  const bool ok_tail = db->read_client_events_tail_jsonl(*sid, max_bytes, /*max_events=*/0, &tail, &terr);

  Json::Value out(Json::objectValue);
  out["ok"] = ok_tail;
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  if (!ok_tail) {
    out["error"] = terr.empty() ? "failed to read client event log" : terr;
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
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;
  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

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
  if (max_artifacts > 512) max_artifacts = 512;

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  out["max_artifacts"] = (Json::UInt64)max_artifacts;

  std::vector<AgentDb::ArtifactRow> rows;
  std::string err;
  if (!db->list_artifacts_by_session(*sid, max_artifacts, &rows, &err)) {
    out["error"] = err.empty() ? "failed to list artifacts" : err;
    out["artifacts"] = Json::Value(Json::arrayValue);
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  Json::Value artifacts(Json::arrayValue);
  Json::CharReaderBuilder rb;
  size_t used = 0;
  for (const auto& r : rows) {
    if ((size_t)artifacts.size() >= max_artifacts) break;
    const std::string aj = r.artifact_json.empty() ? std::string("{}") : r.artifact_json;
    const size_t add = aj.size();
    if (used + add > max_bytes && artifacts.size() > 0) break;
    used += add;

    Json::Value artifact_obj(Json::objectValue);
    {
      std::string perr;
      std::istringstream iss(aj);
      Json::Value v;
      if (Json::parseFromStream(rb, iss, &v, &perr) && v.isObject()) {
        artifact_obj = v;
      } else {
        artifact_obj["path"] = r.path;
        if (!r.kind.empty()) artifact_obj["kind"] = r.kind;
        if (!r.mime.empty()) artifact_obj["mime"] = r.mime;
        if (!r.title.empty()) artifact_obj["title"] = r.title;
        artifact_obj["autoplay"] = r.autoplay;
        artifact_obj["repeat"] = r.repeat;
      }
    }

    Json::Value data(Json::objectValue);
    if (!r.tool_call_id.empty()) data["tool_call_id"] = r.tool_call_id;
    data["tool_name"] = "artifact_register";
    data["artifact"] = artifact_obj;

    Json::Value rec(Json::objectValue);
    rec["ts_unix_ms"] = (Json::Int64)r.ts_unix_ms;
    rec["data"] = data;
    artifacts.append(rec);
  }

  out["ok"] = true;
  out["count"] = (Json::UInt64)artifacts.size();
  out["artifacts"] = artifacts;
  resp->body = json_stringify(out);
}

void handle_session_scene_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  if (!db || !db->is_open()) {
    out["error"] = "db not available";
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty() || !session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }

  Json::Value scene(Json::objectValue);
  int64_t updated = 0;
  std::string err;
  if (!scene_store_get(db, *sid, &scene, &updated, &err)) {
    out["error"] = err.empty() ? "failed to read scene" : err;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  out["ok"] = true;
  out["session_id"] = *sid;
  out["updated_unix_ms"] = (Json::Int64)updated;
  out["scene"] = scene;
  resp->body = json_stringify(out);
}

void handle_session_scene_apply_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  if (!db || !db->is_open()) {
    out["error"] = "db not available";
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

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
  const std::string sid = body.isMember("session_id") && body["session_id"].isString() ? body["session_id"].asString() : "";
  if (sid.empty() || !session_id_is_safe(sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }
  const Json::Value ops = body.isMember("ops") ? body["ops"] : Json::Value(Json::arrayValue);
  if (!ops.isArray()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"ops must be an array"})";
    return;
  }

  const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

  Json::Value apply_result(Json::objectValue);
  int64_t updated = 0;
  std::string err;
  if (!scene_store_apply_ops(db, sid, ops, now, &apply_result, &updated, &err)) {
    out["error"] = err.empty() ? "failed to apply scene ops" : err;
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  Json::Value scene(Json::objectValue);
  int64_t read_updated = 0;
  std::string get_err;
  if (!scene_store_get(db, sid, &scene, &read_updated, &get_err)) {
    out["ok"] = true;
    out["session_id"] = sid;
    out["updated_unix_ms"] = (Json::Int64)updated;
    out["apply"] = apply_result;
    out["warning"] = get_err.empty() ? "applied but failed to read scene" : get_err;
    resp->body = json_stringify(out);
    return;
  }

  out["ok"] = true;
  out["session_id"] = sid;
  out["updated_unix_ms"] = (Json::Int64)read_updated;
  out["apply"] = apply_result;
  out["scene"] = scene;
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
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid session_id"})";
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = false;
  out["session_id"] = *sid;

  if (db_or_null && db_or_null->is_open()) {
    std::string db_err;
    if (!db_or_null->delete_session(*sid, &db_err)) {
      out["error"] = db_err.empty() ? "failed to delete session from db" : db_err;
      resp->status = 500;
      resp->body = json_stringify(out);
      return;
    }
    out["ok"] = true;
    out["deleted_from_db"] = true;
  } else {
    out["error"] = "db not available";
    resp->status = 500;
    resp->body = json_stringify(out);
    return;
  }

  // Best-effort legacy file cleanup (non-canonical).
  bool legacy_attempted = false;
  bool legacy_any_deleted = false;
  if (!sessions_root_dir.empty()) {
    legacy_attempted = true;
    std::error_code ec;
    const std::filesystem::path root(sessions_root_dir);
    const std::string id = *sid;
    const std::vector<std::filesystem::path> paths = {
      root / (id + ".sess"),
      root / (id + ".json"),
      root / (id + ".events.jsonl"),
      root / (id + ".client_events.jsonl"),
    };
    for (const auto& p : paths) {
      ec.clear();
      if (std::filesystem::exists(p, ec)) {
        ec.clear();
        if (std::filesystem::remove(p, ec) && !ec) legacy_any_deleted = true;
      }
    }
  }
  out["legacy_cleanup_attempted"] = legacy_attempted;
  out["legacy_any_deleted"] = legacy_any_deleted;

  resp->body = json_stringify(out);
}

}  // namespace agentd
