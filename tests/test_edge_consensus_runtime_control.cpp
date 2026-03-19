#include "agent_db.h"
#include "edge_consensus_runtime_control.h"
#include "edge_consensus_runtime_registry.h"
#include "edge_consensus_runtime_store.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

using agentd::AgentDb;
using agentd::DaemonConfig;
using agentd::EdgeConsensusRuntime;
using agentd::EdgeConsensusRuntimeStopDisposition;
using agentd::EdgeConsensusRuntimeStopResult;
using agentd::clear_edge_consensus_runtime_record;
using agentd::edge_consensus_runtime_activate_started;
using agentd::edge_consensus_runtime_forget_active;
using agentd::edge_consensus_runtime_lookup_active;
using agentd::edge_consensus_runtime_stop;
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
  assert(out_db->open(path.string(), &err));
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

static void test_stop_missing_runtime_reports_not_running() {
  std::mutex runtime_mu;
  EdgeConsensusRuntimeStopResult result;
  std::string err;
  assert(edge_consensus_runtime_stop(DaemonConfig(), nullptr, nullptr, runtime_mu, &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusRuntimeStopDisposition::not_running);
  assert(result.runtime.isNull());
}

static void test_stop_stopped_runtime_persists_snapshot() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_control_stop_stopped");
  AgentDb db;
  open_test_db(db_path, &db);

  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-stopped", "builtin"));
  st->running = false;
  std::mutex runtime_mu;
  EdgeConsensusRuntimeStopResult result;
  std::string err;
  assert(edge_consensus_runtime_stop(DaemonConfig(), &db, st, runtime_mu, &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusRuntimeStopDisposition::not_running);
  assert(result.runtime["node_id"].asString() == "node-stopped");

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-stopped", &raw, &err));
  assert(!raw.empty());
#endif
}

static void test_stop_builtin_runtime_waits_and_marks_stopped() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_control_stop_builtin");
  AgentDb db;
  open_test_db(db_path, &db);

  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-builtin", "builtin"));
  st->running = true;
  st->stop_requested = std::make_shared<std::atomic<bool>>(false);
  std::mutex runtime_mu;

  std::thread stopper([st, &runtime_mu]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard<std::mutex> lk(runtime_mu);
    st->running = false;
  });

  EdgeConsensusRuntimeStopResult result;
  std::string err;
  assert(edge_consensus_runtime_stop(DaemonConfig(), &db, st, runtime_mu, &result, &err));
  stopper.join();

  assert(err.empty());
  assert(result.disposition == EdgeConsensusRuntimeStopDisposition::stopped);
  assert(result.runtime["running"].isBool() && !result.runtime["running"].asBool());
  assert(st->stop_requested->load());

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-builtin", &raw, &err));
  assert(!raw.empty());
#endif
}

static void test_activate_started_runtime_remembers_and_persists() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_control_activate_ok");
  AgentDb db;
  open_test_db(db_path, &db);

  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-started", "builtin"));
  st->running = true;
  st->startup_ready = std::make_shared<std::atomic<bool>>(true);

  std::mutex runtime_mu;
  Json::Value runtime(Json::nullValue);
  std::string err;
  assert(edge_consensus_runtime_activate_started(
    DaemonConfig(), &db, st->node_id, st, runtime_mu, &runtime, &err));
  assert(err.empty());
  assert(runtime["node_id"].asString() == "node-started");
  auto active = edge_consensus_runtime_lookup_active("node-started");
  assert(active);
  edge_consensus_runtime_forget_active("node-started");

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-started", &raw, &err));
  assert(!raw.empty());
#endif
}

static void test_activate_started_runtime_clears_failed_record() {
#if !defined(AGENT_HAVE_SQLITE3)
  return;
#else
  const std::filesystem::path db_path = make_temp_db_path("edge_consensus_runtime_control_activate_fail");
  AgentDb db;
  open_test_db(db_path, &db);

  EdgeConsensusRuntime persisted = make_runtime("node-fail", "builtin");
  persisted.running = true;
  std::string err;
  assert(persist_edge_consensus_runtime_record(&db, persisted, &err));

  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime("node-fail", "builtin"));
  st->running = false;
  st->exit_code = 1;
  st->last_error = "boom";
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);

  std::mutex runtime_mu;
  Json::Value runtime(Json::nullValue);
  assert(!edge_consensus_runtime_activate_started(
    DaemonConfig(), &db, st->node_id, st, runtime_mu, &runtime, &err));
  assert(err == "boom");
  assert(runtime["exit_code"].asInt() == 1);

  std::string raw;
  assert(db.meta_get("edge.consensus_runtime.node-fail", &raw, &err));
  assert(raw.empty());
  edge_consensus_runtime_forget_active("node-fail");
  assert(clear_edge_consensus_runtime_record(&db, "node-fail", &err));
#endif
}

}  // namespace

int main() {
  test_stop_missing_runtime_reports_not_running();
  test_stop_stopped_runtime_persists_snapshot();
  test_stop_builtin_runtime_waits_and_marks_stopped();
  test_activate_started_runtime_remembers_and_persists();
  test_activate_started_runtime_clears_failed_record();
  return 0;
}
