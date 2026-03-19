#include "edge_consensus_http_runtime.h"

#include "agent_db.h"
#include "edge_consensus_local_transport.h"
#include "edge_consensus_runtime_core.h"

namespace agentd {

bool run_edge_consensus_local_runtime(
  AgentDb* db,
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
) {
  if (!db || !db->is_open()) {
    if (out_error) *out_error = "db not available";
    return false;
  }
  const EdgeConsensusRuntimeTransportOps transport = make_edge_consensus_local_transport(db, cfg);
  return run_edge_consensus_runtime_core(cfg, hooks, transport, out_result, out_error);
}

}  // namespace agentd
