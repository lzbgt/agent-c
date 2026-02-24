#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "agent_db.h"

namespace agentd {

void handle_moderator_directive_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_moderator_task_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_moderator_events_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
