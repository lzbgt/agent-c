#pragma once

#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

void handle_ota_update_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_ota_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
