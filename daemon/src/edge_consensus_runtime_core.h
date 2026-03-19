#pragma once

#include "edge_consensus_runtime_execution.h"
#include "edge_node_consensus.h"

#include <functional>
#include <string>
#include <vector>

namespace agentd {

struct EdgeConsensusRuntimeTransportOps {
  std::function<bool(uint64_t* io_seq, std::string* out_error)> post_hello;
  std::function<bool(
    const EdgeConsensusFrame& frame,
    const std::vector<std::string>& raw_target_node_ids,
    uint64_t* io_seq,
    std::string* out_error
  )> send_consensus_frame;
  std::function<bool(int64_t cursor, Json::Value* out, std::string* out_error)> poll_outbox;
};

bool run_edge_consensus_runtime_core(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusRuntimeHooks& hooks,
  const EdgeConsensusRuntimeTransportOps& transport,
  Json::Value* out_result,
  std::string* out_error
);

}  // namespace agentd
