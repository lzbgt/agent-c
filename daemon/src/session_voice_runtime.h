#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

namespace agentd {

void handle_session_voice_webrtc_peer_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_session_voice_webrtc_peer_status_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  AgentDb* db,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
