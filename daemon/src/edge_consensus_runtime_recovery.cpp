#include "edge_consensus_runtime_recovery.h"

#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_store.h"

namespace agentd {

void edge_consensus_runtime_append_recovery_updates(
  const Json::Value& updates,
  Json::Value* out
) {
  if (!out || !out->isObject() || !updates.isObject()) return;
  if (updates.isMember("cleanup_on_corrupt_record")) {
    (*out)["cleanup_on_corrupt_record"] = updates["cleanup_on_corrupt_record"];
  }
  if (updates.isMember("cleanup_on_stale_record")) {
    (*out)["cleanup_on_stale_record"] = updates["cleanup_on_stale_record"];
  }
}

bool edge_consensus_runtime_reconcile_persisted_running(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  EdgeConsensusPersistedRunningReconcileResult* out,
  std::string* out_err
) {
  if (out) *out = EdgeConsensusPersistedRunningReconcileResult{};
  if (out_err) out_err->clear();
  if (!st || !st->running) return true;

  refresh_edge_consensus_runtime_state(st.get());
  if (st->running && st->runtime_kind == "external") {
    if (out) out->disposition = EdgeConsensusPersistedRunningDisposition::active_external;
    return true;
  }
  if (!st->running) return true;

  Json::Value cleanup(Json::objectValue);
  cleanup["persisted_record_cleared"] = clear_edge_consensus_runtime_record(db, node_id, nullptr);
  bool artifacts_deleted = false;
  std::string aerr;
  if (remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr)) {
    cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
  } else if (!aerr.empty()) {
    cleanup["runtime_artifacts_delete_error"] = aerr;
  }
  if (out) {
    out->disposition = EdgeConsensusPersistedRunningDisposition::stale_cleared;
    out->cleanup = cleanup;
  }
  return true;
}

}  // namespace agentd
