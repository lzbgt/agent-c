#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

// GET /api/v1/runtime_skills?kind=...&category=...
void handle_runtime_skill_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// POST /api/v1/runtime_skills/resolve
void handle_runtime_skill_resolve_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
