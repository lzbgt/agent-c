#include "agent_db.h"
#include "edge_consensus_runtime_snapshot.h"
#include "edge_consensus_runtime_store.h"
#include "edge_consensus_runtime_registry.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::EdgeConsensusRuntime;
using agentd::EdgeConsensusRuntimeSnapshot;
using agentd::clear_edge_consensus_runtime_record;
using agentd::edge_consensus_runtime_forget_active;
using agentd::edge_consensus_runtime_load_snapshot;
using agentd::edge_consensus_runtime_reconcile_snapshot;
using agentd::edge_consensus_runtime_remember_active;
using agentd::persist_edge_consensus_runtime_record;

static std::filesystem::path make_temp_db_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".sqlite");
}

static void open_test_db(const std::filesystem::path& path, AgentDb* out_db) {
  assert(out_db);
  std::error_code ec;
  std::filesystem::remove(path, ec);
  std::string err;
  const bool ok = out_db->open(path.string(), &err);
  assert(ok);
  (void)err;
}

static EdgeConsensusRuntime make_runtime(const std::string& node_id, const std::string& runtime_kind) {
  EdgeConsensusRuntime st;
  st.runtime_kind = runtime_kind;
  st.node_id = node_id;
  st.cluster_id = "cluster-a";
  st.manifest_sha256 = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  st.daemon_url = runtime_kind == "builtin" ? "@local" : "http://127.0.0.1:8123";
  st.tool_path = runtime_kind == "builtin" ? "@builtin" : "/tmp/edge-consensus-node";
  st.model = "edge_consensus_node";
  st.fw_git_sha = "agentd_managed_runtime";
  st.member_node_ids = {"node-a", "node-b"};
  st.peer_node_ids = {"node-b"};
  return st;
}

static void test_load_snapshot_prefers_active_registry_state() {
  auto active = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-active", "external"));
  active->running = true;
  active->pid = getpid();
  edge_consensus_runtime_remember_active(active);

  EdgeConsensusRuntimeSnapshot snapshot;
  std::string err;
  assert(edge_consensus_runtime_load_snapshot(DaemonConfig(), nullptr, "node-active", &snapshot, &err));
  assert(err.empty());
  assert(snapshot.from_registry);
  assert(snapshot.runtime);
  assert(snapshot.runtime->node_id == "node-active");

  edge_consensus_runtime_forget_active("node-active");
}

static void test_load_snapshot_recovers_persisted_updates() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_snapshot_updates");
  AgentDb db;
  open_test_db(db_path, &db);

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", "{not-json", &err));

  EdgeConsensusRuntimeSnapshot snapshot;
  assert(edge_consensus_runtime_load_snapshot(DaemonConfig(), &db, "node-a", &snapshot, &err));
  assert(err.empty());
  assert(!snapshot.runtime);
  assert(snapshot.updates["cleanup_on_corrupt_record"].isObject());
#endif
}

static void test_reconcile_snapshot_clears_stale_builtin_runtime() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_snapshot_builtin");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_snapshot_state_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "edge_consensus_runtimes" / "node-builtin";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stderr.log").string().c_str(), "w");
    assert(f);
    std::fputs("stale builtin\n", f);
    std::fclose(f);
  }

  EdgeConsensusRuntime st = make_runtime("node-builtin", "builtin");
  st.running = true;
  st.stderr_log_path = (runtime_dir / "stderr.log").string();
  std::string err;
  assert(persist_edge_consensus_runtime_record(&db, st, &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  EdgeConsensusRuntimeSnapshot snapshot;
  assert(edge_consensus_runtime_load_snapshot(cfg, &db, "node-builtin", &snapshot, &err));
  assert(!snapshot.runtime);
  assert(!snapshot.from_registry);
  assert(snapshot.updates["cleanup_on_stale_record"]["persisted_record_cleared"].asBool());
  assert(snapshot.updates["cleanup_on_stale_record"]["runtime_artifacts_deleted"].asBool());
  assert(edge_consensus_runtime_reconcile_snapshot(cfg, &db, "node-builtin", &snapshot, &err));
  assert(err.empty());
  assert(!snapshot.runtime);
#endif
}

static void test_reconcile_snapshot_adopts_persisted_external_runtime() {
#if defined(_WIN32) || !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_snapshot_external");
  AgentDb db;
  open_test_db(db_path, &db);

  EdgeConsensusRuntime st = make_runtime("node-external", "external");
  st.running = true;
  st.pid = getpid();
  std::string err;
  assert(persist_edge_consensus_runtime_record(&db, st, &err));

  EdgeConsensusRuntimeSnapshot snapshot;
  assert(edge_consensus_runtime_load_snapshot(DaemonConfig(), &db, "node-external", &snapshot, &err));
  assert(snapshot.runtime);
  assert(!snapshot.from_registry);
  assert(edge_consensus_runtime_reconcile_snapshot(DaemonConfig(), &db, "node-external", &snapshot, &err));
  assert(err.empty());
  assert(snapshot.runtime);
  auto active = agentd::edge_consensus_runtime_lookup_active("node-external");
  assert(active);
  edge_consensus_runtime_forget_active("node-external");
  assert(clear_edge_consensus_runtime_record(&db, "node-external", &err));
#endif
}

}  // namespace

int main() {
  test_load_snapshot_prefers_active_registry_state();
  test_load_snapshot_recovers_persisted_updates();
  test_reconcile_snapshot_clears_stale_builtin_runtime();
  test_reconcile_snapshot_adopts_persisted_external_runtime();
  return 0;
}
