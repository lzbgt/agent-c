#include "workflow_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "session_id_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
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

static std::string new_workflow_id() {
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
  (void)snprintf(buf, sizeof(buf), "wf_%08x-%04x-%04x-%04x-%012llx",
                 a, (unsigned)b, (unsigned)c, (unsigned)d, (unsigned long long)(e & 0xffffffffffffull));
  return std::string(buf);
}

static std::string redact_json_best_effort(const std::string& json) {
  if (json.empty()) return json;
  Json::Value v;
  std::string perr;
  if (!json_parse_any(json, &v, &perr)) return json;

  std::function<void(Json::Value*)> walk = [&](Json::Value* cur) {
    if (!cur) return;
    if (cur->isObject()) {
      for (const auto& k : cur->getMemberNames()) {
        if (k == "api_key" || k == "Authorization" || k == "auth_token") {
          (*cur)[k] = "***redacted***";
        } else {
          walk(&((*cur)[k]));
        }
      }
    } else if (cur->isArray()) {
      for (Json::ArrayIndex i = 0; i < cur->size(); i++) {
        walk(&((*cur)[i]));
      }
    }
  };
  walk(&v);
  return json_stringify_compact(v);
}

static bool validate_dag_or_error(
  const std::vector<std::string>& task_ids,
  const std::unordered_map<std::string, std::vector<std::string>>& deps,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  std::unordered_set<std::string> nodes(task_ids.begin(), task_ids.end());

  std::unordered_map<std::string, int> indeg;
  indeg.reserve(nodes.size());
  for (const auto& id : nodes) indeg[id] = 0;
  for (const auto& kv : deps) {
    const std::string& t = kv.first;
    if (!nodes.count(t)) continue;
    for (const auto& d : kv.second) {
      if (!nodes.count(d)) {
        if (out_err) *out_err = "unknown dependency: " + d;
        return false;
      }
      indeg[t] += 1;
    }
  }

  std::vector<std::string> q;
  q.reserve(nodes.size());
  for (const auto& kv : indeg) {
    if (kv.second == 0) q.push_back(kv.first);
  }
  size_t idx = 0;
  size_t visited = 0;
	  while (idx < q.size()) {
	    const std::string cur = q[idx++];
	    visited++;
	    // deps map is reverse (task -> deps). Need reverse edges:
	    // Just scan all nodes once (bounded by task count).
	    for (const auto& kv2 : deps) {
	      const std::string& child = kv2.first;
      if (!nodes.count(child)) continue;
      for (const auto& dep : kv2.second) {
        if (dep == cur) {
          indeg[child] -= 1;
          if (indeg[child] == 0) q.push_back(child);
        }
      }
    }
  }
  if (visited != nodes.size()) {
    if (out_err) *out_err = "dependency cycle detected";
    return false;
  }
  return true;
}

}  // namespace

