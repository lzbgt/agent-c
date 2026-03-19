#pragma once

#include "agent_db.h"
#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"

#include <json/json.h>

#include <string>

namespace agentd {

void handle_edge_node_consensus_runtime_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_edge_node_consensus_runtime_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
