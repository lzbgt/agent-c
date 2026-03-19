#pragma once

#include "edge_consensus_runtime_model.h"

#include <memory>
#include <mutex>
#include <string>

namespace agentd {

void refresh_edge_consensus_runtime_state(EdgeConsensusRuntime* st);

void finalize_recovered_edge_consensus_stop(EdgeConsensusRuntime* st, int signal_used);

bool edge_consensus_runtime_kill_best_effort(
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  std::mutex& runtime_mu,
  int* out_signal_used,
  std::string* out_err
);

bool edge_consensus_runtime_confirm_startup(
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  std::mutex& runtime_mu,
  Json::Value* out_runtime,
  std::string* out_err,
  int64_t timeout_ms = 400
);

}  // namespace agentd