void handle_workflow_submit_endpoint(
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

  const auto& tasks = args["tasks"];
  if (!tasks.isArray() || tasks.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing tasks (expected non-empty array)\"}";
    return;
  }
  if (tasks.size() > 128) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"too many tasks (max 128)\"}";
    return;
  }

  const bool allow_sessions =
    args.isMember("allow_sessions") && args["allow_sessions"].isBool() ? args["allow_sessions"].asBool() : false;
  const bool allow_inline_api_keys =
    args.isMember("allow_inline_api_keys") && args["allow_inline_api_keys"].isBool() ? args["allow_inline_api_keys"].asBool() : false;

  std::string workflow_id =
    args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty()) workflow_id = new_workflow_id();
  if (!id_is_safe(workflow_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid workflow_id\"}";
    return;
  }

  std::string trace_id =
    args.isMember("trace_id") && args["trace_id"].isString() ? trim_copy(args["trace_id"].asString()) : "";
  if (!trace_id.empty() && !id_is_safe(trace_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid trace_id\"}";
    return;
  }
  if (trace_id.empty()) {
    trace_id = workflow_id; // stable default
  }

  std::string session_id =
    args.isMember("session_id") && args["session_id"].isString() ? trim_copy(args["session_id"].asString()) : "";
  if (!session_id.empty() && !session_id_is_safe(session_id)) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"invalid session_id\"}";
    return;
  }

  const Json::Value defaults =
    args.isMember("defaults") && args["defaults"].isObject() ? args["defaults"] : Json::Value(Json::nullValue);

  int workflow_priority = 0;
  if (args.isMember("priority")) {
    if (!args["priority"].isInt()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"invalid priority (expected int)\"}";
      return;
    }
    workflow_priority = args["priority"].asInt();
    if (workflow_priority < -1000) workflow_priority = -1000;
    if (workflow_priority > 1000) workflow_priority = 1000;
    args["priority"] = workflow_priority; // canonicalize
  }
  if (!args.isMember("priority")) args["priority"] = workflow_priority;

  const int64_t now = unix_ms_now();
  std::unordered_set<std::string> seen_ids;
  std::vector<std::string> task_ids;
  task_ids.reserve(tasks.size());
  std::unordered_map<std::string, std::vector<std::string>> deps_by_task;
  std::vector<AgentDb::WorkflowTaskRow> rows;
  rows.reserve(tasks.size());

  for (Json::ArrayIndex i = 0; i < tasks.size(); i++) {
    const auto& t = tasks[i];
    if (!t.isObject()) {
      resp->status = 400;
      resp->body = "{\"ok\":false,\"error\":\"task entry is not an object\"}";
      return;
    }
    const std::string task_id =
      t.isMember("task_id") && t["task_id"].isString() ? trim_copy(t["task_id"].asString()) : ("task_" + std::to_string(i));
    if (!id_is_safe(task_id)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid task_id";
      o["task_id"] = task_id;
      resp->body = json_stringify_compact(o);
      return;
    }
    if (!seen_ids.insert(task_id).second) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "duplicate task_id";
      o["task_id"] = task_id;
      resp->body = json_stringify_compact(o);
      return;
    }

    const std::string kind =
      t.isMember("kind") && t["kind"].isString() ? trim_copy(t["kind"].asString()) : std::string();
    const bool is_avm = (kind == "avm_capsule");
    const bool is_aggregate = (kind == "aggregate");
    const bool is_edge = (kind == "edge_invoke");
    const bool is_special = is_avm || is_aggregate || is_edge;

    Json::Value run_req = t.isMember("request") && t["request"].isObject() ? t["request"] : t;
    if (!is_special && defaults.isObject()) {
      for (const auto& k : defaults.getMemberNames()) {
        if (!run_req.isMember(k)) run_req[k] = defaults[k];
      }
    }

    int task_priority = workflow_priority;
    if (run_req.isMember("priority")) {
      if (!run_req["priority"].isInt()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid task priority (expected int)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_priority = run_req["priority"].asInt();
    }
    if (t.isMember("priority")) {
      if (!t["priority"].isInt()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "invalid task priority (expected int)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_priority = t["priority"].asInt();
    }
    if (task_priority < -1000) task_priority = -1000;
    if (task_priority > 1000) task_priority = 1000;
    if (!is_special) run_req["priority"] = task_priority;

    if (!is_special) {
      if (!allow_sessions) {
        run_req["no_session"] = true;
        if (!run_req.isMember("tools")) run_req["tools"] = "none";
      } else if (!session_id.empty()) {
        // If sessions are allowed, default tasks to the workflow session unless explicitly overridden.
        if (!run_req.isMember("session_id") && (!run_req.isMember("no_session") || !run_req["no_session"].isBool() || !run_req["no_session"].asBool())) {
          run_req["session_id"] = session_id;
        }
      }
    }

    if (!is_special) {
      if (run_req.isMember("api_key") && run_req["api_key"].isString() && !run_req["api_key"].asString().empty() && !allow_inline_api_keys) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "inline api_key is not allowed for durable workflows (set daemon api_key/provider_keys or pass allow_inline_api_keys=true)";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
    }

    Json::Value task_req(Json::objectValue);
    if (is_avm) {
      if (!t.isMember("capsule") || !t["capsule"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "avm_capsule task missing capsule object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& cap = t["capsule"];
      if (!cap.isMember("obc_base64") || !cap["obc_base64"].isString() || cap["obc_base64"].asString().empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "avm_capsule task capsule missing obc_base64";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_req["kind"] = "avm_capsule";
      task_req["capsule"] = cap;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
    } else if (is_aggregate) {
      if (!t.isMember("aggregate") || !t["aggregate"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "aggregate task missing aggregate object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& agg = t["aggregate"];
      if (!agg.isMember("task_ids") || !agg["task_ids"].isArray() || agg["task_ids"].empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "aggregate.task_ids must be a non-empty array";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_req["kind"] = "aggregate";
      task_req["aggregate"] = agg;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
    } else if (is_edge) {
      if (!t.isMember("edge") || !t["edge"].isObject()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_invoke task missing edge object";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      const auto& edge = t["edge"];
      const std::string tool = edge.isMember("tool") && edge["tool"].isString() ? trim_copy(edge["tool"].asString()) : "";
      const bool has_args = edge.isMember("args") && edge["args"].isObject();
      const bool has_node_id = edge.isMember("node_id") && edge["node_id"].isString() && !trim_copy(edge["node_id"].asString()).empty();
      const bool has_match_any = edge.isMember("match_any") && edge["match_any"].isObject();
      if (tool.empty() || !has_args) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_invoke task edge missing tool/args";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      if (!has_node_id && !has_match_any) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "edge_invoke task edge missing node_id or match_any";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }
      task_req["kind"] = "edge_invoke";
      task_req["edge"] = edge;
      task_req["priority"] = task_priority;
      task_req["trace_id"] = trace_id + ":" + task_id;
    } else {
      if (!run_req.isMember("prompt") || !run_req["prompt"].isString() || run_req["prompt"].asString().empty()) {
        resp->status = 400;
        Json::Value o(Json::objectValue);
        o["ok"] = false;
        o["error"] = "task missing prompt";
        o["task_id"] = task_id;
        resp->body = json_stringify_compact(o);
        return;
      }

      // Default trace_id per task (inherits workflow trace_id).
      if (!run_req.isMember("trace_id") || !run_req["trace_id"].isString() || run_req["trace_id"].asString().empty()) {
        run_req["trace_id"] = trace_id + ":" + task_id;
      }
      task_req = run_req;
    }

    Json::Value deps_arr(Json::arrayValue);
    std::vector<std::string> dep_ids;
    if (t.isMember("depends_on") && t["depends_on"].isArray()) {
      deps_arr = t["depends_on"];
      for (Json::ArrayIndex di = 0; di < deps_arr.size(); di++) {
        if (!deps_arr[di].isString()) continue;
        dep_ids.push_back(trim_copy(deps_arr[di].asString()));
      }
    }
    deps_by_task[task_id] = dep_ids;

    int max_attempts = 1;
    if (t.isMember("max_attempts") && t["max_attempts"].isInt()) {
      max_attempts = std::max(1, std::min(512, t["max_attempts"].asInt()));
    } else if (is_edge) {
      // Edge invocations are inherently async; default to a generous poll budget.
      // (The workflow engine uses `retry_in_ms` for polling delay.)
      max_attempts = 200;
    }

    AgentDb::WorkflowTaskRow row;
    row.workflow_id = workflow_id;
    row.task_id = task_id;
    row.priority = task_priority;
    row.created_unix_ms = now;
    row.updated_unix_ms = now;
    row.status = "queued";
    row.attempt = 0;
    row.max_attempts = max_attempts;
    row.ready_unix_ms = 0;
    row.started_unix_ms = 0;
    row.finished_unix_ms = 0;
    row.depends_on_json = deps_arr.isArray() ? json_stringify_compact(deps_arr) : "[]";
    row.request_json = json_stringify_compact(task_req);
    if (t.isMember("expect") && t["expect"].isObject()) {
      row.expect_json = json_stringify_compact(t["expect"]);
    }
    rows.push_back(std::move(row));
    task_ids.push_back(task_id);
  }

  {
    std::string derr;
    if (!validate_dag_or_error(task_ids, deps_by_task, &derr)) {
      resp->status = 400;
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = "invalid workflow DAG";
      o["detail"] = derr;
      resp->body = json_stringify_compact(o);
      return;
    }
  }

  // Persist.
  AgentDb::WorkflowRow wf;
  wf.workflow_id = workflow_id;
  wf.session_id = allow_sessions ? session_id : "";
  wf.trace_id = trace_id;
  wf.priority = workflow_priority;
  wf.created_unix_ms = now;
  wf.updated_unix_ms = now;
  wf.status = "queued";
  wf.cancel_requested = false;
  wf.error.clear();

  // Store a redacted view of the submit request for troubleshooting.
  {
    Json::Value view = args;
    view["workflow_id"] = workflow_id;
    view["trace_id"] = trace_id;
    view["session_id"] = allow_sessions ? session_id : "";
    view["submitted_unix_ms"] = (Json::Int64)now;
    wf.spec_json = redact_json_best_effort(json_stringify_compact(view));
    if (wf.spec_json.size() > 256 * 1024) {
      wf.spec_json.resize(256 * 1024);
    }
  }

  std::string db_err;
  if (!db_or_null->create_workflow(wf, rows, &db_err)) {
    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to create workflow";
    o["detail"] = db_err;
    o["workflow_id"] = workflow_id;
    resp->body = json_stringify_compact(o);
    return;
  }

  // Workflow event log: initial created marker (best-effort).
  {
    Json::Value d(Json::objectValue);
    d["workflow_id"] = workflow_id;
    d["status"] = "queued";
    d["trace_id"] = trace_id;
    if (allow_sessions && !session_id.empty()) d["session_id"] = session_id;
    d["ts_unix_ms"] = (Json::Int64)now;
    AgentDb::WorkflowEventRow ev;
    ev.workflow_id = workflow_id;
    ev.ts_unix_ms = now;
    ev.type = "workflow_created";
    ev.data_json = json_stringify_compact(d);
    (void)db_or_null->insert_workflow_event(ev, nullptr, nullptr);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  o["trace_id"] = trace_id;
  if (allow_sessions && !session_id.empty()) o["session_id"] = session_id;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_get_endpoint(
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
  if (!wid || wid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
    return;
  }

  const auto incl_tasks_q = query_get(req.query, "include_tasks");
  const bool include_tasks = !incl_tasks_q || (*incl_tasks_q != "0" && *incl_tasks_q != "false");
  const auto incl_results_q = query_get(req.query, "include_results");
  const bool include_results = incl_results_q && (*incl_results_q == "1" || *incl_results_q == "true");

  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_or_null->get_workflow(*wid, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value w(Json::objectValue);
  w["workflow_id"] = wf.workflow_id;
  w["status"] = wf.status;
  w["priority"] = wf.priority;
  if (!wf.trace_id.empty()) w["trace_id"] = wf.trace_id;
  if (!wf.session_id.empty()) w["session_id"] = wf.session_id;
  w["cancel_requested"] = wf.cancel_requested;
  w["created_unix_ms"] = (Json::Int64)wf.created_unix_ms;
  w["updated_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
  if (!wf.error.empty()) w["error"] = wf.error;
  o["workflow"] = w;

  if (include_tasks) {
    std::vector<AgentDb::WorkflowTaskRow> tasks;
    if (db_or_null->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      Json::Value arr(Json::arrayValue);
      for (const auto& t : tasks) {
        Json::Value row(Json::objectValue);
        row["task_id"] = t.task_id;
        row["status"] = t.status;
        row["priority"] = t.priority;
        row["attempt"] = t.attempt;
        row["max_attempts"] = t.max_attempts;
        row["ready_unix_ms"] = (Json::Int64)t.ready_unix_ms;
        row["started_unix_ms"] = (Json::Int64)t.started_unix_ms;
        row["finished_unix_ms"] = (Json::Int64)t.finished_unix_ms;
        if (!t.error.empty()) row["error"] = t.error;
        if (!t.depends_on_json.empty()) {
          Json::Value deps;
          std::string derr;
          if (json_parse_any(t.depends_on_json, &deps, &derr) && deps.isArray()) row["depends_on"] = deps;
        }
        if (include_results && !t.result_json.empty()) {
          Json::Value rr;
          std::string rerr;
          if (json_parse_any(t.result_json, &rr, &rerr)) row["result"] = rr;
        }
        arr.append(row);
      }
      o["tasks"] = arr;
    }
  }

  if (include_results && !wf.result_json.empty()) {
    Json::Value rr;
    std::string rerr;
    if (json_parse_any(wf.result_json, &rr, &rerr)) o["result"] = rr;
  }

  resp->body = json_stringify_compact(o);
}

void handle_workflow_list_endpoint(
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
  const std::string status = st && !st->empty() ? *st : "running";

  size_t limit = 50;
  const auto lim = query_get(req.query, "limit");
  if (lim && !lim->empty()) {
    try { limit = (size_t)std::stoull(*lim); } catch (...) {}
  }
  limit = std::max<size_t>(1, std::min<size_t>(limit, 200));

  std::vector<AgentDb::WorkflowRow> wfs;
  std::string err;
  if (!db_or_null->list_workflows_by_status(status, limit, &wfs, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list workflows";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["status"] = status;
  Json::Value arr(Json::arrayValue);
  for (const auto& wf : wfs) {
    Json::Value row(Json::objectValue);
    row["workflow_id"] = wf.workflow_id;
    row["status"] = wf.status;
    row["priority"] = wf.priority;
    if (!wf.trace_id.empty()) row["trace_id"] = wf.trace_id;
    if (!wf.session_id.empty()) row["session_id"] = wf.session_id;
    row["cancel_requested"] = wf.cancel_requested;
    row["created_unix_ms"] = (Json::Int64)wf.created_unix_ms;
    row["updated_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
    if (!wf.error.empty()) row["error"] = wf.error;
    arr.append(row);
  }
  o["workflows"] = arr;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_cancel_endpoint(
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

  const std::string workflow_id =
    args.isMember("workflow_id") && args["workflow_id"].isString() ? trim_copy(args["workflow_id"].asString()) : "";
  if (workflow_id.empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
    return;
  }

  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_or_null->get_workflow(workflow_id, &wf, &err)) {
    resp->status = 404;
    resp->body = "{\"ok\":false,\"error\":\"workflow not found\"}";
    return;
  }

  wf.cancel_requested = true;
  wf.updated_unix_ms = unix_ms_now();
  if (wf.status == "queued") wf.status = "running"; // surface cancel in progress
  (void)db_or_null->upsert_workflow(wf, &err);

  // Best-effort: append a cancel_requested event.
  {
    const int64_t now = unix_ms_now();
    Json::Value d(Json::objectValue);
    d["workflow_id"] = wf.workflow_id;
    d["status"] = wf.status;
    d["cancel_requested"] = true;
    if (!wf.trace_id.empty()) d["trace_id"] = wf.trace_id;
    if (!wf.session_id.empty()) d["session_id"] = wf.session_id;
    d["ts_unix_ms"] = (Json::Int64)now;
    AgentDb::WorkflowEventRow ev;
    ev.workflow_id = wf.workflow_id;
    ev.ts_unix_ms = now;
    ev.type = "workflow_cancel_requested";
    ev.data_json = json_stringify_compact(d);
    (void)db_or_null->insert_workflow_event(ev, nullptr, nullptr);
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["workflow_id"] = workflow_id;
  resp->body = json_stringify_compact(o);
}

void handle_workflow_events_endpoint(
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
  if (!wid || wid->empty()) {
    resp->status = 400;
    resp->body = "{\"ok\":false,\"error\":\"missing workflow_id\"}";
    return;
  }

  int64_t after = 0;
  const auto a = query_get(req.query, "after_event_id");
  if (a && !a->empty()) {
    try { after = (int64_t)std::stoll(*a); } catch (...) { after = 0; }
  }

  size_t limit = 256;
  const auto l = query_get(req.query, "limit");
  if (l && !l->empty()) {
    try { limit = (size_t)std::stoull(*l); } catch (...) { limit = 256; }
  }
  limit = std::min<size_t>(limit, 1000);

  std::vector<AgentDb::WorkflowEventRow> rows;
  std::string err;
  if (!db_or_null->list_workflow_events(*wid, after, limit, &rows, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to list workflow events";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["workflow_id"] = *wid;
  out["after_event_id"] = (Json::Int64)after;
  Json::Value arr(Json::arrayValue);
  int64_t last = after;
  for (const auto& r : rows) {
    Json::Value o(Json::objectValue);
    o["event_id"] = (Json::Int64)r.event_id;
    o["ts_unix_ms"] = (Json::Int64)r.ts_unix_ms;
    o["type"] = r.type;
    if (!r.task_id.empty()) o["task_id"] = r.task_id;
    Json::Value data;
    std::string perr;
    if (!json_parse_any(r.data_json, &data, &perr)) {
      Json::Value bad(Json::objectValue);
      bad["ok"] = false;
      bad["error"] = "failed to parse event data_json";
      bad["parse_error"] = perr;
      bad["raw"] = r.data_json;
      data = bad;
    }
    o["data"] = data;
    arr.append(o);
    if (r.event_id > last) last = r.event_id;
  }
  out["events"] = arr;
  out["cursor_next"] = (Json::Int64)last;
  resp->body = json_stringify_compact(out);
}

}  // namespace agentd
