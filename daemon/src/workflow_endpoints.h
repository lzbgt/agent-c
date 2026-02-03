#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

// POST /api/v1/workflow/submit
void handle_workflow_submit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/workflow?workflow_id=...
void handle_workflow_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/workflows?status=queued|running|done|error|cancelled&limit=...
void handle_workflow_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// POST /api/v1/workflow/cancel
void handle_workflow_cancel_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

