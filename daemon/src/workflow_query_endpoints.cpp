#include "workflow_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <string>
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

}  // namespace

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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing workflow_id");
    return;
  }

  const auto incl_tasks_q = query_get(req.query, "include_tasks");
  const bool include_tasks = !incl_tasks_q || (*incl_tasks_q != "0" && *incl_tasks_q != "false");
  const auto incl_results_q = query_get(req.query, "include_results");
  const bool include_results = incl_results_q && (*incl_results_q == "1" || *incl_results_q == "true");
  const auto incl_spec_q = query_get(req.query, "include_spec");
  const bool include_spec = incl_spec_q && (*incl_spec_q == "1" || *incl_spec_q == "true");

  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_or_null->get_workflow(*wid, &wf, &err)) {
    resp->status = 404;
    resp->body = json_error_body("workflow not found");
    return;
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  Json::Value w(Json::objectValue);
  w["workflow_id"] = wf.workflow_id;
  w["status"] = wf.status;
  w["priority"] = wf.priority;
  if (wf.deadline_unix_ms > 0) w["deadline_unix_ms"] = (Json::Int64)wf.deadline_unix_ms;
  if (!wf.idempotency_key.empty()) w["idempotency_key"] = wf.idempotency_key;
  if (!wf.trace_id.empty()) w["trace_id"] = wf.trace_id;
  if (!wf.session_id.empty()) w["session_id"] = wf.session_id;
  w["cancel_requested"] = wf.cancel_requested;
  w["created_unix_ms"] = (Json::Int64)wf.created_unix_ms;
  w["updated_unix_ms"] = (Json::Int64)wf.updated_unix_ms;
  if (!wf.error.empty()) w["error"] = wf.error;
  o["workflow"] = w;

  // Best-effort: surface workflow_limits + aggregate usage/remaining budgets from the persisted spec_json and
  // task cumulative counters (retry-safe).
  {
    Json::Value spec;
    std::string perr;
    if (json_parse_any(wf.spec_json, &spec, &perr) && spec.isObject()) {
      if (spec.isMember("workflow_limits") && spec["workflow_limits"].isObject()) {
        o["workflow_limits"] = spec["workflow_limits"];
      }
    }
  }

  // Best-effort: surface aggregate usage independent of include_tasks so UIs/schedulers can poll cheaply.
  {
    AgentDb::WorkflowUsageTotals totals;
    std::string uerr;
    if (db_or_null->get_workflow_usage_totals(wf.workflow_id, &totals, &uerr)) {
      Json::Value usage(Json::objectValue);
      usage["tool_calls_total_used"] = (Json::Int64)std::max<int64_t>(0, totals.tool_calls_total_used);
      usage["steps_total_used"] = (Json::Int64)std::max<int64_t>(0, totals.steps_total_used);
      usage["elapsed_ms_total_used"] = (Json::Int64)std::max<int64_t>(0, totals.elapsed_ms_total_used);
      usage["prompt_tokens_used"] = (Json::Int64)std::max<int64_t>(0, totals.prompt_tokens_used);
      usage["completion_tokens_used"] = (Json::Int64)std::max<int64_t>(0, totals.completion_tokens_used);
      usage["total_tokens_used"] = (Json::Int64)std::max<int64_t>(0, totals.total_tokens_used);
      o["workflow_usage"] = usage;

      if (o.isMember("workflow_limits") && o["workflow_limits"].isObject()) {
        const Json::Value lim = o["workflow_limits"];
        Json::Value rem(Json::objectValue);

        auto rem_i64 = [&](const char* k, int64_t used) {
          if (!lim.isMember(k)) return;
          const auto& v = lim[k];
          if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return;
          const int64_t maxv = v.asInt64();
          if (maxv <= 0) return;
          rem[k] = (Json::Int64)std::max<int64_t>(0, maxv - std::max<int64_t>(0, used));
        };

        rem_i64("max_tool_calls_total", totals.tool_calls_total_used);
        rem_i64("max_steps_total", totals.steps_total_used);
        rem_i64("max_elapsed_ms_total", totals.elapsed_ms_total_used);
        rem_i64("max_total_tokens", totals.total_tokens_used);

        if (!rem.getMemberNames().empty()) {
          o["workflow_remaining"] = rem;
        }
      }
    }
  }

  if (include_tasks) {
    std::vector<AgentDb::WorkflowTaskRow> tasks;
    if (db_or_null->list_workflow_tasks(wf.workflow_id, &tasks, &err)) {
      Json::Value arr(Json::arrayValue);
      for (const auto& t : tasks) {
        Json::Value row(Json::objectValue);
        row["task_id"] = t.task_id;
        row["status"] = t.status;
        row["allow_error"] = t.allow_error;
        row["priority"] = t.priority;
        row["attempt"] = t.attempt;
        row["max_attempts"] = t.max_attempts;
        row["tool_calls_total_cum"] = (Json::Int64)std::max<int64_t>(0, t.tool_calls_total_cum);
        row["steps_executed_cum"] = (Json::Int64)std::max<int64_t>(0, t.steps_executed_cum);
        row["elapsed_ms_cum"] = (Json::Int64)std::max<int64_t>(0, t.elapsed_ms_cum);
        row["prompt_tokens_cum"] = (Json::Int64)std::max<int64_t>(0, t.prompt_tokens_cum);
        row["completion_tokens_cum"] = (Json::Int64)std::max<int64_t>(0, t.completion_tokens_cum);
        row["total_tokens_cum"] = (Json::Int64)std::max<int64_t>(0, t.total_tokens_cum);

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

  if (include_spec && !wf.spec_json.empty()) {
    // spec_json may have been truncated (size cap), which can render it invalid JSON; return raw string always.
    o["spec_json"] = wf.spec_json;
    Json::Value sr;
    std::string serr;
    if (json_parse_any(wf.spec_json, &sr, &serr)) {
      o["spec"] = sr;
    }
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
    resp->body = json_error_body("db not available");
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
    if (wf.deadline_unix_ms > 0) row["deadline_unix_ms"] = (Json::Int64)wf.deadline_unix_ms;
    if (!wf.idempotency_key.empty()) row["idempotency_key"] = wf.idempotency_key;
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

}  // namespace agentd
