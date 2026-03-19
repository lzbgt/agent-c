#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace agentd {

struct EdgeConsensusRuntimeSnapshot {
  std::shared_ptr<EdgeConsensusRuntime> runtime;
  Json::Value updates = Json::Value(Json::objectValue);
  bool from_registry = false;
};

bool edge_consensus_runtime_load_snapshot(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  EdgeConsensusRuntimeSnapshot* out,
  std::string* out_err
);

bool edge_consensus_runtime_resolve_snapshot(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  EdgeConsensusRuntimeSnapshot* out,
  Json::Value* out_recovery_updates,
  std::string* out_err
);

bool edge_consensus_runtime_reconcile_snapshot(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  EdgeConsensusRuntimeSnapshot* inout,
  std::string* out_err
);

}  // namespace agentd
