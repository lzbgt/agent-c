#include "edge_consensus_runtime_control.h"

#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_registry.h"
#include "edge_consensus_runtime_store.h"

#include <chrono>
#include <thread>

namespace agentd {

EdgeConsensusRuntimePersistFn make_edge_consensus_runtime_persist_on_exit(AgentDb* db) {
  return [db](const EdgeConsensusRuntime& snapshot) {
    std::string perr;
    (void)persist_edge_consensus_runtime_record(db, snapshot, &perr);
  };
}

bool edge_consensus_runtime_stop(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::shared_ptr<EdgeConsensusRuntime>& st,
  std::mutex& runtime_mu,
  EdgeConsensusRuntimeStopResult* out,
  std::string* out_err
) {
  if (out) *out = EdgeConsensusRuntimeStopResult{};
  if (out_err) out_err->clear();

  if (!st) return true;
  if (!st->running) {
    std::string perr;
    (void)persist_edge_consensus_runtime_record(db, *st, &perr);
    if (out) out->runtime = edge_consensus_runtime_response_json(cfg, *st);
    return true;
  }

#if defined(_WIN32)
  if (out) {
    out->disposition = EdgeConsensusRuntimeStopDisposition::unsupported;
    out->runtime = edge_consensus_runtime_response_json(cfg, *st);
  }
  return true;
#else
  std::string serr;
  int signal_used = 0;
  if (!edge_consensus_runtime_kill_best_effort(st, runtime_mu, &signal_used, &serr)) {
    if (out_err) *out_err = serr.empty() ? "failed to stop consensus runtime" : serr;
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  {
    std::lock_guard<std::mutex> lk(runtime_mu);
    refresh_edge_consensus_runtime_state(st.get());
    finalize_recovered_edge_consensus_stop(st.get(), signal_used);
    if (out) out->runtime = edge_consensus_runtime_response_json(cfg, *st);
  }
  std::string perr;
  (void)persist_edge_consensus_runtime_record(db, *st, &perr);
  if (out) out->disposition = EdgeConsensusRuntimeStopDisposition::stopped;
  return true;
#endif
}

bool edge_consensus_runtime_activate_started(
  const DaemonConfig& cfg,
  AgentDb* db,
  const std::string& node_id,
  const std::shared_ptr<EdgeConsensusRuntime>& spawned,
  std::mutex& runtime_mu,
  Json::Value* out_runtime,
  std::string* out_err
) {
  if (out_err) out_err->clear();
  Json::Value startup_runtime(Json::nullValue);
  if (!edge_consensus_runtime_confirm_startup(spawned, runtime_mu, &startup_runtime, out_err)) {
    if (out_runtime) *out_runtime = startup_runtime;
    std::string cerr;
    (void)clear_edge_consensus_runtime_record(db, node_id, &cerr);
    return false;
  }
  edge_consensus_runtime_remember_active(spawned);
  std::string perr;
  (void)persist_edge_consensus_runtime_record(db, *spawned, &perr);
  if (out_runtime) *out_runtime = edge_consensus_runtime_response_json(cfg, *spawned);
  return true;
}

}  // namespace agentd
