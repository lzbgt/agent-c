#include "edge_interop_endpoints.h"

#include "http_util.h"

#include "daemon_auth.h"
#include "edge_util.h"
#include "job_manager.h" // sse_send/sse_ping/write_all_fd
#include "json_util.h"
#include "workflow_event_schema.h"

#include <chrono>
#include <sstream>
#include <thread>

namespace agentd {
namespace {

static bool is_terminal_edge_workflow_status(const std::string& s) {
  return s == "SUCCEEDED" || s == "FAILED" || s == "CANCELED";
}

static bool auth_ok(const std::string& daemon_auth_token, const HttpRequest& req) {
  if (daemon_auth_token.empty()) return true;
  const std::string auth = header_get_ci(req.headers, "authorization");
  const std::string got = bearer_token_from_auth_header(auth);
  return !got.empty() && got == daemon_auth_token;
}

static std::string json_stringify_compact(const Json::Value& v) {
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  return Json::writeString(wb, v);
}

}  // namespace

void handle_edge_workflow_stream_endpoint(
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

  if (!db_or_null || !db_or_null->is_open()) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 503 Service Unavailable\r\n";
    hdr << "Content-Type: application/json; charset=utf-8\r\n";
    if (!req_id.empty()) hdr << "X-Request-Id: " << req_id << "\r\n";
    hdr << cors_wire_headers(req, cors_cfg);
    hdr << "Connection: close\r\n";
    hdr << "\r\n";
    hdr << json_error_body("db not available");
    (void)write_all_fd(client_fd, hdr.str());
    return;
  }

  const auto wid = query_get(req.query, "workflow_id");
  if (!wid || wid->empty() || !edge_id_is_safe(*wid)) {
    std::ostringstream hdr;
    hdr << "HTTP/1.1 400 Bad Request\r\n";
    hdr << "Content-Type: application/json; charset=utf-8\r\n";
    if (!req_id.empty()) hdr << "X-Request-Id: " << req_id << "\r\n";
    hdr << cors_wire_headers(req, cors_cfg);
    hdr << "Connection: close\r\n";
    hdr << "\r\n";
    hdr << json_error_body("missing/invalid workflow_id");
    (void)write_all_fd(client_fd, hdr.str());
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

  std::ostringstream hdr;
  hdr << "HTTP/1.1 200 OK\r\n";
  hdr << "Content-Type: text/event-stream\r\n";
  hdr << "Cache-Control: no-cache\r\n";
  hdr << "Connection: keep-alive\r\n";
  if (!req_id.empty()) hdr << "X-Request-Id: " << req_id << "\r\n";
  hdr << cors_wire_headers(req, cors_cfg);
  hdr << "\r\n";
  if (!write_all_fd(client_fd, hdr.str())) return;

  auto last_send = std::chrono::steady_clock::now();
  for (;;) {
    std::vector<AgentDb::EdgeWorkflowEventRow> rows;
    std::string err;
    if (!db_or_null->list_edge_workflow_events(*wid, cursor, /*max_rows=*/256, &rows, &err)) {
      (void)sse_send(client_fd, "error", json_error_body("failed to list edge workflow events"));
      return;
    }

    bool sent_any = false;
    for (const auto& r : rows) {
      Json::Value ev(Json::objectValue);
      ev["workflow_id"] = r.workflow_id;
      ev["id"] = (Json::Int64)r.id;
      ev["ts_utc_ms"] = (Json::Int64)r.ts_utc_ms;
      ev["type"] = r.type;
      if (const char* schema = edge_workflow_event_schema_for_type(r.type)) {
        ev["schema"] = schema;
      }

      Json::Value data;
      std::string perr;
      if (!json_parse_any(r.data_json, &data, &perr)) {
        Json::Value bad(Json::objectValue);
        bad["ok"] = false;
        bad["error"] = "failed to parse data_json";
        bad["parse_error"] = perr;
        bad["raw"] = r.data_json;
        data = bad;
      }
      ev["data"] = data;

      if (!sse_send(client_fd, "edge_workflow_event", json_stringify_compact(ev), std::to_string((long long)r.id))) {
        return;
      }
      sent_any = true;
      if (r.id > cursor) cursor = r.id;
    }
    if (sent_any) last_send = std::chrono::steady_clock::now();

    AgentDb::EdgeWorkflowRow wf;
    if (db_or_null->get_edge_workflow(*wid, &wf, &err)) {
      if (is_terminal_edge_workflow_status(wf.status)) {
        Json::Value out(Json::objectValue);
        out["ok"] = (wf.status == "SUCCEEDED");
        out["workflow_id"] = wf.workflow_id;
        out["status"] = wf.status;
        if (!wf.error.empty()) out["error"] = wf.error;
        out["cursor_next"] = (Json::Int64)cursor;
        (void)sse_send(client_fd, "edge_workflow_done", json_stringify_compact(out));
        return;
      }
    } else {
      (void)sse_send(client_fd, "error", json_error_body("workflow not found"));
      return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_send).count() >= 15) {
      if (!sse_ping(client_fd)) return;
      last_send = now;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

}  // namespace agentd
