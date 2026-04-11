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
  Json::Value cleanup(Json::objectValue);
  bool recovered = false;
  if (!recover_or_clear_edge_consensus_stale_builtin_record(
        cfg, db, node_id, st, now_ms, &recovered, &cleanup, out_err)) {
    return false;
  }
  if (recovered) {
    if (out) {
      out->disposition = EdgeConsensusPersistedRunningDisposition::stale_recovered;
      out->cleanup = cleanup;
    }
    return true;
  }
  if (out) {
    out->disposition = EdgeConsensusPersistedRunningDisposition::stale_cleared;
    out->cleanup = cleanup;
  }
  return true;
}

}  // namespace agentd
