#pragma once

#include "daemon_config.h"
#include "session_voice_backend_start.h"
#include "session_voice_start_plan.h"

#include <functional>
#include <mutex>
#include <string>

namespace agentd {

bool start_voice_peer_child_backend(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  std::mutex& runtime_mu,
  const std::function<void(const std::string&, const std::shared_ptr<VoicePeerRuntime>&)>& register_runtime,
  const std::function<void(const VoicePeerRuntime&)>& persist_runtime,
  const std::function<bool(const std::string&, const std::string&, Json::Value*, std::string*)>& cleanup_runtime,
  VoicePeerBackendStartResult* out_result
);

}  // namespace agentd
