#pragma once

#include "daemon_config.h"
#include "session_voice_backend_start.h"
#include "session_voice_start_plan.h"

#include <string>

namespace agentd {

bool start_voice_peer_builtin_backend(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  VoicePeerBackendStartResult* out_result
);

}  // namespace agentd
