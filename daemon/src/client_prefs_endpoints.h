#pragma once

#include "agent_db.h"
#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"

namespace agentd {

void handle_client_prefs_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_client_prefs_post_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
