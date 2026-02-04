#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "http_server.h"

namespace agentd {

// POST /api/v1/memory/consolidate
void handle_memory_consolidate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

