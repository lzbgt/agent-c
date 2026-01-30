#pragma once

#include "cors.h"
#include "http_server.h"

#include <string>

namespace agentd {

// Handles GET /api/v1/job/stream (SSE) after minimal HTTP routing.
// Auth is controlled by the daemon auth token (Authorization: Bearer ...).
void handle_job_stream_endpoint(
  const std::string& daemon_auth_token,
  const CorsConfig& cors_cfg,
  const HttpRequest& req,
  int client_fd
);

}  // namespace agentd

