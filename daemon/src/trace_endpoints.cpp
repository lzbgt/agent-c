#include "trace_endpoints.h"

#include "agent_db.h"
#include "cors.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"
#include "trace_id_util.h"
#include "workflow_memory_correlate.h"
#include "workflow_event_schema.h"

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace agentd {
namespace {

static size_t clamp_size(size_t v, size_t lo, size_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool query_bool(const HttpRequest& req, const char* name, bool default_value) {
  const auto v = query_get(req.query, name);
  if (!v) return default_value;
  if (v->empty()) return true;
  std::string s = *v;
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off") return false;
  return default_value;
}

static int64_t query_i64(const HttpRequest& req, const char* name, int64_t default_value) {
  const auto v = query_get(req.query, name);
  if (!v || v->empty()) return default_value;
  try {
    return (int64_t)std::stoll(*v);
  } catch (...) {
    return default_value;
  }
}

static int query_i32(const HttpRequest& req, const char* name, int default_value) {
  const auto v = query_get(req.query, name);
  if (!v || v->empty()) return default_value;
  try {
    return (int)std::stol(*v);
  } catch (...) {
    return default_value;
  }
}

}  // namespace

void handle_trace_lookup_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto tid_q = query_get(req.query, "trace_id");
  const std::string trace_id = tid_q ? trim_copy(*tid_q) : std::string();
  if (trace_id.empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing trace_id"})";
    return;
  }
  if (!trace_id_is_safe(trace_id)) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"invalid trace_id"})";
    return;
  }

  if (!db_or_null || !db_or_null->is_open()) {
    resp->status = 500;
    resp->body = R"({"ok":false,"error":"db not available"})";
    return;
  }

  const bool include_memory = query_bool(req, "include_memory", /*default_value=*/false);

  size_t max_records = 50;
  if (const auto lim_q = query_get(req.query, "limit"); lim_q && !lim_q->empty()) {
    try {
      max_records = (size_t)std::stoull(*lim_q);
    } catch (...) {
      max_records = 50;
    }
  }
  max_records = clamp_size(max_records, 1, 500);

  size_t max_bytes = 512 * 1024;
  if (const auto mb_q = query_get(req.query, "max_bytes"); mb_q && !mb_q->empty()) {
    try {
      max_bytes = (size_t)std::stoull(*mb_q);
    } catch (...) {
      max_bytes = 512 * 1024;
    }
  }
  max_bytes = clamp_size(max_bytes, 1024, 2 * 1024 * 1024);

  std::vector<std::string> rows;
  std::string db_err;
  if (!db_or_null->read_audit_records_by_trace_id(trace_id, max_bytes, max_records, &rows, &db_err)) {
    resp->status = 500;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = db_err.empty() ? "trace query failed" : db_err;
    resp->body = json_stringify(o);
    return;
  }

  Json::Value out(Json::objectValue);
  out["ok"] = true;
  out["trace_id"] = trace_id;
  Json::Value recs(Json::arrayValue);
  size_t used_bytes = 0;
  for (const auto& s : rows) {
    used_bytes += s.size();
    Json::Value v;
    std::string perr;
    if (!json_parse_object(s, &v, &perr)) {
      Json::Value bad(Json::objectValue);
      bad["ok"] = false;
      bad["error"] = "failed to parse audit record";
      bad["parse_error"] = perr;
      bad["raw"] = s;
      recs.append(bad);
      continue;
    }
    recs.append(v);
  }

  // Best-effort: also surface edge interop trace records (task events and inbound envelopes).
  // This makes trace_id correlation usable across the platform ↔ node boundary without a schema migration.
  {
    const size_t remaining_bytes = (used_bytes >= max_bytes) ? 0 : (max_bytes - used_bytes);
    const size_t remaining_records = (rows.size() >= max_records) ? 0 : (max_records - rows.size());

    if (remaining_bytes > 0 && remaining_records > 0) {
      // Edge tasks (metadata) by indexed trace_id.
      {
        std::vector<AgentDb::EdgeTaskRow> edge_tasks;
        std::string terr;
        if (db_or_null->list_edge_tasks_by_trace_id(trace_id, remaining_records, &edge_tasks, &terr)) {
          for (const auto& t : edge_tasks) {
            // Keep these records tiny: avoid echoing payload/result JSON.
            if (used_bytes + 256 > max_bytes) break;
            Json::Value row(Json::objectValue);
            row["source"] = "edge_task";
            row["task_id"] = t.task_id;
            row["step_id"] = t.step_id;
            row["node_id"] = t.node_id;
            row["idempotency_key"] = t.idempotency_key;
            row["mode"] = t.mode;
            if (!t.tool_name.empty()) row["tool_name"] = t.tool_name;
            if (!t.resource_lock.empty()) row["resource_lock"] = t.resource_lock;
            if (!t.result_sha256.empty()) row["result_sha256"] = t.result_sha256;
            row["deadline_utc_ms"] = (Json::Int64)t.deadline_utc_ms;
            row["state"] = t.state;
            row["created_utc_ms"] = (Json::Int64)t.created_utc_ms;
            row["updated_utc_ms"] = (Json::Int64)t.updated_utc_ms;
            if (!t.error.empty()) row["error"] = t.error;
            recs.append(row);
            used_bytes += 256;
            if ((size_t)recs.size() >= max_records) break;
          }
        }
      }

      std::vector<AgentDb::EdgeTaskEventRow> edge_events;
      std::string eerr;
      if (db_or_null->read_edge_task_events_by_trace_id(trace_id, remaining_bytes, remaining_records, &edge_events, &eerr)) {
        for (const auto& ev : edge_events) {
          if (used_bytes + ev.data_json.size() > max_bytes) break;
          Json::Value row(Json::objectValue);
          row["source"] = "edge_task_event";
          row["ts_utc_ms"] = (Json::Int64)ev.ts_utc_ms;
          row["task_id"] = ev.task_id;
          row["step_id"] = ev.step_id;
          row["state"] = ev.state;
          Json::Value v;
          std::string perr;
          if (json_parse_object(ev.data_json, &v, &perr)) row["event"] = v;
          else row["event_raw"] = ev.data_json;
          recs.append(row);
          used_bytes += ev.data_json.size();
          if ((size_t)recs.size() >= max_records) break;
        }
      }
    }
  }

  {
    const size_t remaining_records = ((size_t)recs.size() >= max_records) ? 0 : (max_records - (size_t)recs.size());
    const size_t remaining_bytes = (used_bytes >= max_bytes) ? 0 : (max_bytes - used_bytes);
    if (remaining_records > 0 && remaining_bytes > 0) {
      std::vector<AgentDb::EdgeInboxMessageRow> inbox_rows;
      std::string ierr;
      if (db_or_null->read_edge_inbox_messages_by_trace_id(trace_id, remaining_bytes, remaining_records, &inbox_rows, &ierr)) {
        for (const auto& m : inbox_rows) {
          if (used_bytes + m.envelope_json.size() > max_bytes) break;
          Json::Value row(Json::objectValue);
          row["source"] = "edge_inbox_message";
          row["msg_id"] = m.msg_id;
          row["ts_utc_ms"] = (Json::Int64)m.ts_utc_ms;
          row["type"] = m.type;
          if (!m.from_id.empty()) row["from"] = m.from_id;
          if (!m.to_id.empty()) row["to"] = m.to_id;
          Json::Value env;
          std::string perr;
          if (json_parse_object(m.envelope_json, &env, &perr)) row["envelope"] = env;
          else row["envelope_raw"] = m.envelope_json;
          recs.append(row);
          used_bytes += m.envelope_json.size();
          if ((size_t)recs.size() >= max_records) break;
        }
      }
    }
  }

  // Best-effort: durable workflow events correlated by workflows.trace_id (indexed).
  {
    const size_t remaining_records = ((size_t)recs.size() >= max_records) ? 0 : (max_records - (size_t)recs.size());
    const size_t remaining_bytes = (used_bytes >= max_bytes) ? 0 : (max_bytes - used_bytes);
    if (remaining_records > 0 && remaining_bytes > 0) {
      std::vector<AgentDb::WorkflowEventRow> wf_events;
      std::string werr;
      if (db_or_null->read_workflow_events_by_trace_id(trace_id, remaining_bytes, remaining_records, &wf_events, &werr)) {
        for (const auto& ev : wf_events) {
          if (used_bytes + ev.data_json.size() > max_bytes) break;
          Json::Value row(Json::objectValue);
          row["source"] = "workflow_event";
          row["event_id"] = (Json::Int64)ev.event_id;
          row["workflow_id"] = ev.workflow_id;
          if (!ev.task_id.empty()) row["task_id"] = ev.task_id;
          row["ts_unix_ms"] = (Json::Int64)ev.ts_unix_ms;
          row["type"] = ev.type;
          if (const char* schema = workflow_event_schema_for_type(ev.type)) {
            row["schema"] = schema;
          }
          Json::Value v;
          std::string perr;
          if (json_parse_object(ev.data_json, &v, &perr)) row["event"] = v;
          else row["event_raw"] = ev.data_json;
          recs.append(row);
          used_bytes += ev.data_json.size();
          if ((size_t)recs.size() >= max_records) break;
        }
      }
    }
  }

  // Best-effort: edge workflow events correlated via edge_tasks.trace_id (or workflow_id == trace_id).
  {
    const size_t remaining_records = ((size_t)recs.size() >= max_records) ? 0 : (max_records - (size_t)recs.size());
    const size_t remaining_bytes = (used_bytes >= max_bytes) ? 0 : (max_bytes - used_bytes);
    if (remaining_records > 0 && remaining_bytes > 0) {
      std::vector<AgentDb::EdgeWorkflowEventRow> wf_events;
      std::string werr;
      if (db_or_null->read_edge_workflow_events_by_trace_id(trace_id, remaining_bytes, remaining_records, &wf_events, &werr)) {
        for (const auto& ev : wf_events) {
          if (used_bytes + ev.data_json.size() > max_bytes) break;
          Json::Value row(Json::objectValue);
          row["source"] = "edge_workflow_event";
          row["id"] = (Json::Int64)ev.id;
          row["workflow_id"] = ev.workflow_id;
          row["ts_utc_ms"] = (Json::Int64)ev.ts_utc_ms;
          row["type"] = ev.type;
          if (const char* schema = edge_workflow_event_schema_for_type(ev.type)) {
            row["schema"] = schema;
          }
          Json::Value v;
          std::string perr;
          if (json_parse_object(ev.data_json, &v, &perr)) row["event"] = v;
          else row["event_raw"] = ev.data_json;
          recs.append(row);
          used_bytes += ev.data_json.size();
          if ((size_t)recs.size() >= max_records) break;
        }
      }
    }
  }

  out["count"] = (Json::UInt64)recs.size();
  out["records"] = recs;

  // Optional: attach rolling memory correlation for this trace_id.
  //
  // This is a leverage multiplier: it turns trace_id into a "cross-layer join key" across
  // durable execution logs and rolling consolidated memory checkpoints.
  if (include_memory) {
    Json::Value mc(Json::objectValue);
    mc["trace_id"] = trace_id;
    const int64_t since_utc_ms = query_i64(req, "memory_since_utc_ms", /*default_value=*/0);
    const int64_t until_utc_ms = query_i64(req, "memory_until_utc_ms", /*default_value=*/INT64_MAX);
    const int max_entries = query_i32(req, "memory_max_entries", /*default_value=*/200);
    const bool timeline = query_bool(req, "memory_timeline", /*default_value=*/false);
    mc["since_utc_ms"] = (Json::Int64)since_utc_ms;
    mc["until_utc_ms"] = (Json::Int64)until_utc_ms;
    mc["max_entries"] = max_entries;
    mc["timeline"] = timeline;

    std::string merr;
    Json::Value mco = workflow_memory_correlate_to_json(cfg.state_dir, trace_id, mc, &merr);
    // Keep trace response shape lean: strip workflow-task budget counters/assistant_text.
    if (mco.isMember("tool_calls_total")) mco.removeMember("tool_calls_total");
    if (mco.isMember("steps_executed")) mco.removeMember("steps_executed");
    if (mco.isMember("assistant_text")) mco.removeMember("assistant_text");
    if (!merr.empty() && (!mco.isObject() || !mco.isMember("error"))) mco["error"] = merr;
    out["memory_correlate"] = mco;
  }

  resp->body = json_stringify(out);
}

}  // namespace agentd
