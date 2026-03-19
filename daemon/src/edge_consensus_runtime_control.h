#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "edge_consensus_runtime_backend.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <memory>
#include <mutex>
#include <string>

namespace agentd {

enum class EdgeConsensusRuntimeStopDisposition {
  not_running,
  stopped,
  unsupported,
};

struct EdgeConsensusRuntimeStopResult {
  EdgeConsensusRuntimeStopDisposition disposition =
    EdgeConsensusRuntimeStopDisposition::not_running;
  Json::Value runtime = Json::Value(Json::nullValue);
};

EdgeConsensusRuntimePersistFn make_edge_consensus_runtime_persist_on_exit(AgentDb* db);

bool edge_consensus_runtime_stop(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  std::mutex& runtime_mu,
  EdgeConsensusRuntimeStopResult* out,
  std::string* out_err
);

bool edge_consensus_runtime_activate_started(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  const std::shared_ptr<EdgeConsensusRuntime>& spawned,
  std::mutex& runtime_mu,
  Json::Value* out_runtime,
  std::string* out_err
);

}  // namespace agentd
