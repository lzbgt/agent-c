#include "agent_db.h"
#include "edge_consensus_runtime_store.h"
#include "edge_consensus_runtime_model.h"
#include "json_util.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::EdgeConsensusClusterPolicy;
using agentd::EdgeConsensusRuntime;
using agentd::clear_edge_consensus_runtime_record;
using agentd::edge_consensus_runtime_to_json;
using agentd::json_stringify;
using agentd::load_edge_consensus_runtime_record;
using agentd::persist_edge_consensus_runtime_record;
using agentd::recover_edge_consensus_runtime_record;

static int64_t now_unix_ms() {
  using namespace std::chrono;
  return (int64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

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

static EdgeConsensusRuntime make_runtime(const std::string& runtime_kind) {
  EdgeConsensusRuntime st;
  st.runtime_kind = runtime_kind;
  st.node_id = "node-a";
  st.cluster_id = "cluster-a";
  st.manifest_sha256 = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  st.daemon_url = runtime_kind == "builtin" ? "@local" : "http://127.0.0.1:8123";
  st.tool_path = runtime_kind == "builtin" ? "@builtin" : "/tmp/edge-consensus-node";
  st.model = "edge_consensus_node";
  st.fw_git_sha = "agentd_managed_runtime";
  st.member_node_ids = {"node-a", "node-b"};
  st.peer_node_ids = {"node-b"};
  st.running = false;
  return st;
}

static void test_persist_and_load_roundtrip() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_store_roundtrip");
  AgentDb db;
  open_test_db(db_path, &db);

  EdgeConsensusRuntime st = make_runtime("builtin");
  st.running = false;
  st.exit_code = 0;
  st.peer_node_ids = {"node-b", "node-b", "bad/node"};
  st.member_node_ids = {"node-a", "node-b", "node-a", "bad/node"};

  std::string err;
  assert(persist_edge_consensus_runtime_record(&db, st, &err));
  assert(err.empty());

  bool self_healed = false;
  std::shared_ptr<EdgeConsensusRuntime> loaded;
  assert(load_edge_consensus_runtime_record(&db, "node-a", &loaded, &self_healed, &err));
  assert(!self_healed);
  assert(loaded);
  assert(loaded->status_source == "persisted");
  assert(loaded->runtime_kind == "builtin");
  assert(loaded->cluster_id == "cluster-a");
  assert(loaded->peer_node_ids.size() == 1);
  assert(loaded->peer_node_ids[0] == "node-b");
  assert(loaded->member_node_ids.size() == 2);
  assert(loaded->member_node_ids[0] == "node-a");
  assert(loaded->member_node_ids[1] == "node-b");
#endif
}

static void test_load_self_heals_corrupt_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_store_corrupt");
  AgentDb db;
  open_test_db(db_path, &db);

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", "{not-json", &err));

  bool self_healed = false;
  std::shared_ptr<EdgeConsensusRuntime> loaded;
  err.clear();
  assert(load_edge_consensus_runtime_record(&db, "node-a", &loaded, &self_healed, &err));
  assert(!loaded);
  assert(self_healed);
  assert(err.empty());

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-a", &raw, &err));
  assert(raw.empty());
#endif
}

static void test_recover_cleans_stale_builtin_running_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_store_stale_builtin");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_store_state_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "edge_consensus_runtimes" / "node-a";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stderr.log").string().c_str(), "w");
    assert(f);
    std::fputs("stale builtin\n", f);
    std::fclose(f);
  }

  EdgeConsensusRuntime st = make_runtime("builtin");
  st.running = true;
  st.stderr_log_path = (runtime_dir / "stderr.log").string();

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", json_stringify(edge_consensus_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  std::shared_ptr<EdgeConsensusRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_edge_consensus_runtime_record(cfg, &db, "node-a", &recovered, &updates, &err));
  assert(!recovered);
  assert(err.empty());
  assert(updates["cleanup_on_stale_record"]["persisted_record_cleared"].asBool());
  assert(updates["cleanup_on_stale_record"]["runtime_artifacts_deleted"].asBool());
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

