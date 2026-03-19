#pragma once

#include "session_voice_backend_start.h"
#include "session_voice_broker_session.h"
#include "session_voice_child_runtime.h"

#include <functional>
#include <memory>
#include <string>

namespace agentd {

struct VoicePeerLaunchFlowOps {
  std::function<bool(
    const VoicePeerStartPlan& start_plan,
    VoicePeerBrokerSessionBinding* out_binding,
    int* out_http_status,
    std::string* out_err)>
    resolve_broker_session;

  std::function<bool(
    const std::string& session_id,
    const VoicePeerStartPlan& start_plan,
    const VoicePeerBrokerSessionBinding& binding,
    std::shared_ptr<VoicePeerRuntime>* out_state,
    std::string* out_err)>
    launch_runtime;

  std::function<void(
    const std::string& session_id,
    const std::shared_ptr<VoicePeerRuntime>& runtime)>
    register_runtime;

  std::function<void(const VoicePeerRuntime& runtime)> persist_runtime;

  std::function<VoicePeerStartupWaitResult(
    const std::shared_ptr<VoicePeerRuntime>& runtime,
    int64_t timeout_ms)>
    wait_startup;

  std::function<void(const std::shared_ptr<VoicePeerRuntime>& runtime)> sync_runtime_state;

  std::function<bool(
    const std::string& session_id,
    const std::string& broker_token,
    Json::Value* out_cleanup,
    std::string* out_err)>
    cleanup_runtime;

  std::function<bool(
    const VoicePeerStartPlan& start_plan,
    const VoicePeerBrokerSessionBinding& binding,
    std::string* out_err)>
    release_managed_broker_session;
};

bool run_voice_peer_launch_flow_with_ops(
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerLaunchFlowOps& ops,
  VoicePeerBackendStartResult* out_result
);

Json::Value voice_peer_launch_startup_sequence_json(
  const VoicePeerStartPlan& start_plan,
  bool runtime_launch_deferred
);

}  // namespace agentd
