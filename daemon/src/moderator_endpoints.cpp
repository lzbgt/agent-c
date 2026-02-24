#include "moderator_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include "session_id_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool is_safe_actor_field(const std::string& s) {
  if (s.empty()) return true;
  if (s.size() > 200) return false;
  for (unsigned char c : s) {
    if (c < 0x20) return false;
  }
  return true;
}

bool parse_actor(const Json::Value& body, Json::Value* out_actor, std::string* out_err) {
  if (!out_actor) return false;
  std::string id;
  std::string kind;
  std::string instance_id;

  if (body.isMember("actor") && body["actor"].isObject()) {
    const auto& a = body["actor"];
    if (a.isMember("id") && a["id"].isString()) id = a["id"].asString();
    if (a.isMember("kind") && a["kind"].isString()) kind = a["kind"].asString();
    if (a.isMember("instance_id") && a["instance_id"].isString()) instance_id = a["instance_id"].asString();
  }

  if (id.empty() && body.isMember("client") && body["client"].isObject()) {
    const auto& c = body["client"];
    if (c.isMember("id") && c["id"].isString()) id = c["id"].asString();
    if (c.isMember("kind") && c["kind"].isString()) kind = c["kind"].asString();
    if (c.isMember("instance_id") && c["instance_id"].isString()) instance_id = c["instance_id"].asString();
  }
  if (id.empty() && body.isMember("client_id") && body["client_id"].isString()) id = body["client_id"].asString();
  if (kind.empty() && body.isMember("client_kind") && body["client_kind"].isString()) kind = body["client_kind"].asString();
  if (instance_id.empty() && body.isMember("client_instance_id") && body["client_instance_id"].isString()) {
    instance_id = body["client_instance_id"].asString();
  }

  if (!is_safe_actor_field(id) || !is_safe_actor_field(kind) || !is_safe_actor_field(instance_id)) {
    if (out_err) *out_err = "invalid actor identity";
    return false;
  }

  if (id.empty()) id = "moderator";

  Json::Value actor(Json::objectValue);
  actor["role"] = "moderator";
  if (!id.empty()) actor["id"] = id;
  if (!kind.empty()) actor["kind"] = kind;
  if (!instance_id.empty()) actor["instance_id"] = instance_id;
  *out_actor = actor;
  return true;
}

bool append_session_message(
  AgentDb* db,
  const std::string& session_id,
  int64_t ts_unix_ms,
  const std::string& label,
  const std::string& content,
  std::string* out_error
) {
  if (!db) return false;
  std::vector<AgentDb::MessageRow> msgs;
  std::string err;
  (void)db->load_session_messages(session_id, &msgs, &err);

  std::string msg = "[" + label + "] " + content;
  if (msg.size() > 4096) {
    msg.resize(4096);
    msg += "…";
  }
  AgentDb::MessageRow row;
  row.role = "user";
  row.content = msg;
  msgs.emplace_back(std::move(row));

  err.clear();
  if (!db->replace_session_messages(session_id, msgs, ts_unix_ms, &err)) {
    if (out_error) *out_error = err.empty() ? "failed to append to session" : err;
    return false;
  }
  return true;
}

bool insert_client_event(
  AgentDb* db,
  const std::string& session_id,
  int64_t ts_unix_ms,
  const std::string& type,
  const Json::Value& payload,
  std::string* out_error
) {
  if (!db) return false;
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string payload_json = Json::writeString(wb, payload);

  AgentDb::ClientEventRow er;
  er.ts_unix_ms = ts_unix_ms;
  er.session_id = session_id;
  er.type = type;
  er.data_json = payload_json;
  return db->insert_client_event(er, out_error);
}

bool parse_ts_unix_ms(const Json::Value& body, int64_t* out_ts) {
  if (!out_ts) return false;
  if (body.isMember("ts_unix_ms") && body["ts_unix_ms"].isInt64()) {
    *out_ts = body["ts_unix_ms"].asInt64();
    return true;
  }
  if (body.isMember("ts_unix_ms") && body["ts_unix_ms"].isUInt64()) {
    *out_ts = (int64_t)body["ts_unix_ms"].asUInt64();
    return true;
  }
  return false;
}

std::vector<std::string> split_csv(const std::string& raw) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : raw) {
    if (c == ',') {
      const std::string trimmed = trim_copy(cur);
      if (!trimmed.empty()) out.push_back(trimmed);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  const std::string trimmed = trim_copy(cur);
  if (!trimmed.empty()) out.push_back(trimmed);
  return out;
}

}  // namespace

