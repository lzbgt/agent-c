#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "http_server.h"
#include "openai_client.h"

#include <string>

namespace agentd {

void handle_run_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_run_async_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

