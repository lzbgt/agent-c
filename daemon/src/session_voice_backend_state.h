#pragma once

#include "daemon_config.h"
#include "session_voice_runtime_internal.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace agentd {

void refresh_voice_peer_runtime_backend_state(VoicePeerRuntime* st);

bool stop_voice_peer_runtime_backend_process(
  const std::shared_ptr<VoicePeerRuntime>& st,
  std::mutex& runtime_mu,
  int64_t timeout_ms,
  bool was_running,
  bool* out_stopped,
  std::string* out_err
);

bool remove_voice_peer_runtime_backend_artifacts(
  const DaemonConfig& cfg,
  const std::string& session_id,
  const std::string& runtime_kind,
  bool* out_any_deleted,
  std::string* out_err
);

}  // namespace agentd
