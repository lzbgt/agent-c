#pragma once

#include "daemon_config.h"
#include "session_voice_start_plan.h"

#include <json/json.h>

#include <string>

namespace agentd {

Json::Value session_voice_builtin_start_contract_json(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
);

}  // namespace agentd
