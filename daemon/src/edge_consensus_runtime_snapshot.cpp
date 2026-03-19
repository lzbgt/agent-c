#include "edge_consensus_runtime_snapshot.h"

#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_recovery.h"
#include "edge_consensus_runtime_registry.h"
#include "edge_consensus_runtime_store.h"

namespace agentd {

bool edge_consensus_runtime_load_snapshot(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  EdgeConsensusRuntimeSnapshot* out,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!out) {
    if (out_err) *out_err = "snapshot output missing";
    return false;
  }
  *out = EdgeConsensusRuntimeSnapshot{};

  out->runtime = edge_consensus_runtime_lookup_active(node_id);
  out->from_registry = (bool)out->runtime;
  if (out->runtime) {
    std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
    refresh_edge_consensus_runtime_state(out->runtime.get());
    return true;
  }

  Json::Value updates(Json::objectValue);
  std::shared_ptr<EdgeConsensusRuntime> persisted;
  std::string lerr;
  if (!recover_edge_consensus_runtime_record(cfg, db, node_id, &persisted, &updates, &lerr)) {
    if (out_err) *out_err = lerr.empty() ? "failed to load persisted consensus runtime state" : lerr;
    return false;
  }
  out->runtime = persisted;
  out->updates = updates;
  return true;
}

bool edge_consensus_runtime_resolve_snapshot(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  EdgeConsensusRuntimeSnapshot* out,
  Json::Value* out_recovery_updates,
  std::string* out_err
) {
  if (out_recovery_updates) *out_recovery_updates = Json::Value(Json::objectValue);
  if (!edge_consensus_runtime_load_snapshot(cfg, db, node_id, out, out_err)) return false;
  if (out_recovery_updates && out) {
    edge_consensus_runtime_append_recovery_updates(out->updates, out_recovery_updates);
  }
  if (!edge_consensus_runtime_reconcile_snapshot(cfg, db, node_id, out, out_err)) return false;
  if (out_recovery_updates && out) {
    edge_consensus_runtime_append_recovery_updates(out->updates, out_recovery_updates);
  }
  return true;
}

bool edge_consensus_runtime_reconcile_snapshot(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  EdgeConsensusRuntimeSnapshot* inout,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  if (!inout) {
    if (out_err) *out_err = "snapshot missing";
    return false;
  }
  if (!inout->runtime || inout->from_registry) return true;
  if (inout->runtime->status_source != "persisted" || !inout->runtime->running) return true;

  EdgeConsensusPersistedRunningReconcileResult reconcile;
  std::string rerr;
  if (!edge_consensus_runtime_reconcile_persisted_running(
        cfg, db, node_id, inout->runtime, &reconcile, &rerr)) {
    if (out_err) *out_err = rerr.empty() ? "failed to reconcile persisted consensus runtime state" : rerr;
    return false;
  }
  if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::active_external) {
    edge_consensus_runtime_remember_active(inout->runtime);
    return true;
  }
  if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::stale_cleared) {
    inout->updates["cleanup_on_stale_record"] = reconcile.cleanup;
    inout->runtime.reset();
  }
  return true;
}

}  // namespace agentd
