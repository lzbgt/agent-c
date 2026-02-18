#pragma once

#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

class AgentDb;

void handle_ota_update_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp,
  AgentDb* db_or_null
);

void handle_ota_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp,
  AgentDb* db_or_null
);

}  // namespace agentd
