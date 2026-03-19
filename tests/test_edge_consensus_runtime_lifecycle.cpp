#include "edge_consensus_runtime_lifecycle.h"

#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

using agentd::EdgeConsensusRuntime;
using agentd::edge_consensus_runtime_confirm_startup;
using agentd::edge_consensus_runtime_kill_best_effort;
using agentd::finalize_recovered_edge_consensus_stop;
using agentd::refresh_edge_consensus_runtime_state;

static EdgeConsensusRuntime make_runtime() {
  EdgeConsensusRuntime st;
  st.node_id = "node-a";
  st.cluster_id = "cluster-a";
  st.manifest_sha256 = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  st.daemon_url = "@local";
  st.tool_path = "@builtin";
  st.model = "edge_consensus_node";
  st.fw_git_sha = "agentd_managed_runtime";
  return st;
}

static void test_refresh_marks_external_dead_pid_stopped() {
#if defined(_WIN32)
  return;
#else
  EdgeConsensusRuntime st = make_runtime();
  st.runtime_kind = "external";
  st.running = true;
  st.pid = -1;
  assert(st.ended_unix_ms == 0);
  refresh_edge_consensus_runtime_state(&st);
  assert(!st.running);
  assert(st.ended_unix_ms > 0);
#endif
}

static void test_finalize_recovered_stop_sets_exit_signal() {
  EdgeConsensusRuntime st = make_runtime();
  st.status_source = "persisted";
  st.running = false;
  finalize_recovered_edge_consensus_stop(&st, 15);
  assert(st.exit_signal == 15);
  assert(st.ended_unix_ms > 0);
}

static void test_confirm_startup_accepts_ready_runtime() {
  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime());
  st->running = true;
  st->startup_ready = std::make_shared<std::atomic<bool>>(true);
  std::mutex runtime_mu;

  Json::Value runtime(Json::nullValue);
  std::string err;
  assert(edge_consensus_runtime_confirm_startup(st, runtime_mu, &runtime, &err, 25));
  assert(err.empty());
  assert(runtime["node_id"].asString() == "node-a");
  assert(runtime["running"].asBool());
}

static void test_confirm_startup_accepts_fast_successful_exit() {
  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime());
  st->running = false;
  st->exit_code = 0;
  st->last_stdout_json["ok"] = true;
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);
  std::mutex runtime_mu;

  Json::Value runtime(Json::nullValue);
  std::string err;
  assert(edge_consensus_runtime_confirm_startup(st, runtime_mu, &runtime, &err, 25));
  assert(err.empty());
  assert(runtime["exit_code"].asInt() == 0);
}

static void test_confirm_startup_reports_error_on_early_exit() {
  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime());
  st->running = false;
  st->exit_code = 1;
  st->last_error = "boom";
  st->startup_ready = std::make_shared<std::atomic<bool>>(false);
  std::mutex runtime_mu;

  Json::Value runtime(Json::nullValue);
  std::string err;
  assert(!edge_consensus_runtime_confirm_startup(st, runtime_mu, &runtime, &err, 25));
  assert(err == "boom");
  assert(runtime["exit_code"].asInt() == 1);
}

static void test_kill_best_effort_builtin_waits_for_stop() {
  auto st = std::make_shared<EdgeConsensusRuntime>(make_runtime());
  st->runtime_kind = "builtin";
  st->running = true;
  st->stop_requested = std::make_shared<std::atomic<bool>>(false);
  std::mutex runtime_mu;

  std::thread stopper([st, &runtime_mu]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::lock_guard<std::mutex> lk(runtime_mu);
    st->running = false;
  });

  int signal_used = 0;
  std::string err;
  const bool ok = edge_consensus_runtime_kill_best_effort(st, runtime_mu, &signal_used, &err);
  stopper.join();

  assert(ok);
  assert(err.empty());
  assert(signal_used == 0);
  assert(st->stop_requested->load());
}

}  // namespace

int main() {
  test_refresh_marks_external_dead_pid_stopped();
  test_finalize_recovered_stop_sets_exit_signal();
  test_confirm_startup_accepts_ready_runtime();
  test_confirm_startup_accepts_fast_successful_exit();
  test_confirm_startup_reports_error_on_early_exit();
  test_kill_best_effort_builtin_waits_for_stop();
  return 0;
}
