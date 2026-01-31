#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"

namespace agentd {

void handle_file_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
