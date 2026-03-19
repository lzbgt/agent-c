#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agentd {

std::mutex& edge_consensus_runtime_registry_mutex();

std::shared_ptr<EdgeConsensusRuntime> edge_consensus_runtime_lookup_active(const std::string& node_id);
void edge_consensus_runtime_remember_active(const std::shared_ptr<EdgeConsensusRuntime>& st);
void edge_consensus_runtime_forget_active(const std::string& node_id);

Json::Value edge_consensus_runtime_status_json_for_node(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  const std::string& node_id
);

std::vector<std::string> edge_consensus_runtime_node_ids(AgentDb* db_or_null, size_t limit);

}  // namespace agentd
