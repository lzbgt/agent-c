#pragma once

#include "edge_consensus_runtime_execution.h"
#include "edge_node_consensus.h"

#include <json/json.h>

#include <string>

namespace agentd {

EdgeConsensusIdentity edge_consensus_runtime_self_identity(
  const EdgeConsensusRuntimeConfig& cfg
);

EdgeConsensusNodeLoopConfig edge_consensus_runtime_node_loop_config(
  const EdgeConsensusRuntimeConfig& cfg
);

Json::Value edge_consensus_runtime_loop_result_json(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusNodeLoop& loop,
  bool ok,
  const std::string& error
);

}  // namespace agentd
