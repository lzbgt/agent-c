#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "http_server.h"

#include <string>

namespace agentd {

void handle_sessions_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_get_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_audit_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_delete_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd

