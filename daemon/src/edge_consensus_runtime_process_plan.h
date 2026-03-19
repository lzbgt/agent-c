#pragma once

#include "daemon_config.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace agentd {

struct EdgeConsensusRuntimeArtifactsPlan {
  std::string runtime_dir;
  std::string stderr_log_path;
};

struct EdgeConsensusExternalProcessPlan {
  EdgeConsensusRuntimeArtifactsPlan artifacts;
  std::string tool_path;
  std::vector<std::string> argv;
};

EdgeConsensusRuntimeArtifactsPlan plan_edge_consensus_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& node_id
);

EdgeConsensusExternalProcessPlan make_edge_consensus_external_process_plan(
  const DaemonConfig& cfg,
  const EdgeConsensusHttpRuntimeConfig& run_cfg,
  const EdgeConsensusRuntime& runtime_state
);

bool edge_consensus_runtime_stdout_event_is_startup_ready(const Json::Value& parsed);

bool edge_consensus_runtime_stdout_event_live_status(
  const Json::Value& parsed,
  Json::Value* out_status
);

}  // namespace agentd
