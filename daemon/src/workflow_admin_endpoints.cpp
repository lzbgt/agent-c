#include "workflow_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "workflow_event_schema.h"

#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <climits>
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

}  // namespace

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
    resp->body = json_error_body("db not available");
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
    resp->body = json_error_body("missing workflow_id");
    return;
  }

  AgentDb::WorkflowRow wf;
  std::string err;
  if (!db_or_null->get_workflow(workflow_id, &wf, &err)) {
    resp->status = 404;
    resp->body = json_error_body("workflow not found");
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
    resp->body = json_error_body("db not available");
    return;
  }

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty()) {
    resp->status = 400;
    resp->body = json_error_body("missing workflow_id");
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
    if (const char* schema = workflow_event_schema_for_type(r.type)) {
      o["schema"] = schema;
    }
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

void handle_workflow_stats_endpoint(
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

  AgentDb::WorkflowSchedulerStats st;
  std::string err;
  if (!db_or_null->get_workflow_scheduler_stats(unix_ms_now(), &st, &err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "failed to read workflow scheduler stats";
    o["detail"] = err;
    resp->body = json_stringify_compact(o);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["now_unix_ms"] = (Json::Int64)st.now_unix_ms;
  out["tasks_queued_ready"] = (Json::Int64)st.tasks_queued_ready;
  out["tasks_queued_not_ready"] = (Json::Int64)st.tasks_queued_not_ready;
  {
    Json::Value m(Json::objectValue);
    for (const auto& kv : st.workflows_by_status) {
      m[kv.first] = (Json::Int64)kv.second;
    }
    out["workflows_by_status"] = m;
  }
  {
    Json::Value m(Json::objectValue);
    for (const auto& kv : st.tasks_by_status) {
      m[kv.first] = (Json::Int64)kv.second;
    }
    out["tasks_by_status"] = m;
  }

  // Optional: surface aggregate workflow budget pressure (best-effort) so schedulers/UIs can poll cheaply.
  //
  // This is intentionally a bounded scan:
  // - only considers queued|running workflows (active pressure surface)
  // - only includes workflows that define workflow_limits in their persisted spec_json
  // - uses durable per-workflow usage totals derived from retry-safe per-task cumulative counters
  const auto include_budget_q = query_get(req.query, "include_budget_pressure");
  const bool include_budget_pressure = include_budget_q && (*include_budget_q == "1" || *include_budget_q == "true");
  if (include_budget_pressure) {
    size_t max_workflows = 64;
    if (const auto lq = query_get(req.query, "budget_workflow_limit")) {
      try {
        const int n = std::stoi(*lq);
        if (n > 0) max_workflows = (size_t)std::min<int>(512, n);
      } catch (...) {
      }
    }

    const auto include_budget_workflows_q = query_get(req.query, "include_budget_workflows");
    const bool include_budget_workflows =
      include_budget_workflows_q && (*include_budget_workflows_q == "1" || *include_budget_workflows_q == "true");

    auto parse_limits_best_effort = [](const std::string& spec_json) -> Json::Value {
      Json::Value spec;
      std::string perr;
      if (!json_parse_any(spec_json, &spec, &perr) || !spec.isObject()) return Json::Value(Json::nullValue);
      if (!spec.isMember("workflow_limits") || !spec["workflow_limits"].isObject()) return Json::Value(Json::nullValue);
      return spec["workflow_limits"];
    };

    struct LimitAgg {
      int64_t workflows_limited = 0;
      int64_t workflows_remaining_zero = 0;
      int64_t workflows_ratio_le_0_1 = 0;
      int64_t workflows_ratio_le_0_2 = 0;
      int64_t min_remaining = INT64_MAX;
      double min_remaining_ratio = 1.0;
    };
    auto update_limit_agg = [](LimitAgg* agg, int64_t maxv, int64_t used) {
      if (!agg) return;
      if (maxv <= 0) return;
      const int64_t used_clamped = std::max<int64_t>(0, std::min<int64_t>(maxv, std::max<int64_t>(0, used)));
      const int64_t remaining = std::max<int64_t>(0, maxv - used_clamped);
      const double ratio = maxv > 0 ? (double)remaining / (double)maxv : 1.0;
      agg->workflows_limited++;
      if (remaining <= 0) agg->workflows_remaining_zero++;
      if (ratio <= 0.10) agg->workflows_ratio_le_0_1++;
      if (ratio <= 0.20) agg->workflows_ratio_le_0_2++;
      agg->min_remaining = std::min<int64_t>(agg->min_remaining, remaining);
      agg->min_remaining_ratio = std::min<double>(agg->min_remaining_ratio, ratio);
    };

    std::vector<AgentDb::WorkflowRow> wrows;
    wrows.reserve(max_workflows);
    {
      std::vector<AgentDb::WorkflowRow> queued;
      std::vector<AgentDb::WorkflowRow> running;
      std::string qerr, rerr;
      (void)db_or_null->list_workflows_by_status_for_scheduler("queued", max_workflows, &queued, &qerr);
      (void)db_or_null->list_workflows_by_status_for_scheduler("running", max_workflows, &running, &rerr);
      std::unordered_set<std::string> seen;
      for (const auto& r : queued) {
        if (wrows.size() >= max_workflows) break;
        if (r.workflow_id.empty()) continue;
        if (!seen.insert(r.workflow_id).second) continue;
        wrows.push_back(r);
      }
      for (const auto& r : running) {
        if (wrows.size() >= max_workflows) break;
        if (r.workflow_id.empty()) continue;
        if (!seen.insert(r.workflow_id).second) continue;
        wrows.push_back(r);
      }
    }

    int64_t scanned = 0;
    int64_t with_limits = 0;
    LimitAgg tool_calls, steps, elapsed, tokens;
    Json::Value sampled(Json::arrayValue);
    const size_t max_samples = include_budget_workflows ? 12 : 0;

    for (const auto& wf : wrows) {
      scanned++;
      const Json::Value lim = parse_limits_best_effort(wf.spec_json);
      if (!lim.isObject()) continue;
      with_limits++;

      AgentDb::WorkflowUsageTotals totals;
      std::string uerr;
      if (!db_or_null->get_workflow_usage_totals(wf.workflow_id, &totals, &uerr)) {
        continue;
      }

      auto lim_i64 = [&](const char* k, int64_t* outv) {
        if (outv) *outv = 0;
        if (!lim.isMember(k)) return;
        const auto& v = lim[k];
        if (!(v.isInt64() || v.isUInt64() || v.isInt() || v.isUInt())) return;
        const int64_t n = v.asInt64();
        if (n <= 0) return;
        if (outv) *outv = n;
      };

      int64_t max_tool_calls = 0, max_steps_total = 0, max_elapsed_ms_total = 0, max_total_tokens = 0;
      lim_i64("max_tool_calls_total", &max_tool_calls);
      lim_i64("max_steps_total", &max_steps_total);
      lim_i64("max_elapsed_ms_total", &max_elapsed_ms_total);
      lim_i64("max_total_tokens", &max_total_tokens);

      update_limit_agg(&tool_calls, max_tool_calls, totals.tool_calls_total_used);
      update_limit_agg(&steps, max_steps_total, totals.steps_total_used);
      update_limit_agg(&elapsed, max_elapsed_ms_total, totals.elapsed_ms_total_used);
      update_limit_agg(&tokens, max_total_tokens, totals.total_tokens_used);

      if (max_samples > 0 && (size_t)sampled.size() < max_samples) {
        Json::Value row(Json::objectValue);
        row["workflow_id"] = wf.workflow_id;
        row["status"] = wf.status;
        if (!wf.session_id.empty()) row["session_id"] = wf.session_id;
        if (!wf.trace_id.empty()) row["trace_id"] = wf.trace_id;
        Json::Value rem(Json::objectValue);
        if (max_tool_calls > 0) rem["max_tool_calls_total"] = (Json::Int64)std::max<int64_t>(0, max_tool_calls - std::max<int64_t>(0, totals.tool_calls_total_used));
        if (max_steps_total > 0) rem["max_steps_total"] = (Json::Int64)std::max<int64_t>(0, max_steps_total - std::max<int64_t>(0, totals.steps_total_used));
        if (max_elapsed_ms_total > 0) rem["max_elapsed_ms_total"] = (Json::Int64)std::max<int64_t>(0, max_elapsed_ms_total - std::max<int64_t>(0, totals.elapsed_ms_total_used));
        if (max_total_tokens > 0) rem["max_total_tokens"] = (Json::Int64)std::max<int64_t>(0, max_total_tokens - std::max<int64_t>(0, totals.total_tokens_used));
        if (!rem.getMemberNames().empty()) row["remaining"] = rem;
        sampled.append(row);
      }
    }

    auto agg_to_json = [](const LimitAgg& a) -> Json::Value {
      Json::Value o(Json::objectValue);
      o["workflows_limited"] = (Json::Int64)std::max<int64_t>(0, a.workflows_limited);
      o["workflows_remaining_zero"] = (Json::Int64)std::max<int64_t>(0, a.workflows_remaining_zero);
      o["workflows_ratio_le_0_1"] = (Json::Int64)std::max<int64_t>(0, a.workflows_ratio_le_0_1);
      o["workflows_ratio_le_0_2"] = (Json::Int64)std::max<int64_t>(0, a.workflows_ratio_le_0_2);
      if (a.workflows_limited > 0) {
        o["min_remaining"] = (Json::Int64)std::max<int64_t>(0, a.min_remaining == INT64_MAX ? 0 : a.min_remaining);
        o["min_remaining_ratio"] = a.min_remaining_ratio;
      }
      return o;
    };

    Json::Value bp(Json::objectValue);
    bp["workflows_scanned"] = (Json::Int64)std::max<int64_t>(0, scanned);
    bp["workflows_with_limits"] = (Json::Int64)std::max<int64_t>(0, with_limits);
    bp["workflow_limit"] = (Json::Int64)max_workflows;
    bp["include_budget_workflows"] = include_budget_workflows;
    bp["tool_calls"] = agg_to_json(tool_calls);
    bp["steps"] = agg_to_json(steps);
    bp["elapsed_ms"] = agg_to_json(elapsed);
    bp["total_tokens"] = agg_to_json(tokens);
    if (include_budget_workflows) bp["workflows"] = sampled;
    out["budget_pressure"] = bp;
  }

  // Optional: session-level inflight pressure snapshot (multi-tenant / fairness tuning).
  const auto include_sessions_q = query_get(req.query, "include_sessions");
  const bool include_sessions = include_sessions_q && (*include_sessions_q == "1" || *include_sessions_q == "true");
  if (include_sessions) {
    size_t limit = 32;
    if (const auto lq = query_get(req.query, "session_limit")) {
      try {
        const int n = std::stoi(*lq);
        if (n > 0) limit = (size_t)std::min<int>(512, n);
      } catch (...) {
      }
    }
    const auto include_no_session_q = query_get(req.query, "include_no_session");
    const bool include_no_session =
      include_no_session_q && (*include_no_session_q == "1" || *include_no_session_q == "true");

    std::vector<AgentDb::WorkflowSessionStatsRow> rows;
    std::string serr;
    if (db_or_null->list_workflow_session_stats(limit, include_no_session, &rows, &serr)) {
      Json::Value arr(Json::arrayValue);
      for (const auto& r : rows) {
        Json::Value o(Json::objectValue);
        o["session_id"] = r.session_id;
        o["inflight_tasks"] = (Json::Int64)std::max<int64_t>(0, r.inflight_tasks);
        o["queued_tasks"] = (Json::Int64)std::max<int64_t>(0, r.queued_tasks);
        o["running_tasks"] = (Json::Int64)std::max<int64_t>(0, r.running_tasks);
        o["workflows_queued"] = (Json::Int64)std::max<int64_t>(0, r.workflows_queued);
        o["workflows_running"] = (Json::Int64)std::max<int64_t>(0, r.workflows_running);
        arr.append(o);
      }
      out["session_limit"] = (Json::Int64)limit;
      out["include_no_session"] = include_no_session;
      out["sessions"] = arr;
    } else {
      Json::Value o(Json::objectValue);
      o["ok"] = false;
      o["error"] = serr;
      out["sessions_error"] = o;
    }
  }
  resp->body = json_stringify_compact(out);
}

}  // namespace agentd
