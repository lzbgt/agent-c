#include "edge_interop_endpoints.h"

#include "daemon_auth.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace agentd {
namespace {

static int64_t unix_ms_now() {
  return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

static bool id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':';
    if (!ok) return false;
  }
  return true;
}

static std::string make_uuidish_msg_id() {
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);
  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);
  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);
  char buf[96];
  (void)snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
}

static bool parse_string_array(const std::string& json, std::unordered_set<std::string>* out) {
  if (!out) return false;
  out->clear();
  if (json.empty()) return true;
  Json::Value v;
  std::string err;
  if (!json_parse_any(json, &v, &err) || !v.isArray()) return false;
  for (Json::ArrayIndex i = 0; i < v.size(); i++) {
    if (v[i].isString()) out->insert(v[i].asString());
  }
  return true;
}

static void manifest_extract_best_effort(
  const Json::Value& manifest,
  std::string* out_tags_json,
  std::string* out_tools_json,
  std::string* out_hw_presence_json
) {
  if (out_tags_json) *out_tags_json = "[]";
  if (out_tools_json) *out_tools_json = "[]";
  if (out_hw_presence_json) *out_hw_presence_json = "{}";
  if (!manifest.isObject()) return;

  if (out_tags_json) {
    const auto& tags = manifest["tags"];
    if (tags.isArray()) *out_tags_json = json_stringify_compact(tags);
  }

  if (out_tools_json) {
    Json::Value arr(Json::arrayValue);
    const auto& tools = manifest["tools"];
    if (tools.isArray()) {
      for (Json::ArrayIndex i = 0; i < tools.size(); i++) {
        const auto& t = tools[i];
        if (!t.isObject()) continue;
        const auto& n = t["name"];
        if (n.isString()) arr.append(n.asString());
      }
    }
    *out_tools_json = json_stringify_compact(arr);
  }

  if (out_hw_presence_json) {
    const auto& hw = manifest["hardware"];
    if (hw.isObject()) {
      const auto& pres = hw["presence"];
      if (pres.isObject()) *out_hw_presence_json = json_stringify_compact(pres);
    }
  }
}

static std::string node_to_prefix(const std::string& node_id) {
  if (node_id.empty()) return "";
  if (node_id.rfind("node:", 0) == 0) return node_id;
  return std::string("node:") + node_id;
}

