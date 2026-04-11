#pragma once

#include <json/json.h>

#include <string>
#include <vector>

namespace agentd {

struct DaemonConfig;

std::vector<std::string> edge_consensus_normalize_member_node_ids(const std::vector<std::string>& in);

bool build_edge_consensus_membership_bundle(
  const DaemonConfig& cfg,
  const std::string& cluster_id,
  Json::Value* out_bundle,
  std::string* out_error
);

}  // namespace agentd
