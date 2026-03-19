#pragma once

#include "daemon_config.h"
#include "edge_consensus_http_runtime.h"
#include "edge_consensus_runtime_model.h"
#include "edge_consensus_runtime_process_plan.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace agentd {

class AgentDb;

using EdgeConsensusRuntimePersistFn = std::function<void(const EdgeConsensusRuntime&)>;

std::shared_ptr<EdgeConsensusRuntime> make_edge_consensus_external_runtime_state(
  const EdgeConsensusRuntime& runtime_state,
  const EdgeConsensusExternalProcessPlan& process_plan,
#if defined(_WIN32)
  intptr_t pid
#else
  pid_t pid
#endif
);

std::shared_ptr<EdgeConsensusRuntime> make_edge_consensus_builtin_runtime_state(
  const EdgeConsensusRuntime& runtime_state
);

void apply_edge_consensus_runtime_terminal_result(
  EdgeConsensusRuntime* st,
  bool ok,
  const Json::Value& result,
  const std::string& err
);

#if !defined(_WIN32)
bool edge_consensus_runtime_spawn_external(
  const DaemonConfig& cfg,
  AgentDb* db,
  const Json::Value& body,
  std::mutex& runtime_mu,
  const EdgeConsensusRuntimePersistFn& on_exit_persist,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  std::string* out_err
);

bool edge_consensus_runtime_start_builtin(
  const DaemonConfig& cfg,
  AgentDb* db,
  const Json::Value& body,
  std::mutex& runtime_mu,
  const EdgeConsensusRuntimePersistFn& on_exit_persist,
  std::shared_ptr<EdgeConsensusRuntime>* out_state,
  std::string* out_err
);
#endif

}  // namespace agentd