static void test_recover_preserves_recent_stale_builtin_with_grace_policy() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_store_stale_builtin_grace");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_store_state_grace_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "edge_consensus_runtimes" / "node-a";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stderr.log").string().c_str(), "w");
    assert(f);
    std::fputs("recent stale builtin\n", f);
    std::fclose(f);
  }

  EdgeConsensusRuntime st = make_runtime("builtin");
  st.running = true;
  st.started_unix_ms = now_unix_ms();
  st.membership_epoch = 5;
  st.stale_runtime_recovery_grace_ms = 60000;
  st.stderr_log_path = (runtime_dir / "stderr.log").string();

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", json_stringify(edge_consensus_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.previous_membership_epoch = 6;
  pol.member_node_ids = {"node-a", "node-b"};
  pol.membership_lineage.push_back({6, {"node-a", "node-b"}});
  pol.membership_lineage.push_back({5, {"node-a"}});
  pol.stale_runtime_recovery_grace_ms = 60000;
  cfg.edge_consensus_clusters["cluster-a"] = pol;

  std::shared_ptr<EdgeConsensusRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_edge_consensus_runtime_record(cfg, &db, "node-a", &recovered, &updates, &err));
  assert(recovered);
  assert(!recovered->running);
  assert(recovered->status_source == "persisted_recovered");
  assert(recovered->last_error == "stale_builtin_runtime_recovered_after_restart");
  assert(recovered->ended_unix_ms >= st.started_unix_ms);
  assert(updates["cleanup_on_stale_record"]["persisted_record_recovered"].asBool());
  assert(!updates["cleanup_on_stale_record"]["persisted_record_cleared"].asBool());
  assert(updates["cleanup_on_stale_record"]["runtime_artifacts_deleted"].asBool());
  assert(updates["cleanup_on_stale_record"]["stale_runtime_recovery_grace_ms"].asInt64() == 60000);
  assert(updates["cleanup_on_stale_record"]["membership_recovery_policy"]["recoverable"].asBool());
  assert(updates["cleanup_on_stale_record"]["membership_recovery_policy"]["reason"].asString() == "membership_lineage");
  assert(!std::filesystem::exists(runtime_dir));

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-a", &raw, &err));
  assert(!raw.empty());
#endif
}

static void test_recover_clears_recent_stale_builtin_with_incompatible_membership() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_store_stale_builtin_epoch_mismatch");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_store_state_epoch_mismatch_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "edge_consensus_runtimes" / "node-a";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);
  {
    FILE* f = std::fopen((runtime_dir / "stderr.log").string().c_str(), "w");
    assert(f);
    std::fputs("recent stale builtin wrong epoch\n", f);
    std::fclose(f);
  }

  EdgeConsensusRuntime st = make_runtime("builtin");
  st.running = true;
  st.started_unix_ms = now_unix_ms();
  st.membership_epoch = 4;
  st.stale_runtime_recovery_grace_ms = 60000;
  st.stderr_log_path = (runtime_dir / "stderr.log").string();

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", json_stringify(edge_consensus_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.previous_membership_epoch = 6;
  pol.member_node_ids = {"node-a", "node-b"};
  pol.membership_lineage.push_back({6, {"node-a", "node-b"}});
  pol.membership_lineage.push_back({5, {"node-a"}});
  pol.stale_runtime_recovery_grace_ms = 60000;
  cfg.edge_consensus_clusters["cluster-a"] = pol;

  std::shared_ptr<EdgeConsensusRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_edge_consensus_runtime_record(cfg, &db, "node-a", &recovered, &updates, &err));
  assert(!recovered);
  assert(err.empty());
  assert(updates["cleanup_on_stale_record"]["persisted_record_cleared"].asBool());
  assert(!updates["cleanup_on_stale_record"]["persisted_record_recovered"].asBool());
  assert(updates["cleanup_on_stale_record"]["membership_recovery_policy"]["recoverable"].asBool() == false);
  assert(updates["cleanup_on_stale_record"]["membership_recovery_policy"]["reason"].asString() ==
         "not_in_current_policy_or_lineage");
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

