#pragma once

#include "daemon_config.h"
#include "session_voice_runtime_internal.h"

#include <json/json.h>

#include <string>

namespace agentd {

struct VoicePeerRuntimeArtifactsPlan {
  std::string runtime_dir;
  std::string ready_file_path;
  std::string stdout_log_path;
  std::string stderr_log_path;
};

VoicePeerRuntimeArtifactsPlan plan_voice_peer_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& session_id
);

Json::Value voice_peer_runtime_artifacts_json(
  const VoicePeerRuntimeArtifactsPlan& plan
);

VoicePeerRuntimeSeed make_spawned_voice_peer_runtime_seed(
  const VoicePeerChildLaunchConfig& launch_cfg,
  const VoicePeerRuntimeArtifactsPlan& artifacts,
#if defined(_WIN32)
  intptr_t pid
#else
  pid_t pid
#endif
);

}  // namespace agentd
