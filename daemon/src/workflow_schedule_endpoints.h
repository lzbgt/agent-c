#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

// POST /api/v1/workflow_schedules
void handle_workflow_schedule_create_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/workflow_schedules?status=...&limit=...&offset=...
void handle_workflow_schedule_list_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/workflow_schedule?schedule_id=...
void handle_workflow_schedule_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// DELETE /api/v1/workflow_schedule?schedule_id=...
void handle_workflow_schedule_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// POST /api/v1/workflow_schedule/pause
void handle_workflow_schedule_pause_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// POST /api/v1/workflow_schedule/resume
void handle_workflow_schedule_resume_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

// GET /api/v1/workflow_schedule/runs?schedule_id=...&limit=...&offset=...
void handle_workflow_schedule_runs_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
