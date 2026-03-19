#include "edge_consensus_http_runtime.h"

#include "edge_consensus_http_transport.h"
#include "edge_consensus_runtime_core.h"

namespace agentd {

bool run_edge_consensus_http_runtime(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
) {
  const EdgeConsensusRuntimeTransportOps transport = make_edge_consensus_http_transport(cfg);
  return run_edge_consensus_runtime_core(cfg, hooks, transport, out_result, out_error);
}

}  // namespace agentd
