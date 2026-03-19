#pragma once

#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_core.h"

#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

namespace agentd {

class AgentDb;
struct EdgeConsensusFrame;

bool post_edge_consensus_local_hello(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
  uint64_t* io_seq,
  std::string* out_error
);

bool send_edge_consensus_local_frame(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
  const EdgeConsensusFrame& frame,
  const std::vector<std::string>& raw_target_node_ids,
  uint64_t* io_seq,
  std::string* out_error
);

bool poll_edge_consensus_local_outbox(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg,
  int64_t cursor,
  Json::Value* out,
  std::string* out_error
);

EdgeConsensusRuntimeTransportOps make_edge_consensus_local_transport(
  AgentDb* db,
  const EdgeConsensusHttpRuntimeConfig& cfg
);

}  // namespace agentd
