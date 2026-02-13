#pragma once

#include "daemon_config.h"
#include "agentd/http_types.h"
#include "cors.h"

#include <chrono>

namespace agentd {

void handle_caps_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  std::chrono::steady_clock::time_point start_time,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
