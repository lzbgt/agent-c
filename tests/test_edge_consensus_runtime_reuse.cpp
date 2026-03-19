#include "edge_consensus_runtime_reuse.h"

#include <cassert>

namespace {

using agentd::DaemonConfig;
using agentd::EdgeConsensusClusterPolicy;
using agentd::EdgeConsensusRuntime;
using agentd::EdgeConsensusRuntimeReuseDisposition;
using agentd::EdgeConsensusRuntimeReuseResult;
using agentd::edge_consensus_runtime_evaluate_reuse;

static DaemonConfig make_cfg() {
  DaemonConfig cfg;
  cfg.listen_host = "0.0.0.0";
  cfg.listen_port = 8080;
  cfg.auth_token = "daemon-token";
  cfg.edge_consensus_node_tool_path = "/tmp/edge-consensus-node";
  cfg.edge_auth_trust_roots_epoch = 11;
  cfg.edge_auth_revocations_epoch = 12;
  cfg.edge_auth_cert_roots_epoch = 13;

  EdgeConsensusClusterPolicy pol;
  pol.membership_epoch = 7;
  pol.member_node_ids = {"node-b", "node-a", "node-a"};
  pol.campaign_retry_ms = 1500;
  pol.campaign_retry_max_ms = 2500;
  pol.campaign_retry_backoff_factor = 2;
  pol.leader_heartbeat_ms = 1000;
  pol.leader_lease_ms = 5000;
  pol.lease_expiry_recampaign_delay_ms = 333;
  cfg.edge_consensus_clusters["cluster-a"] = pol;
  return cfg;
}

static Json::Value make_body() {
  Json::Value body(Json::objectValue);
  body["node_id"] = "node-a";
  body["cluster_id"] = "cluster-a";
  body["manifest_sha256"] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  return body;
}

static EdgeConsensusRuntime make_runtime(const std::string& runtime_kind) {
  EdgeConsensusRuntime st;
  st.runtime_kind = runtime_kind;
  st.node_id = "node-a";
  st.cluster_id = "cluster-a";
  st.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  st.daemon_url = runtime_kind == "builtin" ? "@local" : "http://127.0.0.1:8080";
  st.tool_path = runtime_kind == "builtin" ? "@builtin" : "/tmp/edge-consensus-node";
  st.model = "edge_consensus_node";
  st.fw_git_sha = "agentd_managed_runtime";
  st.member_node_ids = {"node-b", "node-a"};
  st.peer_node_ids = {"node-b"};
  st.campaign_delay_ms = 0;
  st.campaign_retry_ms = 1500;
  st.campaign_retry_max_ms = 2500;
  st.campaign_retry_backoff_factor = 2;
  st.leader_heartbeat_ms = 1000;
  st.leader_lease_ms = 5000;
  st.cluster_size = 2;
  st.poll_interval_ms = 100;
  st.deadline_ms = 10000;
  st.outbox_limit = 128;
  st.trust_roots_epoch = 11;
  st.revocations_epoch = 12;
  st.cert_roots_epoch = 13;
  st.membership_epoch = 7;
  st.lease_expiry_recampaign_delay_ms = 333;
  st.running = true;
  return st;
}

static void test_reuse_accepts_same_effective_config() {
  const DaemonConfig cfg = make_cfg();
  const Json::Value body = make_body();
  const EdgeConsensusRuntime current = make_runtime("external");

  EdgeConsensusRuntimeReuseResult result;
  std::string err;
  assert(edge_consensus_runtime_evaluate_reuse(cfg, body, "external", current, &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusRuntimeReuseDisposition::reusable);
  assert(result.runtime["running"].asBool());
  assert(result.runtime["node_id"].asString() == "node-a");
}

static void test_reuse_reports_conflict_for_different_runtime_kind() {
  const DaemonConfig cfg = make_cfg();
  const Json::Value body = make_body();
  const EdgeConsensusRuntime current = make_runtime("external");

  EdgeConsensusRuntimeReuseResult result;
  std::string err;
  assert(edge_consensus_runtime_evaluate_reuse(cfg, body, "builtin", current, &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusRuntimeReuseDisposition::conflict);
  assert(result.error == "consensus runtime already running with different config");
}

static void test_reuse_reports_invalid_request_when_requested_config_invalid() {
  const DaemonConfig cfg = make_cfg();
  Json::Value body = make_body();
  body["cluster_id"] = "bad cluster id";
  const EdgeConsensusRuntime current = make_runtime("external");

  EdgeConsensusRuntimeReuseResult result;
  std::string err;
  assert(edge_consensus_runtime_evaluate_reuse(cfg, body, "external", current, &result, &err));
  assert(err.empty());
  assert(result.disposition == EdgeConsensusRuntimeReuseDisposition::invalid_request);
  assert(!result.error.empty());
}

}  // namespace

int main() {
  test_reuse_accepts_same_effective_config();
  test_reuse_reports_conflict_for_different_runtime_kind();
  test_reuse_reports_invalid_request_when_requested_config_invalid();
  return 0;
}
