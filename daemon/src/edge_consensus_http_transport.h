#pragma once

#include "edge_consensus_runtime_execution.h"
#include "edge_consensus_runtime_core.h"

#include <json/json.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace agentd {

struct EdgeConsensusFrame;

std::map<std::string, std::string> edge_consensus_http_json_headers(
  const EdgeConsensusRuntimeConfig& cfg
);

Json::Value build_edge_consensus_http_hello_envelope(
  const EdgeConsensusRuntimeConfig& cfg,
  const std::string& msg_id,
  int64_t ts_utc_ms
);

Json::Value build_edge_consensus_http_frame_envelope(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& raw_target_node_ids,
  const std::string& msg_id,
  int64_t ts_utc_ms
);

std::string build_edge_consensus_http_outbox_poll_url(
  const EdgeConsensusRuntimeConfig& cfg,
  int64_t cursor
);

EdgeConsensusRuntimeTransportOps make_edge_consensus_http_transport(
  const EdgeConsensusRuntimeConfig& cfg
);

}  // namespace agentd