static bool select_node_match_any(
  AgentDb* db,
  const std::vector<std::string>& requires_tools,
  const std::vector<std::string>& tags_all,
  const std::vector<std::string>& tags_any,
  const std::vector<std::string>& tags_none,
  std::string* out_node_id
) {
  if (out_node_id) out_node_id->clear();
  if (!db || !db->is_open() || !out_node_id) return false;

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db->list_edge_nodes(/*max_rows=*/256, &nodes, &err)) return false;

  for (const auto& n : nodes) {
    if (n.node_id.empty()) continue;

    std::unordered_set<std::string> toolset;
    std::unordered_set<std::string> tagset;
    if (!parse_string_array(n.tools_json, &toolset)) {
      // If node's tools list can't be parsed, treat as unknown => skip for strict matching.
      continue;
    }
    if (!parse_string_array(n.tags_json, &tagset)) {
      continue;
    }

    bool ok = true;
    for (const auto& t : requires_tools) {
      if (t.empty()) continue;
      if (!toolset.count(t)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    for (const auto& tag : tags_all) {
      if (tag.empty()) continue;
      if (!tagset.count(tag)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    if (!tags_any.empty()) {
      bool any = false;
      for (const auto& tag : tags_any) {
        if (tag.empty()) continue;
        if (tagset.count(tag)) {
          any = true;
          break;
        }
      }
      if (!any) continue;
    }

    for (const auto& tag : tags_none) {
      if (tag.empty()) continue;
      if (tagset.count(tag)) {
        ok = false;
        break;
      }
    }
    if (!ok) continue;

    *out_node_id = n.node_id;
    return true;
  }

  return false;
}

}  // namespace

void handle_edge_message_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value env;
  std::string perr;
  if (!json_parse_object(req.body, &env, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  const std::string msg_id = env.isMember("msg_id") && env["msg_id"].isString() ? trim_copy(env["msg_id"].asString()) : "";
  const int64_t ts_utc_ms = env.isMember("ts_utc_ms") && env["ts_utc_ms"].isInt64() ? env["ts_utc_ms"].asInt64()
    : (env.isMember("ts_utc_ms") && env["ts_utc_ms"].isUInt64() ? (int64_t)env["ts_utc_ms"].asUInt64() : 0);
  const std::string type = env.isMember("type") && env["type"].isString() ? trim_copy(env["type"].asString()) : "";
  const std::string from_id = env.isMember("from") && env["from"].isString() ? trim_copy(env["from"].asString()) : "";
  std::string to_id;
  if (env.isMember("to")) {
    if (env["to"].isString()) to_id = trim_copy(env["to"].asString());
    else if (env["to"].isNull()) to_id.clear();
    else {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid envelope.to (expected string|null)\"}";
      return;
    }
  }
  const Json::Value body = env.isMember("body") ? env["body"] : Json::Value(Json::nullValue);

  if (msg_id.empty() || type.empty() || !body.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope (missing msg_id/type/body)\"}";
    return;
  }
  if (!id_is_safe(type) || msg_id.size() > 128) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid envelope msg_id/type\"}";
    return;
  }

  // Persist inbound (dedupe by msg_id).
  {
    AgentDb::EdgeInboxMessageRow ir;
    ir.msg_id = msg_id;
    ir.ts_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : unix_ms_now();
    ir.type = type;
    ir.from_id = from_id;
    ir.to_id = to_id;
    ir.envelope_json = json_stringify_compact(env);
    std::string derr;
    if (!db_or_null->insert_edge_inbox_message(ir, &derr)) {
      // Treat duplicates as OK (idempotent ingest).
      if (derr.find("UNIQUE") != std::string::npos || derr.find("constraint") != std::string::npos) {
        resp->body = "{\"ok\":true,\"deduped\":true}";
        return;
      }
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist inbound message";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
  }

  const int64_t now = unix_ms_now();
  auto queue_caps_req = [&](const std::string& node_id) {
    if (node_id.empty()) return;
    Json::Value oenv(Json::objectValue);
    oenv["msg_id"] = make_uuidish_msg_id();
    oenv["ts_utc_ms"] = (Json::Int64)now;
    oenv["type"] = "PLATFORM_CAPS_REQ";
    oenv["from"] = "platform";
    oenv["to"] = node_to_prefix(node_id);
    Json::Value b(Json::objectValue);
    b["node_id"] = node_id;
    b["want"] = "full";
    oenv["body"] = b;
    AgentDb::EdgeOutboxMessageRow orow;
    orow.node_id = node_id;
    orow.ts_utc_ms = now;
    orow.envelope_json = json_stringify_compact(oenv);
    (void)db_or_null->insert_edge_outbox_message(orow, nullptr, nullptr);
  };

  if (type == "NODE_HELLO") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string model = body.isMember("model") && body["model"].isString() ? body["model"].asString() : "";
    const std::string fw_git_sha = body.isMember("fw_git_sha") && body["fw_git_sha"].isString() ? body["fw_git_sha"].asString() : "";
    const std::string caps_sha = body.isMember("caps_sha256") && body["caps_sha256"].isString() ? body["caps_sha256"].asString() : "";
    if (node_id.empty() || !id_is_safe(node_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid node_id\"}";
      return;
    }

    AgentDb::EdgeNodeRow prev;
    std::string gerr;
    const bool have = db_or_null->get_edge_node(node_id, &prev, &gerr);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    nr.model = model;
    nr.fw_git_sha = fw_git_sha;
    nr.caps_sha256 = caps_sha;
    nr.last_hello_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    (void)db_or_null->upsert_edge_node(nr, &uerr);

    const bool need_caps =
      !have || prev.manifest_json.empty() || (!caps_sha.empty() && prev.caps_sha256 != caps_sha);
    if (need_caps) queue_caps_req(node_id);

    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "NODE_HEARTBEAT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string caps_sha = body.isMember("caps_sha256") && body["caps_sha256"].isString() ? body["caps_sha256"].asString() : "";
    const Json::Value health = body.isMember("health") ? body["health"] : Json::Value(Json::nullValue);
    if (node_id.empty() || !id_is_safe(node_id)) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid node_id\"}";
      return;
    }

    AgentDb::EdgeNodeRow prev;
    std::string gerr;
    const bool have = db_or_null->get_edge_node(node_id, &prev, &gerr);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    nr.caps_sha256 = caps_sha;
    if (health.isObject()) nr.health_json = json_stringify_compact(health);
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    (void)db_or_null->upsert_edge_node(nr, &uerr);

    const bool need_caps =
      !have || prev.manifest_json.empty() || (!caps_sha.empty() && prev.caps_sha256 != caps_sha);
    if (need_caps) queue_caps_req(node_id);

    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "NODE_CAPS_RSP") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const Json::Value manifest = body.isMember("manifest") ? body["manifest"] : Json::Value(Json::nullValue);
    if (node_id.empty() || !id_is_safe(node_id) || !manifest.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid NODE_CAPS_RSP body\"}";
      return;
    }

    std::string tags_json, tools_json, hw_json;
    manifest_extract_best_effort(manifest, &tags_json, &tools_json, &hw_json);

    AgentDb::EdgeNodeRow nr;
    nr.node_id = node_id;
    if (manifest.isMember("caps_sha256") && manifest["caps_sha256"].isString()) {
      nr.caps_sha256 = manifest["caps_sha256"].asString();
    }
    nr.manifest_json = json_stringify_compact(manifest);
    nr.tags_json = tags_json;
    nr.tools_json = tools_json;
    nr.hardware_presence_json = hw_json;
    nr.last_heartbeat_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    std::string uerr;
    if (!db_or_null->upsert_edge_node(nr, &uerr)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist node manifest";
      o["detail"] = uerr;
      resp->body = json_stringify_compact(o);
      return;
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  auto update_task_state = [&](const std::string& task_id, const std::string& step_id, const std::string& state,
                               const std::string& result_json, const std::string& error_text, const Json::Value& event_data) {
    if (task_id.empty() || step_id.empty() || state.empty()) return;
    AgentDb::EdgeTaskRow tr;
    std::string terr;
    if (!db_or_null->get_edge_task(task_id, step_id, &tr, &terr)) {
      // Unknown task; ignore to keep ingestion robust.
      return;
    }
    tr.state = state;
    tr.updated_utc_ms = ts_utc_ms > 0 ? ts_utc_ms : now;
    if (!result_json.empty()) tr.result_json = result_json;
    if (!error_text.empty()) tr.error = error_text;
    (void)db_or_null->upsert_edge_task(tr, nullptr);
    AgentDb::EdgeTaskEventRow ev;
    ev.task_id = task_id;
    ev.step_id = step_id;
    ev.ts_utc_ms = tr.updated_utc_ms;
    ev.state = state;
    ev.data_json = json_stringify_compact(event_data.isNull() ? Json::Value(Json::objectValue) : event_data);
    (void)db_or_null->insert_edge_task_event(ev, nullptr, nullptr);
  };

  if (type == "TASK_ACK") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const bool accepted = body.isMember("accepted") && body["accepted"].isBool() ? body["accepted"].asBool() : false;
    std::string reason;
    if (body.isMember("reason") && body["reason"].isString()) reason = body["reason"].asString();
    Json::Value d = body;
    if (accepted) {
      update_task_state(task_id, step_id, "QUEUED", /*result_json=*/"", /*error_text=*/"", d);
    } else {
      update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", reason.empty() ? "rejected" : reason, d);
    }
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_EVENT") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    const std::string state = body.isMember("state") && body["state"].isString() ? body["state"].asString() : "";
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    std::string result_json;
    if (body.isMember("result")) result_json = json_stringify_compact(body["result"]);
    update_task_state(task_id, step_id, state, result_json, error, body);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_DONE") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    std::string result_json;
    if (body.isMember("result")) result_json = json_stringify_compact(body["result"]);
    update_task_state(task_id, step_id, "SUCCEEDED", result_json, /*error_text=*/"", body);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "TASK_FAILED") {
    const std::string task_id = body.isMember("task_id") && body["task_id"].isString() ? body["task_id"].asString() : "";
    const std::string step_id = body.isMember("step_id") && body["step_id"].isString() ? body["step_id"].asString() : "";
    std::string error;
    if (body.isMember("error") && body["error"].isString()) error = body["error"].asString();
    update_task_state(task_id, step_id, "FAILED", /*result_json=*/"", error.empty() ? "failed" : error, body);
    resp->body = "{\"ok\":true}";
    return;
  }

  if (type == "SENSOR_EVENT") {
    const std::string node_id = body.isMember("node_id") && body["node_id"].isString() ? trim_copy(body["node_id"].asString()) : "";
    const std::string event_type = body.isMember("event_type") && body["event_type"].isString() ? body["event_type"].asString() : "";
    const int64_t ts2 = body.isMember("ts_utc_ms") && (body["ts_utc_ms"].isInt64() || body["ts_utc_ms"].isUInt64())
      ? (body["ts_utc_ms"].isInt64() ? body["ts_utc_ms"].asInt64() : (int64_t)body["ts_utc_ms"].asUInt64())
      : (ts_utc_ms > 0 ? ts_utc_ms : now);
    const double confidence = body.isMember("confidence") && (body["confidence"].isDouble() || body["confidence"].isInt())
      ? body["confidence"].asDouble()
      : 0.0;
    Json::Value data = body.isMember("data") ? body["data"] : Json::Value(Json::objectValue);
    if (node_id.empty() || !id_is_safe(node_id) || event_type.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid SENSOR_EVENT body\"}";
      return;
    }
    AgentDb::EdgeSensorEventRow sr;
    sr.node_id = node_id;
    sr.event_type = event_type;
    sr.ts_utc_ms = ts2;
    sr.confidence = confidence;
    sr.data_json = json_stringify_compact(data.isNull() ? Json::Value(Json::objectValue) : data);
    (void)db_or_null->insert_edge_sensor_event(sr, nullptr, nullptr);
    resp->body = "{\"ok\":true}";
    return;
  }

  // Unknown message types are accepted and persisted (forward compatible).
  resp->body = "{\"ok\":true}";
}

void handle_edge_outbox_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  int64_t cursor = 0;
  const auto c = query_get(req.query, "cursor");
  if (c && !c->empty()) {
    try { cursor = (int64_t)std::stoll(*c); } catch (...) { cursor = 0; }
  }
  if (cursor < 0) cursor = 0;

  size_t limit = 256;
  const auto l = query_get(req.query, "limit");
  if (l && !l->empty()) {
    try { limit = (size_t)std::stoull(*l); } catch (...) { limit = 256; }
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 2048));

  std::vector<AgentDb::EdgeOutboxMessageRow> msgs;
  std::string err;
  if (!db_or_null->list_edge_outbox_messages(*nid, cursor, limit, &msgs, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list outbox";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["cursor_base"] = (Json::Int64)cursor;
  Json::Value arr(Json::arrayValue);
  int64_t cursor_next = cursor;
  for (const auto& m : msgs) {
    Json::Value row(Json::objectValue);
    row["outbox_id"] = (Json::Int64)m.outbox_id;
    row["ts_utc_ms"] = (Json::Int64)m.ts_utc_ms;
    Json::Value env;
    std::string perr;
    if (json_parse_any(m.envelope_json, &env, &perr) && env.isObject()) {
      row["msg"] = env;
    } else {
      row["msg_raw"] = m.envelope_json;
      row["parse_error"] = perr;
    }
    arr.append(row);
    cursor_next = std::max(cursor_next, m.outbox_id);
  }
  o["messages"] = arr;
  o["cursor_next"] = (Json::Int64)cursor_next;
  resp->body = json_stringify_compact(o);
}

void handle_edge_nodes_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 200));

  std::vector<AgentDb::EdgeNodeRow> nodes;
  std::string err;
  if (!db_or_null->list_edge_nodes(limit, &nodes, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list nodes";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value arr(Json::arrayValue);
  for (const auto& n : nodes) {
    Json::Value row(Json::objectValue);
    row["node_id"] = n.node_id;
    if (!n.model.empty()) row["model"] = n.model;
    if (!n.fw_git_sha.empty()) row["fw_git_sha"] = n.fw_git_sha;
    if (!n.caps_sha256.empty()) row["caps_sha256"] = n.caps_sha256;
    row["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
    row["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
    arr.append(row);
  }
  o["nodes"] = arr;
  resp->body = json_stringify_compact(o);
}

void handle_edge_node_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value row(Json::objectValue);
  row["node_id"] = n.node_id;
  if (!n.model.empty()) row["model"] = n.model;
  if (!n.fw_git_sha.empty()) row["fw_git_sha"] = n.fw_git_sha;
  if (!n.caps_sha256.empty()) row["caps_sha256"] = n.caps_sha256;
  row["last_hello_utc_ms"] = (Json::Int64)n.last_hello_utc_ms;
  row["last_heartbeat_utc_ms"] = (Json::Int64)n.last_heartbeat_utc_ms;
  if (!n.tags_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.tags_json, &v, &perr) && v.isArray()) row["tags"] = v;
  }
  if (!n.tools_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.tools_json, &v, &perr) && v.isArray()) row["tools"] = v;
  }
  if (!n.hardware_presence_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.hardware_presence_json, &v, &perr) && v.isObject()) row["hardware_presence"] = v;
  }
  if (!n.health_json.empty()) {
    Json::Value v;
    std::string perr;
    if (json_parse_any(n.health_json, &v, &perr) && v.isObject()) row["health"] = v;
  }
  row["has_manifest"] = !n.manifest_json.empty();
  o["node"] = row;
  resp->body = json_stringify_compact(o);
}

