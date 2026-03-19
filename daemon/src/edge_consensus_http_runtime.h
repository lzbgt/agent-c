#pragma once

#include "edge_consensus_runtime_execution.h"

#include <json/json.h>

namespace agentd {

class AgentDb;

using EdgeConsensusHttpRuntimeConfig = EdgeConsensusRuntimeConfig;
using EdgeConsensusHttpRuntimeHooks = EdgeConsensusRuntimeHooks;

// Runs the poll/process/post consensus loop against agentd HTTP surfaces.
//
// Returns true when a structured final result JSON was produced in `out_result`.
// That includes successful commit, deadline expiry, or graceful stop.
// Returns false only for internal transport / parse failures, in which case
// `out_error` contains a human-readable error.
bool run_edge_consensus_http_runtime(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
);

// Runs the same consensus loop against daemon-local DB-backed transport instead of
// calling back through agentd's HTTP surfaces. This is the transport used by the
// builtin managed runtime.
bool run_edge_consensus_local_runtime(
  AgentDb* db,
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusRuntimeHooks& hooks,
  Json::Value* out_result,
  std::string* out_error
);

}  // namespace agentd
