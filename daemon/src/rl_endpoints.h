#pragma once

#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

// GET /api/v1/rl/experience_records
void handle_rl_experience_records_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
