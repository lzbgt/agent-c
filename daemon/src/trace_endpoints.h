#pragma once

#include <string>

namespace agentd {

struct CorsConfig;
struct DaemonConfig;
struct HttpRequest;
struct HttpResponse;
class AgentDb;

// GET /api/v1/trace?trace_id=...
//
// Returns a best-effort list of audit records whose record_json contains the given trace_id.
// This is intended for debugging and correlated timeline UIs.
void handle_trace_lookup_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

