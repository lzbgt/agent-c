#include "edge_consensus_runtime_process_plan.h"

#include <cassert>
#include <string>
#include <vector>

namespace {

using agentd::DaemonConfig;
using agentd::EdgeConsensusExternalProcessPlan;
using agentd::EdgeConsensusHttpRuntimeConfig;
using agentd::EdgeConsensusRuntime;
using agentd::EdgeConsensusRuntimeArtifactsPlan;
using agentd::edge_consensus_runtime_stdout_event_is_startup_ready;
using agentd::edge_consensus_runtime_stdout_event_live_status;
using agentd::make_edge_consensus_external_process_plan;
using agentd::plan_edge_consensus_runtime_artifacts;

static void test_runtime_artifacts_follow_state_dir() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-state";

  const EdgeConsensusRuntimeArtifactsPlan plan =
    plan_edge_consensus_runtime_artifacts(cfg, "node-a");
  assert(plan.runtime_dir == "/tmp/agentd-state/edge_consensus_runtimes/node-a");
  assert(plan.stderr_log_path == "/tmp/agentd-state/edge_consensus_runtimes/node-a/stderr.log");
}

static void test_external_process_plan_shapes_argv() {
  DaemonConfig cfg;
  cfg.state_dir = "/tmp/agentd-runtime";
  cfg.edge_consensus_node_tool_path = "/tmp/edge-consensus-node";

  EdgeConsensusHttpRuntimeConfig run_cfg;
  run_cfg.daemon_url = "http://127.0.0.1:8123";
  run_cfg.auth_token = "daemon-token";

  EdgeConsensusRuntime runtime;
  runtime.node_id = "node-a";
  runtime.cluster_id = "cluster-a";
  runtime.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  runtime.decision_sha256 =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  runtime.cluster_size = 3;
  runtime.outbox_limit = 128;
  runtime.campaign_delay_ms = 10;
  runtime.campaign_retry_ms = 20;
  runtime.campaign_retry_max_ms = 30;
  runtime.campaign_retry_backoff_factor = 2;
  runtime.leader_heartbeat_ms = 1000;
  runtime.leader_lease_ms = 5000;
  runtime.lease_expiry_recampaign_delay_ms = 333;
  runtime.poll_interval_ms = 100;
  runtime.deadline_ms = 2000;
  runtime.trust_roots_epoch = 11;
  runtime.revocations_epoch = 12;
  runtime.cert_roots_epoch = 13;
  runtime.membership_epoch = 7;
  runtime.model = "edge_consensus_node";
  runtime.fw_git_sha = "agentd_managed_runtime";
  runtime.peer_node_ids = {"node-b", "node-c"};
  runtime.member_node_ids = {"node-a", "node-b", "node-c"};

  const EdgeConsensusExternalProcessPlan plan =
    make_edge_consensus_external_process_plan(cfg, run_cfg, runtime);

  assert(plan.tool_path == "/tmp/edge-consensus-node");
  assert(plan.artifacts.runtime_dir == "/tmp/agentd-runtime/edge_consensus_runtimes/node-a");
  assert(plan.artifacts.stderr_log_path == "/tmp/agentd-runtime/edge_consensus_runtimes/node-a/stderr.log");
  const std::vector<std::string> expected = {
    "/tmp/edge-consensus-node",
    "--daemon-url", "http://127.0.0.1:8123",
    "--node-id", "node-a",
    "--cluster-id", "cluster-a",
    "--manifest-sha256", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "--cluster-size", "3",
    "--outbox-limit", "128",
    "--campaign-delay-ms", "10",
    "--campaign-retry-ms", "20",
    "--campaign-retry-max-ms", "30",
    "--campaign-retry-backoff-factor", "2",
    "--leader-heartbeat-ms", "1000",
    "--leader-lease-ms", "5000",
    "--lease-expiry-recampaign-delay-ms", "333",
    "--poll-interval-ms", "100",
    "--deadline-ms", "2000",
    "--trust-roots-epoch", "11",
    "--revocations-epoch", "12",
    "--cert-roots-epoch", "13",
    "--membership-epoch", "7",
    "--model", "edge_consensus_node",
    "--fw-git-sha", "agentd_managed_runtime",
    "--decision-sha256", "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "--auth-token", "daemon-token",
    "--peer-node-id", "node-b",
    "--peer-node-id", "node-c",
    "--member-node-id", "node-a",
    "--member-node-id", "node-b",
    "--member-node-id", "node-c",
  };
  assert(plan.argv == expected);
}

static void test_startup_ready_event_parser() {
  Json::Value event(Json::objectValue);
  event["schema"] = "edge_node_consensus_runtime_event_v1";
  event["event"] = "startup_ready";
  assert(edge_consensus_runtime_stdout_event_is_startup_ready(event));

  event["event"] = "other";
  assert(!edge_consensus_runtime_stdout_event_is_startup_ready(event));
}

static void test_live_status_event_parser() {
  Json::Value event(Json::objectValue);
  event["schema"] = "edge_node_consensus_live_status_v1";
  Json::Value status(Json::objectValue);
  status["node_id"] = "node-a";
  status["leader"] = true;
  event["status"] = status;

  Json::Value out_status(Json::nullValue);
  assert(edge_consensus_runtime_stdout_event_live_status(event, &out_status));
  assert(out_status["node_id"].asString() == "node-a");
  assert(out_status["leader"].asBool());

  event["schema"] = "other";
  assert(!edge_consensus_runtime_stdout_event_live_status(event, &out_status));
}

}  // namespace

int main() {
  test_runtime_artifacts_follow_state_dir();
  test_external_process_plan_shapes_argv();
  test_startup_ready_event_parser();
  test_live_status_event_parser();
  return 0;
}