static void test_recover_clears_recent_stale_builtin_removed_from_matching_lineage() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path =
    make_temp_db_path("edge_consensus_runtime_store_stale_builtin_lineage_nonmember");
  AgentDb db;
  open_test_db(db_path, &db);

  const std::filesystem::path state_dir =
    std::filesystem::temp_directory_path() /
    ("edge_consensus_runtime_store_state_lineage_nonmember_" + std::to_string((long long)getpid()));
  std::error_code ec;
  std::filesystem::remove_all(state_dir, ec);
  const std::filesystem::path runtime_dir = state_dir / "edge_consensus_runtimes" / "node-a";
  std::filesystem::create_directories(runtime_dir, ec);
  assert(!ec);

  EdgeConsensusRuntime st = make_runtime("builtin");
  st.running = true;
  st.started_unix_ms = now_unix_ms();
  st.membership_epoch = 5;
  st.stale_runtime_recovery_grace_ms = 60000;
  st.stderr_log_path = (runtime_dir / "stderr.log").string();

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", json_stringify(edge_consensus_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  cfg.state_dir = state_dir.string();
  EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.previous_membership_epoch = 6;
  pol.member_node_ids = {"node-a", "node-b"};
  pol.membership_lineage.push_back({6, {"node-a", "node-b"}});
  pol.membership_lineage.push_back({5, {"node-c"}});
  pol.stale_runtime_recovery_grace_ms = 60000;
  cfg.edge_consensus_clusters["cluster-a"] = pol;

  std::shared_ptr<EdgeConsensusRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_edge_consensus_runtime_record(cfg, &db, "node-a", &recovered, &updates, &err));
  assert(!recovered);
  assert(err.empty());
  assert(updates["cleanup_on_stale_record"]["persisted_record_cleared"].asBool());
  assert(!updates["cleanup_on_stale_record"]["persisted_record_recovered"].asBool());
  assert(updates["cleanup_on_stale_record"]["membership_recovery_policy"]["recoverable"].asBool() == false);
  assert(updates["cleanup_on_stale_record"]["membership_recovery_policy"]["reason"].asString() ==
         "membership_lineage_node_not_member");
  assert(!std::filesystem::exists(runtime_dir));
#endif
}

static void test_recover_preserves_running_external_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_store_external");
  AgentDb db;
  open_test_db(db_path, &db);

  EdgeConsensusRuntime st = make_runtime("external");
  st.running = true;
  st.pid = 123;

  std::string err;
  assert(db.meta_set("edge.consensus_runtime.node-a", json_stringify(edge_consensus_runtime_to_json(st)), &err));

  DaemonConfig cfg;
  std::shared_ptr<EdgeConsensusRuntime> recovered;
  Json::Value updates(Json::objectValue);
  err.clear();
  assert(recover_edge_consensus_runtime_record(cfg, &db, "node-a", &recovered, &updates, &err));
  assert(recovered);
  assert(recovered->status_source == "persisted");
  assert(recovered->runtime_kind == "external");
  assert(recovered->running);
  assert(!updates.isMember("cleanup_on_stale_record"));
  assert(!updates.isMember("cleanup_on_corrupt_record"));
#endif
}

}  // namespace

int main() {
  test_persist_and_load_roundtrip();
  test_load_self_heals_corrupt_record();
  test_recover_cleans_stale_builtin_running_record();
  test_recover_preserves_recent_stale_builtin_with_grace_policy();
  test_recover_clears_recent_stale_builtin_with_incompatible_membership();
  test_recover_clears_recent_stale_builtin_removed_from_matching_lineage();
  test_recover_preserves_running_external_record();
  return 0;
}
