#include "edge_consensus_runtime_backend.h"

#include <cassert>

namespace {

using agentd::EdgeConsensusExternalProcessPlan;
using agentd::EdgeConsensusRuntime;
using agentd::apply_edge_consensus_runtime_terminal_result;
using agentd::make_edge_consensus_builtin_runtime_state;
using agentd::make_edge_consensus_external_runtime_state;

static EdgeConsensusRuntime make_runtime() {
  EdgeConsensusRuntime st;
  st.node_id = "node-a";
  st.cluster_id = "cluster-a";
  st.manifest_sha256 = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  st.daemon_url = "http://127.0.0.1:8123";
  st.model = "edge_consensus_node";
  st.fw_git_sha = "agentd_managed_runtime";
  return st;
}

static void test_make_external_runtime_state_shapes_runtime_metadata() {
  EdgeConsensusRuntime runtime = make_runtime();
  EdgeConsensusExternalProcessPlan plan;
  plan.tool_path = "/tmp/edge-consensus-node";
  plan.artifacts.stderr_log_path = "/tmp/edge/stderr.log";

  const auto st = make_edge_consensus_external_runtime_state(runtime, plan, 321);
  assert(st);
  assert(st->runtime_kind == "external");
  assert(st->tool_path == "/tmp/edge-consensus-node");
  assert(st->stderr_log_path == "/tmp/edge/stderr.log");
  assert(st->pid == 321);
  assert(st->startup_ready);
}

static void test_make_builtin_runtime_state_shapes_local_runtime_metadata() {
  EdgeConsensusRuntime runtime = make_runtime();
  const auto st = make_edge_consensus_builtin_runtime_state(runtime);
  assert(st);
  assert(st->runtime_kind == "builtin");
  assert(st->tool_path == "@builtin");
  assert(st->daemon_url == "@local");
  assert(st->stop_requested);
  assert(st->startup_ready);
}

static void test_apply_terminal_result_for_internal_error() {
  EdgeConsensusRuntime st = make_runtime();
  st.running = true;
  Json::Value result(Json::nullValue);

  apply_edge_consensus_runtime_terminal_result(&st, false, result, "transport failed");
  assert(!st.running);
  assert(st.exit_code == 1);
  assert(st.last_error == "transport failed");
  assert(st.ended_unix_ms > 0);
}

static void test_apply_terminal_result_for_structured_failure() {
  EdgeConsensusRuntime st = make_runtime();
  st.running = true;
  Json::Value result(Json::objectValue);
  result["ok"] = false;
  result["error"] = "deadline";

  apply_edge_consensus_runtime_terminal_result(&st, true, result, "");
  assert(!st.running);
  assert(st.exit_code == 1);
  assert(st.last_error == "deadline");
  assert(st.last_stdout_json["ok"].asBool() == false);
}

static void test_apply_terminal_result_for_structured_success() {
  EdgeConsensusRuntime st = make_runtime();
  st.running = true;
  Json::Value result(Json::objectValue);
  result["ok"] = true;
  result["schema"] = "edge_node_consensus_result_v1";

  apply_edge_consensus_runtime_terminal_result(&st, true, result, "");
  assert(!st.running);
  assert(st.exit_code == 0);
  assert(st.last_error.empty());
  assert(st.last_stdout_json["ok"].asBool());
}

}  // namespace

int main() {
  test_make_external_runtime_state_shapes_runtime_metadata();
  test_make_builtin_runtime_state_shapes_local_runtime_metadata();
  test_apply_terminal_result_for_internal_error();
  test_apply_terminal_result_for_structured_failure();
  test_apply_terminal_result_for_structured_success();
  return 0;
}
