#pragma once

#include "agent_db.h"
#include "daemon_config.h"

#include <json/json.h>

#include <string>

namespace agentd {

bool cleanup_session_voice_webrtc_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  const std::string& broker_token,
  Json::Value* out_summary,
  std::string* out_err
);

}  // namespace agentd
