#include "job_stream_endpoint.h"

#include "http_util.h"

#include "job_manager.h"
#include "json_util.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace agentd {

static bool is_terminal_status(const std::string& s) {
  return s == "done" || s == "error" || s == "cancelled" || s == "interrupted";
}

static bool auth_ok(const std::string& daemon_auth_token, const HttpRequest& req) {
  if (daemon_auth_token.empty()) {
    return true;
  }
  const std::string auth = header_get_ci(req.headers, "authorization");
  const std::string got = bearer_token_from_auth_header(auth);
  return !got.empty() && got == daemon_auth_token;
}

void handle_job_stream_endpoint(
  const std::string& daemon_auth_token,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  socket_t client_fd
) {
  const std::string req_id = header_get_ci(req.headers, "x-request-id");
  if (!auth_ok(daemon_auth_token, req)) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 401 Unauthorized\r\n";
    hdr << "Content-Type: application/json; charset=utf-8\r\n";
    hdr << "WWW-Authenticate: Bearer\r\n";
    if (!req_id.empty()) hdr << "X-Request-Id: " << req_id << "\r\n";
    hdr << cors_wire_headers(req, cors_cfg);
    hdr << "Connection: close\r\n";
    hdr << "\r\n";
    hdr << json_error_body("unauthorized");
    (void)write_all_fd(client_fd, hdr.str());
    return;
  }

  const auto jid = query_get(req.query, "job_id");
  if (!jid || jid->empty()) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 400 Bad Request\r\n";
    hdr << "Content-Type: application/json; charset=utf-8\r\n";
    if (!req_id.empty()) hdr << "X-Request-Id: " << req_id << "\r\n";
    hdr << cors_wire_headers(req, cors_cfg);
    hdr << "Connection: close\r\n";
    hdr << "\r\n";
    hdr << json_error_body("missing job_id");
    (void)write_all_fd(client_fd, hdr.str());
    return;
  }

  uint64_t cursor = 0;
  const auto cursor_q = query_get(req.query, "cursor");
  if (cursor_q && !cursor_q->empty()) {
    try {
      cursor = (uint64_t)std::stoull(*cursor_q);
    } catch (...) {
      cursor = 0;
    }
  }

  // SSE headers
  std::ostringstream hdr;
  hdr << "HTTP/1.1 200 OK\r\n";
  hdr << "Content-Type: text/event-stream\r\n";
  hdr << "Cache-Control: no-cache\r\n";
  hdr << "Connection: keep-alive\r\n";
  if (!req_id.empty()) hdr << "X-Request-Id: " << req_id << "\r\n";
  hdr << cors_wire_headers(req, cors_cfg);
  hdr << "\r\n";
  if (!write_all_fd(client_fd, hdr.str())) {
    return;
  }

  auto last_send = std::chrono::steady_clock::now();
  for (;;) {
    // Pull a bounded slice of events to avoid copying huge job state blobs under lock.
    JobSnapshot s;
    if (!job_get_snapshot(*jid, cursor, /*max_events=*/256, /*include_events=*/true, &s)) {
      // Fallback: DB-only jobs (daemon restarted). If terminal, emit job_done immediately.
      if (db_or_null && db_or_null->is_open()) {
        AgentDb::JobRow jr;
        std::string db_err;
        if (db_or_null->get_job(*jid, &jr, &db_err)) {
          if (is_terminal_status(jr.status)) {
            Json::Value out(Json::objectValue);
            out["ok"] = (jr.status == "done");
            out["job_id"] = jr.job_id;
            out["status"] = jr.status;
            out["error"] = jr.error;
            Json::Value r(Json::objectValue);
            if (!jr.result_json.empty()) {
              std::string perr;
              if (!json_parse_object(jr.result_json, &r, &perr)) {
                r = Json::Value(Json::objectValue);
                r["ok"] = false;
                r["error"] = "failed to parse persisted job result";
                r["parse_error"] = perr;
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
            out["result"] = r;
            out["events_cursor_next"] = (Json::UInt64)cursor;
            (void)sse_send(client_fd, "job_done", json_stringify(out));
            return;
          }
          (void)sse_send(client_fd, "error", json_error_body("job not in memory (daemon restarted?)"));
          return;
        }
      }

      (void)sse_send(client_fd, "error", json_error_body("job not found"));
      return;
    }

    const uint64_t base = s.events_cursor_base;
    const uint64_t end = s.events_cursor_end;

    if (cursor < base) {
      Json::Value d(Json::objectValue);
      d["ok"] = true;
      d["reason"] = "cursor_too_old";
      d["cursor_base"] = (Json::UInt64)base;
      d["cursor_end"] = (Json::UInt64)end;
      if (!sse_send(client_fd, "reset", json_stringify(d))) {
        return;
      }
      cursor = base;
      last_send = std::chrono::steady_clock::now();
    }

    bool sent_any = false;
    if (s.events_included && s.events.isArray() && s.events.size() > 0) {
      const uint64_t start = s.events_cursor_start;
      const uint64_t next = s.events_cursor_next;
      for (Json::ArrayIndex i = 0; i < s.events.size(); i++) {
        const uint64_t ev_cursor = start + (uint64_t)i;
        const Json::Value& ev = s.events[i];
        if (!sse_send(client_fd, "agent_event", json_stringify(ev), std::to_string((unsigned long long)ev_cursor))) {
          return;
        }
        sent_any = true;
      }
      cursor = next;
    }
    if (sent_any) {
      last_send = std::chrono::steady_clock::now();
    }

    if (is_terminal_status(s.status)) {
      Json::Value out(Json::objectValue);
      out["ok"] = (s.status == "done");
      out["job_id"] = s.id;
      if (!s.trace_id.empty()) out["trace_id"] = s.trace_id;
      out["status"] = s.status;
      out["error"] = s.error;
      out["result"] = s.result;
      out["events_cursor_next"] = (Json::UInt64)cursor;
      (void)sse_send(client_fd, "job_done", json_stringify(out));
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_send).count() >= 15) {
      if (!sse_ping(client_fd)) {
        return;
      }
      last_send = now;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

}  // namespace agentd
