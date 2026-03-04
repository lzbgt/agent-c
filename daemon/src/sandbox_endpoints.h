#pragma once

#include <string>

namespace agentd {

struct CorsConfig;
struct DaemonConfig;
struct HttpRequest;
struct HttpResponse;

void handle_sandbox_mount_validate_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
