#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "edge_consensus_runtime_model.h"

#include <json/json.h>

#include <memory>
#include <string>

namespace agentd {

enum class EdgeConsensusPersistedRunningDisposition {
  not_running,
  active_external,
  stale_cleared,
};

struct EdgeConsensusPersistedRunningReconcileResult {
  EdgeConsensusPersistedRunningDisposition disposition =
    EdgeConsensusPersistedRunningDisposition::not_running;
  Json::Value cleanup = Json::Value(Json::nullValue);
};

void edge_consensus_runtime_append_recovery_updates(
  const Json::Value& updates,
  Json::Value* out
);

bool edge_consensus_runtime_reconcile_persisted_running(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  EdgeConsensusPersistedRunningReconcileResult* out,
  std::string* out_err
);

}  // namespace agentd
