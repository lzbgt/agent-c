#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "cors.h"
#include "daemon_config.h"

#include <json/json.h>

namespace agentd {

Json::Value session_voice_webrtc_backend_metadata_json(const DaemonConfig& cfg);

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

bool cleanup_session_voice_webrtc_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  const std::string& broker_token,
  Json::Value* out_summary,
  std::string* out_err
);

}  // namespace agentd
