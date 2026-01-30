#include "job_endpoints.h"

#include "daemon_auth.h"
#include "http_util.h"
#include "job_manager.h"
#include "json_util.h"

#include <json/json.h>

#include <algorithm>

namespace agentd {

void handle_job_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
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
  JobState s;
  if (!job_get(*jid, &s)) {
    resp->status = 404;
    resp->body = R"({"ok":false,"error":"job not found"})";
    return;
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = s.id;
  o["status"] = s.status;
  o["error"] = s.error;
  o["created_unix_ms"] = (Json::Int64)s.created_unix_ms;
  o["updated_unix_ms"] = (Json::Int64)s.updated_unix_ms;

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

  o["events_cursor_base"] = (Json::UInt64)s.events_offset;
  o["events_cursor_end"] = (Json::UInt64)(s.events_offset + (uint64_t)s.events.size());
  if (want_events) {
    Json::Value slice(Json::arrayValue);
    const uint64_t base = s.events_offset;
    const uint64_t end = base + (uint64_t)s.events.size();
    bool reset = false;
    uint64_t cur = cursor;
    if (cur < base) {
      reset = true;
      cur = base;
    }
    if (cur > end) {
      cur = end;
    }
    const uint64_t start_idx = cur - base;
    const uint64_t avail = end - cur;
    const uint64_t take = std::min<uint64_t>((uint64_t)max_events, avail);
    for (uint64_t i = 0; i < take; i++) {
      slice.append(s.events[(Json::ArrayIndex)(start_idx + i)]);
    }
    o["events"] = slice;
    o["events_cursor_next"] = (Json::UInt64)(cur + take);
    o["events_reset"] = reset;
  }

  if (s.status == "done" || s.status == "error") {
    o["result"] = s.result;
  }
  resp->body = json_stringify(o);
}

void handle_job_cancel_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
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
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = *jid;
  resp->body = json_stringify(o);
}

void handle_job_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
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
    resp->status = 409;
    Json::Value o(Json::objectValue);
    o["ok"] = false;
    o["error"] = "cannot delete job (still running or not found)";
    o["job_id"] = *jid;
    resp->body = json_stringify(o);
    return;
  }
  Json::Value o(Json::objectValue);
  o["ok"] = true;
  o["job_id"] = *jid;
  resp->body = json_stringify(o);
}

}  // namespace agentd

