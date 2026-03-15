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

Json::Value edge_consensus_runtime_status_json_for_node(const DaemonConfig& cfg, AgentDb* db_or_null, const std::string& node_id);
Json::Value edge_consensus_runtime_backend_metadata_json(const DaemonConfig& cfg);

}  // namespace agentd
