#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "session_voice_runtime_internal.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace agentd {

bool lookup_or_recover_voice_peer_runtime(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& session_id,
  bool register_running_persisted,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  Json::Value* out_updates,
  std::string* out_err
);

}  // namespace agentd
