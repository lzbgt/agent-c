#pragma once

#include "daemon_config.h"
#include "cors.h"
#include "http_server.h"

#include "agent_db.h"

#include <string>

namespace agentd {

void handle_db_runs_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_db_run_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_db_artifacts_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

