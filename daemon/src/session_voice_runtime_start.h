#pragma once

#include "agent_db.h"
#include "agentd/http_types.h"
#include "daemon_config.h"

#include <json/json.h>

#include <string>

namespace agentd {

void handle_session_voice_webrtc_peer_start_action(
  const DaemonConfig& cfg,
  AgentDb* db,
  const Json::Value& body,
  const std::string& session_id,
  const std::string& request_broker_token,
  Json::Value* out,
  HttpResponse* resp
);

}  // namespace agentd
