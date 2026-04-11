#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

class AgentDb;
struct EdgeConsensusFrame;

bool collect_edge_consensus_target_node_ids(
  const Json::Value& body,
  const std::string& env_to,
  std::vector<std::string>* out_node_ids,
  std::string* out_error
);

bool upsert_edge_node_consensus_health(
  AgentDb* db,
  const std::string& node_id,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& target_node_ids,
  const std::string& original_msg_id,
  int64_t now_utc_ms,
  std::string* out_error
);

}  // namespace agentd
