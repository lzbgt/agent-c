#include "trace_endpoints.h"

#include "agent_db.h"
#include "cors.h"
#include "daemon_auth.h"
#include "http_util.h"
#include "json_util.h"
#include "string_util.h"

#include <json/json.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace agentd {
namespace {

static bool trace_id_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':' || c == '@';
    if (!ok) return false;
  }
  return true;
}

static size_t clamp_size(size_t v, size_t lo, size_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
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

  out["count"] = (Json::UInt64)recs.size();
  out["records"] = recs;
  resp->body = json_stringify(out);
}

}  // namespace agentd
