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
  out["count"] = (Json::UInt64)rows.size();
  Json::Value recs(Json::arrayValue);
  for (const auto& s : rows) {
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
  out["records"] = recs;
  resp->body = json_stringify(out);
}

}  // namespace agentd
