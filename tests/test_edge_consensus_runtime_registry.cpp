#include "agent_db.h"
#include "edge_consensus_runtime_registry.h"
#include "edge_consensus_runtime_store.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::EdgeConsensusRuntime;
using agentd::clear_edge_consensus_runtime_record;
using agentd::edge_consensus_runtime_forget_active;
using agentd::edge_consensus_runtime_node_ids;
using agentd::edge_consensus_runtime_remember_active;
using agentd::edge_consensus_runtime_status_json_for_node;
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

static void test_node_ids_merge_active_and_persisted_deduped() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_registry_ids");
  AgentDb db;
  open_test_db(db_path, &db);

  auto active = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-active", "external"));
  edge_consensus_runtime_remember_active(active);

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-persisted", "{}", &err));
  assert(db.meta_set("edge.consensus_runtime.node-active", "{}", &err));
  assert(db.meta_set("edge.consensus_runtime.invalid node", "{}", &err));

  const std::vector<std::string> ids = edge_consensus_runtime_node_ids(&db, 10);
  assert(ids.size() == 2);
  assert(ids[0] == "node-active");
  assert(ids[1] == "node-persisted");

  edge_consensus_runtime_forget_active("node-active");
#endif
}

static void test_status_json_for_active_runtime_persists_snapshot() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_registry_active");
  AgentDb db;
  open_test_db(db_path, &db);

  DaemonConfig cfg;
  auto active = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-live", "external"));
  active->running = true;
  active->pid = getpid();
  edge_consensus_runtime_remember_active(active);

  const Json::Value status = edge_consensus_runtime_status_json_for_node(cfg, &db, "node-live");
  assert(status.isObject());
  assert(status["node_id"].asString() == "node-live");
  assert(status["runtime_kind"].asString() == "external");
  assert(status["running"].asBool());

  std::string raw;
  std::string err;
  assert(db.meta_get("edge.consensus_runtime.node-live", &raw, &err));
  assert(!raw.empty());

  edge_consensus_runtime_forget_active("node-live");
  assert(clear_edge_consensus_runtime_record(&db, "node-live", &err));
#endif
}

static void test_status_json_self_heals_stale_builtin_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_registry_stale_builtin");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_registry_state_" + std::to_string((long long)getpid()));
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
  const Json::Value status = edge_consensus_runtime_status_json_for_node(cfg, &db, "node-builtin");
  assert(status.isNull());

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-builtin", &raw, &err));
  assert(raw.empty());
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

}  // namespace

int main() {
  test_node_ids_merge_active_and_persisted_deduped();
  test_status_json_for_active_runtime_persists_snapshot();
  test_status_json_self_heals_stale_builtin_record();
  return 0;
}
