#pragma once

#include "daemon_config.h"
#include "session_voice_runtime_internal.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace agentd {

void refresh_voice_peer_runtime_state(VoicePeerRuntime* st);

void finalize_recovered_voice_peer_stop(VoicePeerRuntime* st, int signal_used);

bool wait_for_voice_peer_stop(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms
);

VoicePeerStartupWaitResult wait_for_voice_peer_startup(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms
);

bool voice_peer_spawn_process(
  const DaemonConfig& cfg,
  const VoicePeerChildLaunchConfig& launch_cfg,
  std::mutex& runtime_mu,
  const std::function<void(const VoicePeerRuntime&)>& on_exit_persist,
  std::shared_ptr<VoicePeerRuntime>* out_state,
  std::string* out_err
);

bool voice_peer_kill_best_effort(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int* out_signal_used,
  std::string* out_err
);

bool remove_voice_peer_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& session_id,
  bool* out_any_deleted,
  std::string* out_err
);

}  // namespace agentd
