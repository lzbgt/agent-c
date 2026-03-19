#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace agentd {

bool remove_edge_consensus_runtime_artifacts(
  const DaemonConfig& cfg,
  const std::string& node_id,
  bool* out_deleted,
  std::string* out_err
);

bool edge_consensus_runtime_from_json(
  const Json::Value& v,
  EdgeConsensusRuntime* out,
  std::string* out_err
);

bool persist_edge_consensus_runtime_record(
  AgentDb* db,
  const EdgeConsensusRuntime& st,
  std::string* out_err
);

bool clear_edge_consensus_runtime_record(
  AgentDb* db,
  const std::string& node_id,
  std::string* out_err
);

bool load_edge_consensus_runtime_record(
  AgentDb* db,
  const std::string& node_id,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  bool* out_self_healed,
  std::string* out_err
);

bool recover_edge_consensus_runtime_record(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  Json::Value* out_updates,
  std::string* out_err
);

}  // namespace agentd
