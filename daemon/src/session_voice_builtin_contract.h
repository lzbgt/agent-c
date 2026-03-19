#pragma once

#include "daemon_config.h"
#include "session_voice_start_plan.h"
#include "session_voice_runtime_internal.h"

#include <json/json.h>

#include <string>

namespace agentd {

struct VoicePeerBuiltinStartPreview {
  Json::Value contract = Json::Value(Json::objectValue);
  VoicePeerRuntime planned_runtime;
};

VoicePeerBuiltinStartPreview build_voice_peer_builtin_start_preview(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
);

Json::Value session_voice_builtin_start_contract_json(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan
);

}  // namespace agentd
