#include "session_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include "session_id_util.h"
#include "session_paths.h"
#include "scene_store.h"

#include "base64.h"

#include <json/json.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <vector>

namespace agentd {

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& p) {
  std::error_code ec;
  const std::filesystem::path abs_root = std::filesystem::weakly_canonical(root, ec);
  if (ec) return false;
  ec.clear();
  const std::filesystem::path abs_p = std::filesystem::weakly_canonical(p, ec);
  if (ec) return false;

  auto it_r = abs_root.begin();
  auto it_p = abs_p.begin();
  for (; it_r != abs_root.end(); ++it_r, ++it_p) {
    if (it_p == abs_p.end()) return false;
    if (*it_r != *it_p) return false;
  }
  return true;
}

static bool is_safe_filename_ascii(const std::string& s) {
  if (s.empty() || s.size() > 160) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ' ';
    if (!ok) return false;
  }
  if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) return false;
  if (s.find("..") != std::string::npos) return false;
  return true;
}

static std::string kind_from_mime(const std::string& mime) {
  const std::string m = lower_copy(trim_copy(mime));
  if (m.rfind("image/", 0) == 0) return "image";
  if (m.rfind("audio/", 0) == 0) return "audio";
  if (m.rfind("video/", 0) == 0) return "video";
  if (m.rfind("text/", 0) == 0) return "text";
  return "file";
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

static bool is_safe_printable_field(const std::string& s, size_t max_len) {
  if (s.size() > max_len) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

static std::string make_uuidish_voice_token(const char* prefix) {
  std::string out = prefix ? std::string(prefix) : std::string("voice-");
  out += make_uuidish_session_id();
  return out;
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
      resp->body = json_error_body("invalid JSON body");
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
    resp->body = json_error_body("invalid session_id");
    return;
  }

  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
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
    std::vector<AgentDb::MessageRow> empty;
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
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  if (!db || !db->is_open()) {
    resp->status = 500;
    resp->body = json_error_body("db not available");
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
    resp->body = json_error_body("session not found");
    return;
  }

  std::vector<AgentDb::MessageRow> msgs;
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
    mv["role"] = rc.role;
    mv["content"] = rc.content;
    if (!rc.mm_json.empty()) mv["mm_json"] = rc.mm_json;
    if (rc.mm_bytes > 0) mv["mm_bytes"] = (Json::Int64)rc.mm_bytes;
    if (rc.mm_truncated != 0) mv["mm_truncated"] = (Json::Int64)rc.mm_truncated;
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
    resp->body = json_error_body("db not available");
    return;
  }

  if (req.body.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing JSON body");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string perr;
  if (!json_parse_object(req.body, &body, &perr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
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
    resp->body = json_error_body("invalid session_id");
    return;
  }
  if (type.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing type");
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
    resp->body = json_error_body("invalid client identity");
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
    std::vector<AgentDb::MessageRow> msgs;
    std::string err;
    (void)db->load_session_messages(session_id, &msgs, &err); // missing session => empty

    std::string msg = "[client_event] ";
    msg += payload_json;
    if (msg.size() > 4096) {
      msg.resize(4096);
      msg += "…";
    }
    AgentDb::MessageRow row;
    row.role = "user";
    row.content = msg;
    msgs.emplace_back(std::move(row));
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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
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

void handle_session_voice_control_endpoint(
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
    resp->body = json_error_body("db not available");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
    return;
  }

  const std::string session_id = body.isMember("session_id") && body["session_id"].isString() ? trim_copy(body["session_id"].asString()) : "";
  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  const std::string action = body.isMember("action") && body["action"].isString() ? lower_copy(trim_copy(body["action"].asString())) : "";
  if (action != "play" && action != "pause" && action != "snapshot") {
    resp->status = 400;
    resp->body = json_error_body("action must be play, pause, or snapshot");
    return;
  }

  auto read_string = [&](const char* key, size_t max_len) -> std::optional<std::string> {
    if (!body.isMember(key) || !body[key].isString()) return std::nullopt;
    const std::string v = trim_copy(body[key].asString());
    if (!is_safe_printable_field(v, max_len)) return std::nullopt;
    return v;
  };

  const auto selector = read_string("selector", 200);
  const auto url = read_string("url", 8192);
  const auto src = read_string("src", 8192);
  const auto path = read_string("path", 1024);
  const auto resolved_path = read_string("resolved_path", 1024);
  const auto id = read_string("id", 80);
  const auto tag = read_string("tag", 16);
  const auto title = read_string("title", 160);
  const auto message = read_string("message", 400);

  const bool wants_selector = selector && !selector->empty();
  const bool wants_url = (url && !url->empty()) || (src && !src->empty()) || (path && !path->empty()) || (resolved_path && !resolved_path->empty());
  if (action == "play" && !wants_selector && !wants_url) {
    resp->status = 400;
    resp->body = json_error_body("voice play requires selector or url/path");
    return;
  }
  if (action == "pause" && !wants_selector && !(id && !id->empty())) {
    resp->status = 400;
    resp->body = json_error_body("voice pause requires selector or id");
    return;
  }

  const std::string rpc_kind =
    action == "play" ? "media_play" : (action == "pause" ? "media_pause" : "media_snapshot");
  const std::string rpc_id = make_uuidish_voice_token("voice-rpc-");
  const std::string tool_call_id = make_uuidish_voice_token("voice-tool-");
  const int64_t ts_unix_ms = now_unix_ms();

  Json::Value rpc_args(Json::objectValue);
  if (selector && !selector->empty()) rpc_args["selector"] = *selector;
  if (url && !url->empty()) rpc_args["url"] = *url;
  else if (src && !src->empty()) rpc_args["src"] = *src;
  if (path && !path->empty()) rpc_args["path"] = *path;
  if (resolved_path && !resolved_path->empty()) rpc_args["resolved_path"] = *resolved_path;
  if (id && !id->empty()) rpc_args["id"] = *id;
  if (tag && !tag->empty()) rpc_args["tag"] = *tag;

  if (action == "play") {
    if (body.isMember("controls") && body["controls"].isBool()) rpc_args["controls"] = body["controls"].asBool();
    if (body.isMember("autoplay") && body["autoplay"].isBool()) rpc_args["autoplay"] = body["autoplay"].asBool();
    if (body.isMember("muted") && body["muted"].isBool()) rpc_args["muted"] = body["muted"].asBool();
    if (body.isMember("loop") && body["loop"].isBool()) rpc_args["loop"] = body["loop"].asBool();
    if (body.isMember("volume") && (body["volume"].isDouble() || body["volume"].isInt() || body["volume"].isUInt())) {
      const double volume = std::max(0.0, std::min(1.0, body["volume"].asDouble()));
      rpc_args["volume"] = volume;
    }
  }

  Json::Value rpc(Json::objectValue);
  rpc["kind"] = rpc_kind;
  rpc["args"] = rpc_args;

  Json::Value ui_action(Json::objectValue);
  ui_action["type"] = "client_rpc";
  ui_action["title"] =
    title && !title->empty()
      ? *title
      : (action == "play" ? "Voice play" : (action == "pause" ? "Voice pause" : "Voice snapshot"));
  if (message && !message->empty()) ui_action["message"] = *message;
  ui_action["rpc_id"] = rpc_id;
  ui_action["auto_run"] = true;
  ui_action["side_effects"] = action != "snapshot";
  ui_action["rpc"] = rpc;
  ui_action["source"] = "session_voice_control";
  ui_action["voice_action"] = action;

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";

  AgentDb::UiActionRow row;
  row.ts_unix_ms = ts_unix_ms;
  row.session_id = session_id;
  row.tool_call_id = tool_call_id;
  row.type = "client_rpc";
  row.title = ui_action["title"].asString();
  if (ui_action.isMember("message") && ui_action["message"].isString()) row.message = ui_action["message"].asString();
  row.action_json = Json::writeString(wb, ui_action);

  std::string err;
  AgentDb::RunRow run_row;
  run_row.session_id = session_id;
  run_row.job_id = rpc_id;
  run_row.ts_unix_ms = ts_unix_ms;
  run_row.prompt = std::string("[voice_control] ") + action;
  run_row.tools = "host";
  run_row.model = "voice_control";
  run_row.base_url = "/api/v1/session/voice_control";
  run_row.stream_assistant = false;
  run_row.ok = true;
  run_row.stop_reason = "done";
  run_row.steps_executed = 0;
  run_row.tool_calls_total = 0;
  run_row.request_json = req.body;
  run_row.response_json = row.action_json;
  int64_t run_id = 0;
  if (!db->insert_run(run_row, &run_id, &err) || run_id <= 0) {
    resp->status = 500;
    Json::Value out(Json::objectValue);
    out["ok"] = false;
    out["error"] = err.empty() ? "failed to persist voice control run" : err;
    resp->body = json_stringify(out);
    return;
  }
  row.run_id = run_id;
  if (!db->insert_ui_action(row, &err)) {
    resp->status = 500;
    Json::Value out(Json::objectValue);
    out["ok"] = false;
    out["error"] = err.empty() ? "failed to persist voice control action" : err;
    resp->body = json_stringify(out);
    return;
  }

  Json::Value ev(Json::objectValue);
  ev["tool_call_id"] = tool_call_id;
  ev["action"] = ui_action;
  (void)db->insert_event(run_id, ts_unix_ms, "ui_action", Json::writeString(wb, ev), nullptr);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = session_id;
  out["action"] = action;
  out["run_id"] = (Json::Int64)run_id;
  out["rpc_kind"] = rpc_kind;
  out["rpc_id"] = rpc_id;
  out["tool_call_id"] = tool_call_id;
  out["ts_unix_ms"] = (Json::Int64)ts_unix_ms;
  out["pending_client_execution"] = true;
  out["ui_action"] = ui_action;
  resp->status = 200;
  resp->body = json_stringify(out);
}

void handle_session_voice_stats_endpoint(
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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
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
    resp->status = 500;
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
  std::unordered_map<std::string, ClientSeen> seen_clients;
  std::vector<Json::Value> recent_results_vec;
  Json::Value counts(Json::objectValue);
  counts["media_play_ok"] = (Json::UInt64)0;
  counts["media_play_error"] = (Json::UInt64)0;
  counts["media_pause_ok"] = (Json::UInt64)0;
  counts["media_pause_error"] = (Json::UInt64)0;
  counts["media_snapshot_ok"] = (Json::UInt64)0;
  counts["media_snapshot_error"] = (Json::UInt64)0;
  counts["client_rpc_progress"] = (Json::UInt64)0;

  Json::Value latest_result(Json::nullValue);
  Json::Value latest_snapshot(Json::nullValue);
  int64_t latest_result_ts = -1;
  int64_t latest_snapshot_ts = -1;
  uint64_t scanned_events = 0;

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
      scanned_events += 1;

      const auto& c = obj["client"];
      if (c.isObject()) {
        const std::string id = c.isMember("id") && c["id"].isString() ? c["id"].asString() : "";
        if (!id.empty()) {
          const std::string kind = c.isMember("kind") && c["kind"].isString() ? c["kind"].asString() : "";
          const std::string inst = c.isMember("instance_id") && c["instance_id"].isString() ? c["instance_id"].asString() : "";
          int64_t ts = 0;
          if (obj.isMember("ts_unix_ms") && obj["ts_unix_ms"].isInt64()) ts = obj["ts_unix_ms"].asInt64();
          else if (obj.isMember("ts_unix_ms") && obj["ts_unix_ms"].isUInt64()) ts = (int64_t)obj["ts_unix_ms"].asUInt64();
          const std::string et = obj.isMember("type") && obj["type"].isString() ? obj["type"].asString() : "";
          const std::string key = id + "\n" + inst + "\n" + kind;
          auto it = seen_clients.find(key);
          if (it == seen_clients.end() || ts >= it->second.ts_unix_ms) {
            ClientSeen cs;
            cs.id = id;
            cs.kind = kind;
            cs.instance_id = inst;
            cs.ts_unix_ms = ts;
            cs.last_type = et;
            seen_clients[key] = std::move(cs);
          }
        }
      }

      const std::string type = obj.isMember("type") && obj["type"].isString() ? obj["type"].asString() : "";
      const auto& data = obj["data"];
      if (!data.isObject()) continue;

      if (type == "client_rpc_progress") {
        const std::string rpc_kind = data.isMember("rpc_kind") && data["rpc_kind"].isString() ? data["rpc_kind"].asString() : "";
        if (rpc_kind == "media_play" || rpc_kind == "media_pause" || rpc_kind == "media_snapshot") {
          counts["client_rpc_progress"] = (Json::UInt64)(counts["client_rpc_progress"].asUInt64() + 1);
        }
        continue;
      }

      if (type != "client_rpc_result") continue;
      const std::string rpc_kind = data.isMember("rpc_kind") && data["rpc_kind"].isString() ? data["rpc_kind"].asString() : "";
      if (rpc_kind != "media_play" && rpc_kind != "media_pause" && rpc_kind != "media_snapshot") continue;

      const bool ok = data.isMember("ok") && data["ok"].asBool();
      const std::string count_key = rpc_kind + std::string(ok ? "_ok" : "_error");
      counts[count_key] = (Json::UInt64)(counts[count_key].asUInt64() + 1);

      int64_t ts = 0;
      if (obj.isMember("ts_unix_ms") && obj["ts_unix_ms"].isInt64()) ts = obj["ts_unix_ms"].asInt64();
      else if (obj.isMember("ts_unix_ms") && obj["ts_unix_ms"].isUInt64()) ts = (int64_t)obj["ts_unix_ms"].asUInt64();

      Json::Value row(Json::objectValue);
      row["ts_unix_ms"] = (Json::Int64)ts;
      if (data.isMember("rpc_id")) row["rpc_id"] = data["rpc_id"];
      if (data.isMember("request_tool_call_id")) row["request_tool_call_id"] = data["request_tool_call_id"];
      row["rpc_kind"] = rpc_kind;
      row["ok"] = ok;
      if (data.isMember("elapsed_ms")) row["elapsed_ms"] = data["elapsed_ms"];
      if (ok && data.isMember("result")) row["result"] = data["result"];
      if (!ok && data.isMember("error")) row["error"] = data["error"];
      if (obj.isMember("client") && obj["client"].isObject()) row["client"] = obj["client"];
      recent_results_vec.push_back(row);

      if (ts >= latest_result_ts) {
        latest_result_ts = ts;
        latest_result = row;
      }
      if (rpc_kind == "media_snapshot" && ok && ts >= latest_snapshot_ts && data.isMember("result")) {
        latest_snapshot_ts = ts;
        Json::Value snap(Json::objectValue);
        snap["ts_unix_ms"] = (Json::Int64)ts;
        if (data.isMember("rpc_id")) snap["rpc_id"] = data["rpc_id"];
        snap["result"] = data["result"];
        if (obj.isMember("client") && obj["client"].isObject()) snap["client"] = obj["client"];
        latest_snapshot = snap;
      }
    }
  }

  std::sort(recent_results_vec.begin(), recent_results_vec.end(), [](const Json::Value& a, const Json::Value& b) {
    const int64_t ta = a.isMember("ts_unix_ms") && a["ts_unix_ms"].isInt64() ? a["ts_unix_ms"].asInt64() : 0;
    const int64_t tb = b.isMember("ts_unix_ms") && b["ts_unix_ms"].isInt64() ? b["ts_unix_ms"].asInt64() : 0;
    return ta > tb;
  });

  Json::Value clients(Json::arrayValue);
  std::vector<ClientSeen> clients_sorted;
  clients_sorted.reserve(seen_clients.size());
  for (auto& kv : seen_clients) clients_sorted.push_back(std::move(kv.second));
  std::sort(clients_sorted.begin(), clients_sorted.end(), [](const ClientSeen& a, const ClientSeen& b) {
    return a.ts_unix_ms > b.ts_unix_ms;
  });
  for (const auto& c : clients_sorted) {
    Json::Value v(Json::objectValue);
    v["id"] = c.id;
    if (!c.kind.empty()) v["kind"] = c.kind;
    if (!c.instance_id.empty()) v["instance_id"] = c.instance_id;
    v["last_ts_unix_ms"] = (Json::Int64)c.ts_unix_ms;
    if (!c.last_type.empty()) v["last_type"] = c.last_type;
    clients.append(v);
  }

  Json::Value recent_results(Json::arrayValue);
  const size_t recent_limit = std::min<size_t>(recent_results_vec.size(), 20);
  for (size_t i = 0; i < recent_limit; ++i) recent_results.append(recent_results_vec[i]);

  out["scanned_events"] = (Json::UInt64)scanned_events;
  out["clients"] = clients;
  out["client_count"] = (Json::UInt64)clients.size();
  out["counts"] = counts;
  out["recent_results"] = recent_results;
  out["result_count"] = (Json::UInt64)recent_results_vec.size();
  if (!latest_result.isNull()) out["latest_result"] = latest_result;
  if (!latest_snapshot.isNull()) out["latest_snapshot"] = latest_snapshot;
  resp->status = 200;
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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
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

void handle_session_upload_endpoint(
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
    resp->body = json_error_body("db not available");
    return;
  }
  if (cfg.sessions_root_dir.empty()) {
    resp->status = 500;
    resp->body = json_error_body("sessions_root_dir not configured");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string jerr;
  if (!json_parse_object(req.body, &body, &jerr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
    return;
  }

  const std::string session_id = body.isMember("session_id") && body["session_id"].isString() ? body["session_id"].asString() : "";
  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }

  const Json::Value& files = body["files"];
  if (!files.isArray() || files.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing files array");
    return;
  }

  const std::filesystem::path session_root = session_root_path(cfg.sessions_root_dir, session_id);
  if (session_root.empty()) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }
  std::error_code ec;
  (void)std::filesystem::create_directories(session_root / "uploads", ec);

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = session_id;
  Json::Value out_files(Json::arrayValue);
  Json::Value out_errors(Json::arrayValue);

  auto push_error = [&](Json::ArrayIndex index, const std::string& name, const std::string& code, const std::string& msg,
                        size_t bytes, size_t max_bytes) {
    Json::Value err(Json::objectValue);
    err["index"] = (Json::UInt64)index;
    if (!name.empty()) err["name"] = name;
    if (!code.empty()) err["code"] = code;
    if (!msg.empty()) err["error"] = msg;
    if (bytes > 0) err["bytes"] = (Json::UInt64)bytes;
    if (max_bytes > 0) err["max_bytes"] = (Json::UInt64)max_bytes;
    out_errors.append(err);
  };

  const int64_t ts_ms = now_unix_ms();
  size_t accepted = 0;
  const size_t max_bytes = cfg.upload_max_bytes;
  for (Json::ArrayIndex i = 0; i < files.size(); i++) {
    const Json::Value& f = files[i];
    if (!f.isObject()) {
      push_error(i, "", "invalid_entry", "invalid file entry", 0, 0);
      continue;
    }
    const std::string name = f.isMember("name") && f["name"].isString() ? f["name"].asString() : "";
    const std::string mime = f.isMember("mime") && f["mime"].isString() ? f["mime"].asString() : "";
    const std::string data_b64 = f.isMember("data_base64") && f["data_base64"].isString() ? f["data_base64"].asString() : "";
    if (!is_safe_filename_ascii(name)) {
      push_error(i, name, "invalid_name", "invalid file name", 0, 0);
      continue;
    }
    if (data_b64.empty()) {
      push_error(i, name, "missing_data", "missing data_base64", 0, 0);
      continue;
    }

    std::string bytes;
    std::string berr;
    if (!base64_decode(data_b64, &bytes, &berr)) {
      push_error(i, name, "invalid_base64", "invalid base64 data", 0, 0);
      continue;
    }

    // Keep server memory bounded: reject huge uploads in a single request.
    if (max_bytes > 0 && bytes.size() > max_bytes) {
      push_error(i, name, "file_too_large", "file too large", bytes.size(), max_bytes);
      continue;
    }

    const std::string filename = std::to_string(ts_ms) + "_" + std::to_string((unsigned long long)i) + "_" + name;
    const std::filesystem::path dst = (session_root / "uploads" / filename).lexically_normal();
    if (!path_is_within_root(session_root, dst)) {
      push_error(i, name, "invalid_path", "invalid upload path", 0, 0);
      continue;
    }

    std::ofstream os(dst, std::ios::binary);
    if (!os.is_open()) {
      push_error(i, name, "write_failed", "failed to write file", 0, 0);
      continue;
    }
    os.write(bytes.data(), (std::streamsize)bytes.size());
    if (!os) {
      push_error(i, name, "write_failed", "failed to write file", 0, 0);
      continue;
    }

    const std::string rel = (std::filesystem::path("uploads") / filename).generic_string();
    const std::string kind = kind_from_mime(mime);

    // Best-effort mirror into artifacts so the UI can preview via /api/v1/session/artifacts.
    {
      Json::Value artifact(Json::objectValue);
      artifact["path"] = rel;
      if (!kind.empty()) artifact["kind"] = kind;
      if (!mime.empty()) artifact["mime"] = mime;
      if (!name.empty()) artifact["title"] = name;

      Json::StreamWriterBuilder wb;
      wb["indentation"] = "";
      AgentDb::ArtifactRow row;
      row.run_id = 0;
      row.ts_unix_ms = ts_ms;
      row.session_id = session_id;
      row.tool_call_id.clear();
      row.path = rel;
      row.kind = kind;
      row.mime = mime;
      row.title = name;
      row.autoplay = false;
      row.repeat = 1;
      row.artifact_json = Json::writeString(wb, artifact);
      (void)db->insert_artifact(row, nullptr, nullptr);
    }

    Json::Value of(Json::objectValue);
    of["name"] = name;
    if (!mime.empty()) of["mime"] = mime;
    if (!kind.empty()) of["kind"] = kind;
    of["path"] = rel;
    of["bytes"] = (Json::UInt64)bytes.size();
    out_files.append(of);
    accepted++;
  }

  out["files"] = out_files;
  if (!out_errors.empty()) {
    out["errors"] = out_errors;
  }
  if (accepted == 0) {
    out["ok"] = false;
    out["error"] = "no valid files";
    resp->status = 400;
  }
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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto sid = query_get(req.query, "session_id");
  if (!sid || sid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
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
    resp->body = json_error_body("invalid session_id");
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
    resp->body = json_error_body("missing JSON body");
    return;
  }

  Json::Value body(Json::objectValue);
  std::string perr;
  if (!json_parse_object(req.body, &body, &perr)) {
    resp->status = 400;
    resp->body = json_error_body("invalid JSON body");
    return;
  }
  const std::string sid = body.isMember("session_id") && body["session_id"].isString() ? body["session_id"].asString() : "";
  if (sid.empty() || !session_id_is_safe(sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }
  const Json::Value ops = body.isMember("ops") ? body["ops"] : Json::Value(Json::arrayValue);
  if (!ops.isArray()) {
    resp->status = 400;
    resp->body = json_error_body("ops must be an array");
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
    resp->body = json_error_body("missing session_id");
    return;
  }
  if (!session_id_is_safe(*sid)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
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