void handle_moderator_directive_endpoint(
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
  const std::string directive = body.isMember("directive") && body["directive"].isString() ? body["directive"].asString() : "";
  const std::string scope = body.isMember("scope") && body["scope"].isString() ? body["scope"].asString() : "";
  const bool append_to_session =
    body.isMember("append_to_session") && body["append_to_session"].isBool() ? body["append_to_session"].asBool() : false;

  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }
  if (directive.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing directive");
    return;
  }
  if (directive.size() > 16384) {
    resp->status = 400;
    resp->body = json_error_body("directive too large");
    return;
  }

  int64_t ts_unix_ms = 0;
  if (!parse_ts_unix_ms(body, &ts_unix_ms)) ts_unix_ms = now_unix_ms();

  Json::Value actor(Json::objectValue);
  std::string actor_err;
  if (!parse_actor(body, &actor, &actor_err)) {
    resp->status = 400;
    resp->body = json_error_body(actor_err.empty() ? "invalid actor" : actor_err);
    return;
  }

  Json::Value data(Json::objectValue);
  data["directive"] = directive;
  if (!scope.empty()) data["scope"] = scope;
  if (body.isMember("priority") && (body["priority"].isInt64() || body["priority"].isUInt64() || body["priority"].isInt())) {
    data["priority"] = body["priority"];
  }
  if (body.isMember("metadata") && body["metadata"].isObject()) {
    data["metadata"] = body["metadata"];
  }

  Json::Value payload(Json::objectValue);
  payload["type"] = "moderator_directive";
  payload["ts_unix_ms"] = (Json::Int64)ts_unix_ms;
  payload["actor"] = actor;
  payload["data"] = data;

  std::string err;
  if (!insert_client_event(db, session_id, ts_unix_ms, "moderator_directive", payload, &err)) {
    resp->status = 500;
    resp->body = json_error_body(err.empty() ? "failed to store moderator directive" : err);
    return;
  }

  bool appended = false;
  if (append_to_session) {
    if (!append_session_message(db, session_id, ts_unix_ms, "moderator", directive, &err)) {
      resp->status = 500;
      resp->body = json_error_body(err.empty() ? "failed to append directive" : err);
      return;
    }
    appended = true;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = session_id;
  out["type"] = "moderator_directive";
  out["appended_to_session"] = appended;
  out["logged_to_client_events"] = true;
  out["event"] = payload;
  resp->status = 200;
  resp->body = json_stringify(out);
}

void handle_moderator_task_endpoint(
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
  const std::string title = body.isMember("title") && body["title"].isString() ? body["title"].asString() : "";
  const std::string detail = body.isMember("detail") && body["detail"].isString() ? body["detail"].asString() : "";
  const bool append_to_session =
    body.isMember("append_to_session") && body["append_to_session"].isBool() ? body["append_to_session"].asBool() : false;

  if (!session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = json_error_body("invalid session_id");
    return;
  }
  if (title.empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing title");
    return;
  }
  if (title.size() > 2048) {
    resp->status = 400;
    resp->body = json_error_body("title too large");
    return;
  }

  int64_t ts_unix_ms = 0;
  if (!parse_ts_unix_ms(body, &ts_unix_ms)) ts_unix_ms = now_unix_ms();

  Json::Value actor(Json::objectValue);
  std::string actor_err;
  if (!parse_actor(body, &actor, &actor_err)) {
    resp->status = 400;
    resp->body = json_error_body(actor_err.empty() ? "invalid actor" : actor_err);
    return;
  }

  Json::Value task(Json::objectValue);
  task["title"] = title;
  if (!detail.empty()) task["detail"] = detail;
  if (body.isMember("priority") && (body["priority"].isInt64() || body["priority"].isUInt64() || body["priority"].isInt())) {
    task["priority"] = body["priority"];
  }
  if (body.isMember("tags") && body["tags"].isArray()) {
    Json::Value tags(Json::arrayValue);
    for (const auto& t : body["tags"]) {
      if (t.isString()) tags.append(t.asString());
    }
    if (!tags.empty()) task["tags"] = tags;
  }
  if (body.isMember("assignees") && body["assignees"].isArray()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& t : body["assignees"]) {
      if (t.isString()) arr.append(t.asString());
    }
    if (!arr.empty()) task["assignees"] = arr;
  }
  if (body.isMember("metadata") && body["metadata"].isObject()) {
    task["metadata"] = body["metadata"];
  }
  task["status"] = body.isMember("status") && body["status"].isString() ? body["status"].asString() : "open";

  Json::Value data(Json::objectValue);
  data["task"] = task;

  Json::Value payload(Json::objectValue);
  payload["type"] = "moderator_task_published";
  payload["ts_unix_ms"] = (Json::Int64)ts_unix_ms;
  payload["actor"] = actor;
  payload["data"] = data;

  std::string err;
  if (!insert_client_event(db, session_id, ts_unix_ms, "moderator_task_published", payload, &err)) {
    resp->status = 500;
    resp->body = json_error_body(err.empty() ? "failed to store moderator task" : err);
    return;
  }

  bool appended = false;
  if (append_to_session) {
    if (!append_session_message(db, session_id, ts_unix_ms, "moderator_task", title, &err)) {
      resp->status = 500;
      resp->body = json_error_body(err.empty() ? "failed to append task" : err);
      return;
    }
    appended = true;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["session_id"] = session_id;
  out["type"] = "moderator_task_published";
  out["appended_to_session"] = appended;
  out["logged_to_client_events"] = true;
  out["event"] = payload;
  resp->status = 200;
  resp->body = json_stringify(out);
}

void handle_moderator_events_endpoint(
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

  std::unordered_set<std::string> want = {"moderator_directive", "moderator_task_published"};
  if (const auto types = query_get(req.query, "types")) {
    want.clear();
    for (const auto& t : split_csv(*types)) {
      want.insert(t);
    }
  }

  std::string tail;
  std::string err;
  const bool ok_tail = db->read_client_events_tail_jsonl(*sid, max_bytes, /*max_events=*/0, &tail, &err);

  Json::Value out(Json::objectValue);
  out["ok"] = ok_tail;
  out["session_id"] = *sid;
  out["max_bytes"] = (Json::UInt64)max_bytes;
  if (!ok_tail) {
    out["error"] = err.empty() ? "failed to read moderator events" : err;
  }

  Json::Value events(Json::arrayValue);
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
      const auto& t = obj["type"];
      if (!t.isString()) continue;
      if (!want.empty() && want.find(t.asString()) == want.end()) continue;
      events.append(obj);
    }
  }

  out["count"] = (Json::UInt64)events.size();
  out["events"] = events;
  resp->body = json_stringify(out);
}

}  // namespace agentd
