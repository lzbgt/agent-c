#include "job_stream_endpoint.h"

#include "http_util.h"

#include "job_manager.h"
#include "json_util.h"

#include <chrono>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace agentd {

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
  const HttpRequest& req,
  int client_fd
) {
  if (!auth_ok(daemon_auth_token, req)) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 401 Unauthorized\r\n";
    hdr << "Content-Type: application/json; charset=utf-8\r\n";
    hdr << "WWW-Authenticate: Bearer\r\n";
    hdr << cors_wire_headers(req, cors_cfg);
    hdr << "Connection: close\r\n";
    hdr << "\r\n";
    hdr << "{\"ok\":false,\"error\":\"unauthorized\"}";
    (void)write_all_fd(client_fd, hdr.str());
    return;
  }

  const auto jid = query_get(req.query, "job_id");
  if (!jid || jid->empty()) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 400 Bad Request\r\n";
    hdr << "Content-Type: application/json; charset=utf-8\r\n";
    hdr << cors_wire_headers(req, cors_cfg);
    hdr << "Connection: close\r\n";
    hdr << "\r\n";
    hdr << "{\"ok\":false,\"error\":\"missing job_id\"}";
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
  hdr << cors_wire_headers(req, cors_cfg);
  hdr << "\r\n";
  if (!write_all_fd(client_fd, hdr.str())) {
    return;
  }

  auto last_send = std::chrono::steady_clock::now();
  for (;;) {
    JobState s;
    if (!job_get(*jid, &s)) {
      (void)sse_send(client_fd, "error", R"({"ok":false,"error":"job not found"})");
      return;
    }

    const uint64_t base = s.events_offset;
    const uint64_t end = base + (uint64_t)s.events.size();

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
    while (cursor < end) {
      const uint64_t idx = cursor - base;
      const Json::Value& ev = s.events[(Json::ArrayIndex)idx];
      if (!sse_send(client_fd, "agent_event", json_stringify(ev), std::to_string((unsigned long long)cursor))) {
        return;
      }
      cursor++;
      sent_any = true;
    }
    if (sent_any) {
      last_send = std::chrono::steady_clock::now();
    }

    if (s.status == "done" || s.status == "error") {
      Json::Value out(Json::objectValue);
      out["ok"] = (s.status == "done");
      out["job_id"] = s.id;
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
