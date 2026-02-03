#include "job_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>

namespace agentd {

static bool is_terminal_status(const std::string& s) {
  return s == "done" || s == "error" || s == "cancelled" || s == "interrupted";
}

void handle_job_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto jid = query_get(req.query, "job_id");
  if (!jid || jid->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing job_id"})";
    return;
  }

  // Optional live events for progress (polling UI).
  const auto include_ev = query_get(req.query, "include_events");
  const bool want_events = include_ev && (*include_ev == "1" || *include_ev == "true");
  const auto cursor_q = query_get(req.query, "cursor");
  const auto max_q = query_get(req.query, "max_events");
  uint64_t cursor = 0;
  if (cursor_q && !cursor_q->empty()) {
    try {
      cursor = (uint64_t)std::stoull(*cursor_q);
    } catch (...) {
      cursor = 0;
    }
  }
  size_t max_events = 256;
  if (max_q && !max_q->empty()) {
    try {
      max_events = (size_t)std::stoull(*max_q);
    } catch (...) {
      max_events = 256;
    }
  }
  if (max_events == 0) max_events = 256;
  max_events = std::min<size_t>(max_events, 2048);

  JobSnapshot s;
  if (job_get_snapshot(*jid, cursor, max_events, want_events, &s)) {
    Json::Value o(Json::objectValue);
    o["ok"] = true;
    o["job_id"] = s.id;
    o["status"] = s.status;
    o["error"] = s.error;
    o["cancel_requested"] = s.cancel_requested;
    o["created_unix_ms"] = (Json::Int64)s.created_unix_ms;
    o["updated_unix_ms"] = (Json::Int64)s.updated_unix_ms;

    o["events_cursor_base"] = (Json::UInt64)s.events_cursor_base;
    o["events_cursor_end"] = (Json::UInt64)s.events_cursor_end;
    if (want_events && s.events_included) {
      o["events"] = s.events;
      o["events_cursor_next"] = (Json::UInt64)s.events_cursor_next;
      o["events_reset"] = s.events_reset;
    }

    if (is_terminal_status(s.status)) {
      o["result"] = s.result;
    }
    resp->body = json_stringify(o);
    return;
  }

  // Fallback: durable DB lookup so jobs survive daemon restarts.
  if (db_or_null && db_or_null->is_open()) {
    AgentDb::JobRow jr;
    std::string db_err;
    if (db_or_null->get_job(*jid, &jr, &db_err)) {
      Json::Value o(Json::objectValue);
      o["ok"] = true;
      o["job_id"] = jr.job_id;
      o["status"] = jr.status;
      o["error"] = jr.error;
      o["stop_reason"] = jr.stop_reason;
      o["cancel_requested"] = jr.cancel_requested;
      o["created_unix_ms"] = (Json::Int64)jr.created_unix_ms;
      o["updated_unix_ms"] = (Json::Int64)jr.updated_unix_ms;
      if (jr.last_heartbeat_unix_ms > 0) {
        o["last_heartbeat_unix_ms"] = (Json::Int64)jr.last_heartbeat_unix_ms;
      }

      // No durable per-job event log yet; expose empty cursor space for UIs.
      o["events_cursor_base"] = (Json::UInt64)0;
      o["events_cursor_end"] = (Json::UInt64)0;
      if (want_events) {
        o["events"] = Json::Value(Json::arrayValue);
        o["events_cursor_next"] = (Json::UInt64)0;
        o["events_reset"] = cursor != 0;
      }

      if (is_terminal_status(jr.status)) {
        Json::Value r(Json::objectValue);
        if (!jr.result_json.empty()) {
          std::string perr2;
          if (!json_parse_object(jr.result_json, &r, &perr2)) {
            r = Json::Value(Json::objectValue);
            r["ok"] = false;
            r["error"] = "failed to parse persisted job result";
            r["parse_error"] = perr2;
          }
        } else if (jr.status == "interrupted") {
          r["ok"] = false;
          r["interrupted"] = true;
          r["error"] = jr.error.empty() ? "interrupted by restart" : jr.error;
          if (!jr.stop_reason.empty()) r["stop_reason"] = jr.stop_reason;
        } else {
          r["ok"] = false;
          r["error"] = jr.error.empty() ? "job finished but missing persisted result" : jr.error;
        }
        o["result"] = r;
      }

      resp->body = json_stringify(o);
      return;
    }
  }

  resp->status = 404;
  resp->body = R"({"ok":false,"error":"job not found"})";
}

void handle_job_cancel_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto jid = query_get(req.query, "job_id");
  if (!jid || jid->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing job_id"})";
    return;
  }
  if (!job_request_cancel(*jid)) {
    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "cannot cancel job (not found or already finished)";
    o["job_id"] = *jid;
    resp->body = json_stringify(o);
    return;
  }
  // Best-effort: surface cancellation request to any connected UI.
  {
    Json::Value d(Json::objectValue);
    d["job_id"] = *jid;
    d["ts_unix_ms"] = (Json::Int64)now_unix_ms();
    job_append_event(*jid, "cancel_requested", json_stringify(d));
  }

  if (db_or_null && db_or_null->is_open()) {
    AgentDb::JobRow jr;
    std::string db_err;
    if (db_or_null->get_job(*jid, &jr, &db_err)) {
      jr.updated_unix_ms = now_unix_ms();
      jr.cancel_requested = true;
      // Preserve status as-is when known.
      JobState js;
      if (job_get(*jid, &js)) {
        if (!js.status.empty()) jr.status = js.status;
      }
      (void)db_or_null->upsert_job(jr, &db_err);
    }
  }

  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = *jid;
  resp->body = json_stringify(o);
}

void handle_job_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
) {
  cors_apply(req, resp, cors_cfg);
  resp->headers["Content-Type"] = "application/json; charset=utf-8";
  if (!daemon_require_auth(cfg, req, resp)) return;

  const auto jid = query_get(req.query, "job_id");
  if (!jid || jid->empty()) {
    resp->status = 400;
    resp->body = R"({"ok":false,"error":"missing job_id"})";
    return;
  }
  if (!job_delete(*jid)) {
    // Fallback: allow deleting durable (DB-only) finished jobs.
    if (db_or_null && db_or_null->is_open()) {
      AgentDb::JobRow jr;
      std::string db_err;
      if (db_or_null->get_job(*jid, &jr, &db_err) && is_terminal_status(jr.status)) {
        (void)db_or_null->delete_job(*jid, &db_err);
        Json::Value o(Json::objectValue);
        o["ok"] = true;
        o["job_id"] = *jid;
        resp->body = json_stringify(o);
        return;
      }
    }

    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "cannot delete job (still running or not found)";
    o["job_id"] = *jid;
    resp->body = json_stringify(o);
    return;
  }

  if (db_or_null && db_or_null->is_open()) {
    std::string db_err;
    (void)db_or_null->delete_job(*jid, &db_err);
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = *jid;
  resp->body = json_stringify(o);
}

}  // namespace agentd
