#pragma once

#include "daemon_config.h"

#include <json/json.h>

#include <string>

namespace agentd {

std::string edge_consensus_external_runtime_unavailable_reason(const DaemonConfig& cfg);
std::string default_edge_consensus_runtime_kind_source(const DaemonConfig& cfg);
std::string default_edge_consensus_runtime_kind(const DaemonConfig& cfg);
void edge_consensus_normalize_policy_timing(EdgeConsensusClusterPolicy* pol);
Json::Value edge_consensus_runtime_backend_metadata_json(const DaemonConfig& cfg);

}  // namespace agentd