void handle_edge_node_caps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto nid = query_get(req.query, "node_id");
  if (!nid || nid->empty() || !id_is_safe(*nid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id\"}";
    return;
  }

  AgentDb::EdgeNodeRow n;
  std::string err;
  if (!db_or_null->get_edge_node(*nid, &n, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node not found\"}";
    return;
  }
  if (n.manifest_json.empty()) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"node has no manifest\"}";
    return;
  }

  Json::Value m;
  std::string perr;
  if (!json_parse_any(n.manifest_json, &m, &perr) || !m.isObject()) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to parse stored manifest";
    o["parse_error"] = perr;
    resp->body = json_stringify_compact(o);
    return;
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = *nid;
  o["manifest"] = m;
  resp->body = json_stringify_compact(o);
}

void handle_edge_task_assign_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  Json::Value args;
  std::string perr;
  if (!json_parse_object(req.body, &args, &perr)) {
    resp->status = 400;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = std::string("invalid JSON: ") + perr;
    resp->body = json_stringify_compact(o);
    return;
  }

  std::string node_id = args.isMember("node_id") && args["node_id"].isString() ? trim_copy(args["node_id"].asString()) : "";
  std::vector<std::string> requires_tools;
  std::vector<std::string> tags_all;
  std::vector<std::string> tags_any;
  std::vector<std::string> tags_none;
  if (node_id.empty() && args.isMember("match_any") && args["match_any"].isObject()) {
    const auto& m = args["match_any"];
    auto read_arr = [&](const char* k, std::vector<std::string>* out) {
      if (!out) return;
      out->clear();
      if (!m.isMember(k) || !m[k].isArray()) return;
      for (Json::ArrayIndex i = 0; i < m[k].size(); i++) {
        if (!m[k][i].isString()) continue;
        out->push_back(m[k][i].asString());
      }
    };
    read_arr("requires_tools", &requires_tools);
    read_arr("tags_all", &tags_all);
    read_arr("tags_any", &tags_any);
    read_arr("tags_none", &tags_none);

    if (!select_node_match_any(db_or_null, requires_tools, tags_all, tags_any, tags_none, &node_id)) {
      resp->status = 409;
      resp->body = "{\"ok\":false,\"error\":\"no matching node\"}";
      return;
    }
  }
  if (node_id.empty() || !id_is_safe(node_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid node_id (or match_any did not select)\"}";
    return;
  }

  const std::string task_id = args.isMember("task_id") && args["task_id"].isString() ? args["task_id"].asString() : "";
  const std::string step_id = args.isMember("step_id") && args["step_id"].isString() ? args["step_id"].asString() : "";
  const std::string idempotency_key =
    args.isMember("idempotency_key") && args["idempotency_key"].isString() ? args["idempotency_key"].asString() : "";
  const std::string mode = args.isMember("mode") && args["mode"].isString() ? args["mode"].asString() : "";
  const int64_t deadline_utc_ms = args.isMember("deadline_utc_ms") && (args["deadline_utc_ms"].isInt64() || args["deadline_utc_ms"].isUInt64())
    ? (args["deadline_utc_ms"].isInt64() ? args["deadline_utc_ms"].asInt64() : (int64_t)args["deadline_utc_ms"].asUInt64())
    : 0;
  const Json::Value payload = args.isMember("payload") ? args["payload"] : Json::Value(Json::nullValue);

  if (task_id.empty() || step_id.empty() || idempotency_key.empty() || (mode != "invoke" && mode != "agent") || deadline_utc_ms <= 0 ||
      !payload.isObject()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid task fields\"}";
    return;
  }

  // Dedupe on (node_id, idempotency_key).
  AgentDb::EdgeTaskRow existing;
  std::string err;
  const bool has_existing = db_or_null->get_edge_task_by_node_idempotency(node_id, idempotency_key, &existing, &err);
  if (has_existing) {
    if (existing.task_id != task_id || existing.step_id != step_id) {
      resp->status = 409;
      resp->body = "{\"ok\":false,\"error\":\"idempotency_key collision with different task_id/step_id\"}";
      return;
    }
  } else {
    AgentDb::EdgeTaskRow tr;
    tr.task_id = task_id;
    tr.step_id = step_id;
    tr.node_id = node_id;
    tr.idempotency_key = idempotency_key;
    tr.mode = mode;
    tr.deadline_utc_ms = deadline_utc_ms;
    tr.payload_json = json_stringify_compact(payload);
    tr.state = "QUEUED";
    tr.created_utc_ms = unix_ms_now();
    tr.updated_utc_ms = tr.created_utc_ms;
    if (!db_or_null->upsert_edge_task(tr, &err)) {
      resp->status = 500;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "failed to persist edge task";
      o["detail"] = err;
      resp->body = json_stringify_compact(o);
      return;
    }

    AgentDb::EdgeTaskEventRow ev;
    ev.task_id = task_id;
    ev.step_id = step_id;
    ev.ts_utc_ms = tr.created_utc_ms;
    ev.state = "QUEUED";
    Json::Value d(Json::objectValue);
    d["task_id"] = task_id;
    d["step_id"] = step_id;
    d["node_id"] = node_id;
    d["state"] = "QUEUED";
    d["mode"] = mode;
    d["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
    ev.data_json = json_stringify_compact(d);
    (void)db_or_null->insert_edge_task_event(ev, nullptr, nullptr);
  }

  // Enqueue UM‑BMP TASK_ASSIGN envelope to node outbox.
  const int64_t now = unix_ms_now();
  Json::Value env(Json::objectValue);
  env["msg_id"] = make_uuidish_msg_id();
  env["ts_utc_ms"] = (Json::Int64)now;
  env["type"] = "TASK_ASSIGN";
  env["from"] = "platform";
  env["to"] = node_to_prefix(node_id);
  Json::Value b(Json::objectValue);
  b["task_id"] = task_id;
  b["step_id"] = step_id;
  b["idempotency_key"] = idempotency_key;
  b["mode"] = mode;
  b["deadline_utc_ms"] = (Json::Int64)deadline_utc_ms;
  b["payload"] = payload;
  env["body"] = b;

  AgentDb::EdgeOutboxMessageRow orow;
  orow.node_id = node_id;
  orow.ts_utc_ms = now;
  orow.envelope_json = json_stringify_compact(env);
  int64_t outbox_id = 0;
  if (!db_or_null->insert_edge_outbox_message(orow, &outbox_id, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to enqueue outbox message";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["node_id"] = node_id;
  o["task_id"] = task_id;
  o["step_id"] = step_id;
  o["outbox_id"] = (Json::Int64)outbox_id;
  resp->body = json_stringify_compact(o);
}

void handle_edge_task_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 503;
    resp->body = "{\"ok\":false,\"error\":\"db not available\"}";
    return;
  }

  const auto tid = query_get(req.query, "task_id");
  const auto sid = query_get(req.query, "step_id");
  if (!tid || tid->empty() || !sid || sid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing task_id/step_id\"}";
    return;
  }

  AgentDb::EdgeTaskRow tr;
  std::string err;
  if (!db_or_null->get_edge_task(*tid, *sid, &tr, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"task not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value t(Json::objectValue);
  t["task_id"] = tr.task_id;
  t["step_id"] = tr.step_id;
  t["node_id"] = tr.node_id;
  t["idempotency_key"] = tr.idempotency_key;
  t["mode"] = tr.mode;
  t["deadline_utc_ms"] = (Json::Int64)tr.deadline_utc_ms;
  t["state"] = tr.state;
  t["created_utc_ms"] = (Json::Int64)tr.created_utc_ms;
  t["updated_utc_ms"] = (Json::Int64)tr.updated_utc_ms;
  if (!tr.error.empty()) t["error"] = tr.error;
  if (!tr.payload_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.payload_json, &v, &perr2) && v.isObject()) t["payload"] = v;
  }
  if (!tr.result_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(tr.result_json, &v, &perr2)) t["result"] = v;
  }
  o["task"] = t;
  resp->body = json_stringify_compact(o);
}

}  // namespace agentd

