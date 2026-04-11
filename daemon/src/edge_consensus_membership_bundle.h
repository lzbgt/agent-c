#pragma once

#include "edge_consensus_member_ids.h"

#include <json/json.h>

#include <string>
#include <vector>

namespace agentd {

struct DaemonConfig;

bool build_edge_consensus_membership_bundle(
  const DaemonConfig& cfg,
  const std::string& cluster_id,
  Json::Value* out_bundle,
  std::string* out_error
);

}  // namespace agentd
