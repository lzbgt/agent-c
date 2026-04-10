#pragma once

#include "daemon_config.h"
#include "session_voice_broker_session.h"
#include "session_voice_runtime_internal.h"
#include "session_voice_start_plan.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace agentd {

bool start_builtin_voice_peer_runtime_service(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const VoicePeerStartPlan& start_plan,
  const VoicePeerBrokerSessionBinding& binding,
  std::mutex& runtime_mu,
  const std::function<void(const VoicePeerRuntime&)>& persist_runtime,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  std::string* out_err
);

void refresh_builtin_voice_peer_runtime_state(VoicePeerRuntime* st);

bool stop_builtin_voice_peer_runtime_service(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  bool* out_stopped,
  std::string* out_err
);

}  // namespace agentd
