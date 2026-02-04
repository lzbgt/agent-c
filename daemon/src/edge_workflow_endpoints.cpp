#include "edge_interop_endpoints.h"

#include "daemon_auth.h"
#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <climits>
#include <vector>

namespace agentd {

void handle_edge_workflow_submit_endpoint(
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
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  std::string workflow_id = args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty()) workflow_id = std::string("wf:") + edge_make_uuidish_msg_id();
  if (!edge_id_is_safe(workflow_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid workflow_id\"}";
    return;
  }

  std::string goal = args.isMember("goal") && args["goal"].isString() ? args["goal"].asString() : "";
  int priority = 0;
  if (args.isMember("priority") && (args["priority"].isInt() || args["priority"].isUInt())) {
    priority = args["priority"].isInt() ? args["priority"].asInt() : (int)std::min((Json::UInt)INT32_MAX, args["priority"].asUInt());
  }

  if (!args.isMember("steps") || !args["steps"].isArray() || args["steps"].empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid steps (expected non-empty array)\"}";
    return;
  }

  const int64_t now = edge_unix_ms_now();
  std::vector<AgentDb::EdgeWorkflowStepRow> steps;
  steps.reserve(args["steps"].size());

  for (Json::ArrayIndex i = 0; i < args["steps"].size(); i++) {
    const auto& s = args["steps"][i];
    if (!s.isObject()) continue;
    const std::string step_id = s.isMember("step_id") && s["step_id"].isString() ? trim_copy(s["step_id"].asString()) : "";
    const std::string kind = s.isMember("kind") && s["kind"].isString() ? trim_copy(s["kind"].asString()) : "";
    if (step_id.empty() || !edge_id_is_safe(step_id) || kind.empty()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid step (missing step_id/kind)\"}";
      return;
    }
    if (kind != "invoke_tool" && kind != "run_agent" && kind != "join") {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"unsupported step.kind\"}";
      return;
    }

    Json::Value depends(Json::arrayValue);
    if (s.isMember("depends_on") && s["depends_on"].isArray()) depends = s["depends_on"];
    Json::Value target = s.isMember("target") ? s["target"] : Json::Value(Json::objectValue);
    Json::Value payload = s.isMember("payload") ? s["payload"] : Json::Value(Json::objectValue);
    if (kind != "join" && !target.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid step.target (expected object)\"}";
      return;
    }
    if (!payload.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid step.payload (expected object)\"}";
      return;
    }

    std::string join_mode = s.isMember("join_mode") && s["join_mode"].isString() ? trim_copy(s["join_mode"].asString()) : "";
    if (!join_mode.empty() && join_mode != "all" && join_mode != "any") {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid join_mode (expected all|any)\"}";
      return;
    }

    int64_t deadline_utc_ms = 0;
    if (s.isMember("deadline_utc_ms") && (s["deadline_utc_ms"].isInt64() || s["deadline_utc_ms"].isUInt64())) {
      deadline_utc_ms = s["deadline_utc_ms"].isInt64() ? s["deadline_utc_ms"].asInt64() : (int64_t)s["deadline_utc_ms"].asUInt64();
    }
    if (kind != "join" && deadline_utc_ms <= 0) deadline_utc_ms = now + 60000;

    int max_attempts = 1;
    if (s.isMember("max_attempts") && (s["max_attempts"].isInt() || s["max_attempts"].isUInt())) {
      max_attempts = s["max_attempts"].isInt() ? s["max_attempts"].asInt() : (int)std::min((Json::UInt)INT32_MAX, s["max_attempts"].asUInt());
    }
    if (max_attempts < 1) max_attempts = 1;
    if (max_attempts > 100) max_attempts = 100;

    int backoff_ms = 0;
    if (s.isMember("backoff_ms") && (s["backoff_ms"].isInt() || s["backoff_ms"].isUInt())) {
      backoff_ms = s["backoff_ms"].isInt() ? s["backoff_ms"].asInt() : (int)std::min((Json::UInt)INT32_MAX, s["backoff_ms"].asUInt());
    }
    if (backoff_ms < 0) backoff_ms = 0;
    if (backoff_ms > 600000) backoff_ms = 600000;

    AgentDb::EdgeWorkflowStepRow row;
    row.workflow_id = workflow_id;
    row.step_id = step_id;
    row.kind = kind;
    row.depends_on_json = edge_json_stringify_compact(depends);
    row.target_json = edge_json_stringify_compact(target);
    row.payload_json = edge_json_stringify_compact(payload);
    row.join_mode = join_mode;
    row.deadline_utc_ms = deadline_utc_ms;
    row.attempt = 0;
    row.max_attempts = (kind == "join") ? 1 : max_attempts;
    row.next_ready_utc_ms = 0;
    row.backoff_ms = (kind == "join") ? 0 : backoff_ms;
    row.state = "PENDING";
    row.created_utc_ms = now;
    row.updated_utc_ms = now;
    steps.push_back(std::move(row));
  }

  if (steps.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"no valid steps\"}";
    return;
  }

  args["workflow_id"] = workflow_id;
  const std::string spec_json = edge_json_stringify_compact(args);

  AgentDb::EdgeWorkflowRow wf;
  wf.workflow_id = workflow_id;
  wf.goal = goal;
  wf.status = "QUEUED";
  wf.priority = priority;
  wf.spec_json = spec_json;
  wf.created_utc_ms = now;
  wf.updated_utc_ms = now;

  std::string err;
  if (!db_or_null->create_edge_workflow(wf, steps, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to create edge workflow";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  {
    AgentDb::EdgeWorkflowEventRow ev;
    ev.workflow_id = workflow_id;
    ev.ts_utc_ms = now;
    ev.type = "workflow_created";
    Json::Value d(Json::objectValue);
    d["workflow_id"] = workflow_id;
    if (!goal.empty()) d["goal"] = goal;
    d["priority"] = priority;
    d["steps"] = (Json::Int64)steps.size();
    ev.data_json = edge_json_stringify_compact(d);
    (void)db_or_null->insert_edge_workflow_event(ev, nullptr, nullptr);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_workflow_get_endpoint(
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

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty() || !edge_id_is_safe(*wid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid workflow_id\"}";
    return;
  }
  bool include_steps = false;
  const auto inc = query_get(req.query, "include_steps");
  if (inc && !inc->empty()) {
    const std::string v = lower_copy(*inc);
    include_steps = (v == "1" || v == "true" || v == "yes");
  }

  AgentDb::EdgeWorkflowRow wf;
  std::string err;
  if (!db_or_null->get_edge_workflow(*wid, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value w(Json::objectValue);
  w["workflow_id"] = wf.workflow_id;
  if (!wf.goal.empty()) w["goal"] = wf.goal;
  w["status"] = wf.status;
  w["priority"] = wf.priority;
  w["created_utc_ms"] = (Json::Int64)wf.created_utc_ms;
  w["updated_utc_ms"] = (Json::Int64)wf.updated_utc_ms;
  if (!wf.error.empty()) w["error"] = wf.error;
  if (!wf.spec_json.empty()) {
    Json::Value v;
    std::string perr2;
    if (json_parse_any(wf.spec_json, &v, &perr2) && v.isObject()) w["spec"] = v;
  }
  o["workflow"] = w;

  if (include_steps) {
    std::vector<AgentDb::EdgeWorkflowStepRow> steps;
    std::string serr;
    if (db_or_null->list_edge_workflow_steps(*wid, &steps, &serr)) {
      Json::Value arr(Json::arrayValue);
      std::string perr2;
      for (const auto& s : steps) {
        Json::Value r(Json::objectValue);
        r["step_id"] = s.step_id;
        r["kind"] = s.kind;
        r["state"] = s.state;
        if (!s.join_mode.empty()) r["join_mode"] = s.join_mode;
        if (s.deadline_utc_ms > 0) r["deadline_utc_ms"] = (Json::Int64)s.deadline_utc_ms;
        r["attempt"] = s.attempt;
        r["max_attempts"] = s.max_attempts;
        if (s.next_ready_utc_ms > 0) r["next_ready_utc_ms"] = (Json::Int64)s.next_ready_utc_ms;
        if (s.backoff_ms > 0) r["backoff_ms"] = s.backoff_ms;
        r["created_utc_ms"] = (Json::Int64)s.created_utc_ms;
        r["updated_utc_ms"] = (Json::Int64)s.updated_utc_ms;
        if (!s.error.empty()) r["error"] = s.error;
        Json::Value v;
        if (json_parse_any(s.depends_on_json, &v, &perr2) && v.isArray()) r["depends_on"] = v;
        if (json_parse_any(s.target_json, &v, &perr2) && v.isObject()) r["target"] = v;
        if (json_parse_any(s.payload_json, &v, &perr2) && v.isObject()) r["payload"] = v;
        arr.append(r);
      }
      o["steps"] = arr;
    }
  }
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_workflow_list_endpoint(
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

  const auto st = query_get(req.query, "status");
  std::string status = st && !st->empty() ? trim_copy(*st) : "QUEUED";
  if (status != "QUEUED" && status != "RUNNING" && status != "SUCCEEDED" && status != "FAILED" && status != "CANCELED") {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid status\"}";
    return;
  }

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 256));

  std::vector<AgentDb::EdgeWorkflowRow> rows;
  std::string err;
  if (!db_or_null->list_edge_workflows_by_status(status, limit, &rows, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list workflows";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["status"] = status;
  Json::Value arr(Json::arrayValue);
  for (const auto& wf : rows) {
    Json::Value w(Json::objectValue);
    w["workflow_id"] = wf.workflow_id;
    if (!wf.goal.empty()) w["goal"] = wf.goal;
    w["status"] = wf.status;
    w["priority"] = wf.priority;
    w["created_utc_ms"] = (Json::Int64)wf.created_utc_ms;
    w["updated_utc_ms"] = (Json::Int64)wf.updated_utc_ms;
    if (!wf.error.empty()) w["error"] = wf.error;
    arr.append(w);
  }
  o["workflows"] = arr;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_workflow_cancel_endpoint(
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
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  const std::string workflow_id =
    args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty() || !edge_id_is_safe(workflow_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid workflow_id\"}";
    return;
  }

  AgentDb::EdgeWorkflowRow wf;
  std::string err;
  if (!db_or_null->get_edge_workflow(workflow_id, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  const int64_t now = edge_unix_ms_now();
  if (wf.status != "CANCELED" && wf.status != "SUCCEEDED" && wf.status != "FAILED") {
    wf.status = "CANCELED";
    wf.updated_utc_ms = now;
    (void)db_or_null->upsert_edge_workflow(wf, nullptr);
  }

  std::vector<AgentDb::EdgeWorkflowStepRow> steps;
  std::string serr;
  (void)db_or_null->list_edge_workflow_steps(workflow_id, &steps, &serr);
  for (auto& s : steps) {
    if (s.state == "SUCCEEDED" || s.state == "FAILED" || s.state == "TIMED_OUT" || s.state == "CANCELED") continue;
    s.state = "CANCELED";
    s.updated_utc_ms = now;
    (void)db_or_null->upsert_edge_workflow_step(s, nullptr);

    if (s.kind != "join") {
      AgentDb::EdgeTaskRow tr;
      std::string terr;
      if (db_or_null->get_edge_task(workflow_id, s.step_id, &tr, &terr)) {
        if (!edge_is_terminal_task_state(tr.state)) {
          tr.state = "CANCELED";
          tr.updated_utc_ms = now;
          if (tr.error.empty()) tr.error = "canceled";
          (void)db_or_null->upsert_edge_task(tr, nullptr);
        }
      }
    }
  }

  {
    AgentDb::EdgeWorkflowEventRow ev;
    ev.workflow_id = workflow_id;
    ev.ts_utc_ms = now;
    ev.type = "workflow_canceled";
    Json::Value d(Json::objectValue);
    d["workflow_id"] = workflow_id;
    ev.data_json = edge_json_stringify_compact(d);
    (void)db_or_null->insert_edge_workflow_event(ev, nullptr, nullptr);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  o["status"] = wf.status;
  resp->body = edge_json_stringify_compact(o);
}

void handle_edge_workflow_events_endpoint(
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

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty() || !edge_id_is_safe(*wid)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing/invalid workflow_id\"}";
    return;
  }

  int64_t cursor = 0;
  const auto cursor_q = query_get(req.query, "cursor");
  if (cursor_q && !cursor_q->empty()) {
    try {
      cursor = (int64_t)std::stoll(*cursor_q);
    } catch (...) {
      cursor = 0;
    }
  }
  if (cursor < 0) cursor = 0;

  int limit = 256;
  const auto limit_q = query_get(req.query, "limit");
  if (limit_q && !limit_q->empty()) {
    try {
      limit = std::stoi(*limit_q);
    } catch (...) {
      limit = 256;
    }
  }
  if (limit < 1) limit = 1;
  if (limit > 1024) limit = 1024;

  std::vector<AgentDb::EdgeWorkflowEventRow> rows;
  std::string err;
  if (!db_or_null->list_edge_workflow_events(*wid, cursor, (size_t)limit, &rows, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list edge workflow events";
    o["detail"] = err;
    resp->body = edge_json_stringify_compact(o);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["workflow_id"] = *wid;
  out["cursor_base"] = (Json::Int64)cursor;
  Json::Value arr(Json::arrayValue);
  int64_t cursor_next = cursor;
  for (const auto& r : rows) {
    Json::Value ev(Json::objectValue);
    ev["id"] = (Json::Int64)r.id;
    ev["ts_utc_ms"] = (Json::Int64)r.ts_utc_ms;
    ev["type"] = r.type;
    Json::Value data;
    std::string perr;
    if (json_parse_any(r.data_json, &data, &perr)) ev["data"] = data;
    else {
      Json::Value bad(Json::objectValue);
      bad["ok"] = false;
      bad["error"] = "failed to parse data_json";
      bad["parse_error"] = perr;
      bad["raw"] = r.data_json;
      ev["data"] = bad;
    }
    arr.append(ev);
    if (r.id > cursor_next) cursor_next = r.id;
  }
  out["events"] = arr;
  out["cursor_next"] = (Json::Int64)cursor_next;
  resp->body = edge_json_stringify_compact(out);
}

}  // namespace agentd
