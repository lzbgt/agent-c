#include "agent_db.h"
#include "edge_consensus_runtime_recovery.h"
#include "edge_consensus_runtime_store.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::EdgeConsensusPersistedRunningDisposition;
using agentd::EdgeConsensusPersistedRunningReconcileResult;
using agentd::EdgeConsensusClusterPolicy;
using agentd::EdgeConsensusRuntime;
using agentd::edge_consensus_runtime_append_recovery_updates;
using agentd::edge_consensus_runtime_reconcile_persisted_running;
using agentd::persist_edge_consensus_runtime_record;

static std::filesystem::path make_temp_db_path(const char* label) {
  return std::filesystem::temp_directory_path() /
         (std::string(label) + "_" + std::to_string((long long)getpid()) + ".sqlite");
}

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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

static void test_append_recovery_updates_copies_known_fields() {
  Json::Value updates(Json::objectValue);
  updates["cleanup_on_corrupt_record"]["cleared"] = true;
  updates["cleanup_on_stale_record"]["runtime_artifacts_deleted"] = true;
  Json::Value out(Json::objectValue);
  out["ok"] = false;

  edge_consensus_runtime_append_recovery_updates(updates, &out);
  assert(out["cleanup_on_corrupt_record"]["cleared"].asBool());
  assert(out["cleanup_on_stale_record"]["runtime_artifacts_deleted"].asBool());
}

static void test_reconcile_persisted_external_running_marks_active() {
#if defined(_WIN32)
  return;
#else
  EdgeConsensusRuntime st = make_runtime("node-external", "external");
  st.running = true;
  st.pid = getpid();

  EdgeConsensusPersistedRunningReconcileResult result;
  std::string err;
  assert(edge_consensus_runtime_reconcile_persisted_running(
    DaemonConfig(), nullptr, st.node_id, std::make_shared<EdgeConsensusRuntime>(st), &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusPersistedRunningDisposition::active_external);
  assert(result.cleanup.isNull());
#endif
}

static void test_reconcile_persisted_builtin_running_clears_stale_state() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_recovery_builtin");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_recovery_state_" + std::to_string((long long)getpid()));
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
  EdgeConsensusPersistedRunningReconcileResult result;
  assert(edge_consensus_runtime_reconcile_persisted_running(
    cfg, &db, st.node_id, std::make_shared<EdgeConsensusRuntime>(st), &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusPersistedRunningDisposition::stale_cleared);
  assert(result.cleanup["persisted_record_cleared"].asBool());
  assert(result.cleanup["runtime_artifacts_deleted"].asBool());

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-builtin", &raw, &err));
  assert(raw.empty());
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

static void test_reconcile_persisted_builtin_running_recovers_recent_state() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_recovery_builtin_grace");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_recovery_state_grace_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "edge_consensus_runtimes" / "node-builtin";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stderr.log").string().c_str(), "w");
    assert(f);
    std::fputs("recent stale builtin\n", f);
    std::fclose(f);
  }

  EdgeConsensusRuntime st = make_runtime("node-builtin", "builtin");
  st.running = true;
  st.started_unix_ms = now_unix_ms();
  st.stale_runtime_recovery_grace_ms = 60000;
  st.stderr_log_path = (runtime_dir / "stderr.log").string();

  std::string err;
  assert(persist_edge_consensus_runtime_record(&db, st, &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  EdgeConsensusClusterPolicy pol;
  pol.member_node_ids = {"node-builtin", "node-peer"};
  pol.stale_runtime_recovery_grace_ms = 60000;
  cfg.edge_consensus_clusters["cluster-a"] = pol;
  auto ptr = std::make_shared<EdgeConsensusRuntime>(st);
  EdgeConsensusPersistedRunningReconcileResult result;
  assert(edge_consensus_runtime_reconcile_persisted_running(
    cfg, &db, st.node_id, ptr, &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusPersistedRunningDisposition::stale_recovered);
  assert(!ptr->running);
  assert(ptr->status_source == "persisted_recovered");
  assert(ptr->last_error == "stale_builtin_runtime_recovered_after_restart");
  assert(result.cleanup["persisted_record_recovered"].asBool());
  assert(!result.cleanup["persisted_record_cleared"].asBool());
  assert(result.cleanup["runtime_artifacts_deleted"].asBool());
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

}  // namespace

int main() {
  test_append_recovery_updates_copies_known_fields();
  test_reconcile_persisted_external_running_marks_active();
  test_reconcile_persisted_builtin_running_clears_stale_state();
  test_reconcile_persisted_builtin_running_recovers_recent_state();
  return 0;
}
