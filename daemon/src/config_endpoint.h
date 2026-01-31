#pragma once

#include "cors.h"
#include "config_store.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "agent_db.h"

namespace agentd {

void handle_config_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

// Updates daemon-side defaults at runtime (model/base_url/timeout/proxy + optional provider keys).
// Requires daemon auth when enabled; otherwise allowed for local loopback dev.
void handle_config_update_endpoint(
  DaemonConfigStore* cfg_store,
  AgentDb* db,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
