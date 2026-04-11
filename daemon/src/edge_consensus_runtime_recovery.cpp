#include "edge_consensus_runtime_recovery.h"

#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_store.h"
#include "edge_util.h"

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

  const int64_t now_ms = st->ended_unix_ms > 0 ? st->ended_unix_ms : edge_unix_ms_now();
  int64_t stale_age_ms = 0;
  const int64_t recovery_grace_ms =
    edge_consensus_runtime_effective_stale_recovery_grace_ms(cfg, *st);
  const bool recover_stale =
    edge_consensus_runtime_stale_record_within_recovery_grace(cfg, *st, now_ms, &stale_age_ms);

  Json::Value cleanup(Json::objectValue);
  cleanup["stale_runtime_recovery_grace_ms"] = (Json::Int64)recovery_grace_ms;
  cleanup["stale_runtime_age_ms"] = (Json::Int64)stale_age_ms;
  bool artifacts_deleted = false;
  std::string aerr;
  if (remove_edge_consensus_runtime_artifacts(cfg, node_id, &artifacts_deleted, &aerr)) {
    cleanup["runtime_artifacts_deleted"] = artifacts_deleted;
  } else if (!aerr.empty()) {
    cleanup["runtime_artifacts_delete_error"] = aerr;
  }
  if (recover_stale) {
    st->running = false;
    st->ended_unix_ms = now_ms;
    st->status_source = "persisted_recovered";
    st->last_error = "stale_builtin_runtime_recovered_after_restart";
    cleanup["persisted_record_recovered"] = persist_edge_consensus_runtime_record(db, *st, nullptr);
    cleanup["persisted_record_cleared"] = false;
    if (out) {
      out->disposition = EdgeConsensusPersistedRunningDisposition::stale_recovered;
      out->cleanup = cleanup;
    }
    return true;
  }
  cleanup["persisted_record_cleared"] = clear_edge_consensus_runtime_record(db, node_id, nullptr);
  if (out) {
    out->disposition = EdgeConsensusPersistedRunningDisposition::stale_cleared;
    out->cleanup = cleanup;
  }
  return true;
}

}  // namespace agentd
