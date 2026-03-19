#include "edge_consensus_runtime_registry.h"

#include "edge_consensus_runtime_recovery.h"
#include "edge_consensus_runtime_lifecycle.h"
#include "edge_consensus_runtime_store.h"
#include "edge_util.h"
#include "string_util.h"

#include <algorithm>
#include <unordered_map>

namespace agentd {
namespace {

struct EdgeConsensusRuntimeRegistryState {
  std::mutex mu;
  std::unordered_map<std::string, std::shared_ptr<EdgeConsensusRuntime>> by_node;
};

static EdgeConsensusRuntimeRegistryState& edge_consensus_runtime_registry_state() {
  static EdgeConsensusRuntimeRegistryState state;
  return state;
}

static std::vector<std::string> dedupe_safe_edge_ids(const std::vector<std::string>& in) {
  std::vector<std::string> out;
  out.reserve(in.size());
  for (const auto& raw : in) {
    const std::string s = trim_copy(raw);
    if (!edge_id_is_safe(s)) continue;
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  }
  return out;
}

}  // namespace

std::mutex& edge_consensus_runtime_registry_mutex() {
  return edge_consensus_runtime_registry_state().mu;
}

std::shared_ptr<EdgeConsensusRuntime> edge_consensus_runtime_lookup_active(const std::string& node_id) {
  auto& state = edge_consensus_runtime_registry_state();
  std::lock_guard<std::mutex> lk(state.mu);
  const auto it = state.by_node.find(node_id);
  return it == state.by_node.end() ? nullptr : it->second;
}

void edge_consensus_runtime_remember_active(const std::shared_ptr<EdgeConsensusRuntime>& st) {
  if (!st) return;
  auto& state = edge_consensus_runtime_registry_state();
  std::lock_guard<std::mutex> lk(state.mu);
  state.by_node[st->node_id] = st;
}

void edge_consensus_runtime_forget_active(const std::string& node_id) {
  auto& state = edge_consensus_runtime_registry_state();
  std::lock_guard<std::mutex> lk(state.mu);
  state.by_node.erase(node_id);
}

Json::Value edge_consensus_runtime_status_json_for_node(const DaemonConfig& cfg, AgentDb* db_or_null, const std::string& node_id) {
  auto st = edge_consensus_runtime_lookup_active(node_id);
  if (st) {
    {
      std::lock_guard<std::mutex> lk(edge_consensus_runtime_registry_mutex());
      refresh_edge_consensus_runtime_state(st.get());
      std::string perr;
      (void)persist_edge_consensus_runtime_record(db_or_null, *st, &perr);
      return edge_consensus_runtime_response_json(cfg, *st);
    }
  }
  if (!db_or_null || !db_or_null->is_open()) return Json::Value(Json::nullValue);
  std::shared_ptr<EdgeConsensusRuntime> persisted;
  Json::Value updates(Json::objectValue);
  std::string err;
  if (!recover_edge_consensus_runtime_record(cfg, db_or_null, node_id, &persisted, &updates, &err)) {
    return Json::Value(Json::nullValue);
  }
  if (!persisted) return Json::Value(Json::nullValue);
  if (persisted->running) {
    EdgeConsensusPersistedRunningReconcileResult reconcile;
    std::string rerr;
    if (!edge_consensus_runtime_reconcile_persisted_running(
          cfg, db_or_null, node_id, persisted, &reconcile, &rerr)) {
      return Json::Value(Json::nullValue);
    }
    if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::active_external) {
      edge_consensus_runtime_remember_active(persisted);
      std::string perr;
      (void)persist_edge_consensus_runtime_record(db_or_null, *persisted, &perr);
      return edge_consensus_runtime_response_json(cfg, *persisted);
    }
    if (reconcile.disposition == EdgeConsensusPersistedRunningDisposition::stale_cleared) {
      return Json::Value(Json::nullValue);
    }
  }
  return edge_consensus_runtime_response_json(cfg, *persisted);
}

std::vector<std::string> edge_consensus_runtime_node_ids(AgentDb* db_or_null, size_t limit) {
  if (limit == 0) return {};
  std::vector<std::string> ids;
  {
    auto& state = edge_consensus_runtime_registry_state();
    std::lock_guard<std::mutex> lk(state.mu);
    ids.reserve(state.by_node.size());
    for (const auto& kv : state.by_node) ids.push_back(kv.first);
  }
  if (db_or_null && db_or_null->is_open()) {
    std::vector<AgentDb::MetaRow> rows;
    std::string err;
    const size_t db_limit = std::max<size_t>(limit, 256);
    if (db_or_null->list_meta_prefix("edge.consensus_runtime.", db_limit, &rows, &err)) {
      for (const auto& row : rows) {
        constexpr size_t kPrefixLen = sizeof("edge.consensus_runtime.") - 1;
        if (row.key.size() <= kPrefixLen) continue;
        ids.push_back(row.key.substr(kPrefixLen));
      }
    }
  }
  ids = dedupe_safe_edge_ids(ids);
  std::sort(ids.begin(), ids.end());
  if (ids.size() > limit) ids.resize(limit);
  return ids;
}

}  // namespace agentd
