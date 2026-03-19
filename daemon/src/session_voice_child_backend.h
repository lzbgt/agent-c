#pragma once

#include "daemon_config.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_start_plan.h"

#include <json/json.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace agentd {

struct VoicePeerChildBackendStartResult {
  bool ok = false;
  int http_status = 500;
  bool startup_confirmed = false;
  std::string error;
  std::shared_ptr<VoicePeerRuntime> state;
  Json::Value startup_cleanup = Json::Value(Json::nullValue);
};

bool start_voice_peer_child_backend(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  std::mutex& runtime_mu,
  const std::function<void(const std::string&, const std::shared_ptr<VoicePeerRuntime>&)>& register_runtime,
  const std::function<void(const VoicePeerRuntime&)>& persist_runtime,
  const std::function<bool(const std::string&, const std::string&, Json::Value*, std::string*)>& cleanup_runtime,
  VoicePeerChildBackendStartResult* out_result
);

}  // namespace agentd
