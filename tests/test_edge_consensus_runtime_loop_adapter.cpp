#include "edge_consensus_runtime_loop_adapter.h"

#include <cassert>

namespace {

using agentd::EdgeConsensusNodeLoop;
using agentd::EdgeConsensusRuntimeConfig;
using agentd::edge_consensus_runtime_loop_result_json;
using agentd::edge_consensus_runtime_node_loop_config;
using agentd::edge_consensus_runtime_self_identity;

static EdgeConsensusRuntimeConfig make_config() {
  EdgeConsensusRuntimeConfig cfg;
  cfg.node_id = "node-a";
  cfg.cluster_id = "cluster-a";
  cfg.manifest_sha256 =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  cfg.decision_sha256 =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  cfg.peer_node_ids = {"node-b", "node-c"};
  cfg.member_node_ids = {"node-a", "node-b", "node-c"};
  cfg.cluster_size = 3;
  cfg.campaign_delay_ms = 10;
  cfg.campaign_retry_ms = 20;
  cfg.campaign_retry_max_ms = 40;
  cfg.campaign_retry_backoff_factor = 2;
  cfg.leader_heartbeat_ms = 1000;
  cfg.leader_lease_ms = 5000;
  cfg.lease_expiry_recampaign_delay_ms = 333;
  cfg.membership_epoch = 9;
  cfg.trust_roots_epoch = 3;
  cfg.revocations_epoch = 1;
  cfg.cert_roots_epoch = 5;
  return cfg;
}

static void test_self_identity_carries_cluster_membership_and_trust() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
  const auto self = edge_consensus_runtime_self_identity(cfg);
  assert(self.cluster_id == "cluster-a");
  assert(self.node_id == "node-a");
  assert(self.manifest_sha256 == cfg.manifest_sha256);
  assert(self.membership_epoch == 9);
  assert(self.trust_epochs.trust_roots_epoch == 3);
  assert(self.trust_epochs.revocations_epoch == 1);
  assert(self.trust_epochs.cert_roots_epoch == 5);
}

static void test_node_loop_config_carries_runtime_policy_fields() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
  const auto loop_cfg = edge_consensus_runtime_node_loop_config(cfg);
  assert(loop_cfg.self.node_id == "node-a");
  assert(loop_cfg.peer_node_ids.size() == 2);
  assert(loop_cfg.member_node_ids.size() == 3);
  assert(loop_cfg.cluster_size == 3);
  assert(loop_cfg.campaign_delay_ms == 10);
  assert(loop_cfg.campaign_retry_ms == 20);
  assert(loop_cfg.campaign_retry_max_ms == 40);
  assert(loop_cfg.campaign_retry_backoff_factor == 2);
  assert(loop_cfg.leader_heartbeat_ms == 1000);
  assert(loop_cfg.leader_lease_ms == 5000);
  assert(loop_cfg.lease_expiry_recampaign_delay_ms == 333);
  assert(loop_cfg.decision_sha256 == cfg.decision_sha256);
}

static void test_result_json_surfaces_error_status_and_commit_fields() {
  const EdgeConsensusRuntimeConfig cfg = make_config();
  EdgeConsensusNodeLoop loop(edge_consensus_runtime_node_loop_config(cfg));

  const Json::Value stopped = edge_consensus_runtime_loop_result_json(cfg, loop, false, "stopped");
  assert(!stopped["ok"].asBool());
  assert(stopped["node_id"].asString() == "node-a");
  assert(stopped["error"].asString() == "stopped");
  assert(stopped["status"].isObject());
  assert(stopped["current_term"].asUInt64() == 0);
  assert(!stopped.isMember("leader_node_id"));
  assert(!stopped.isMember("committed_decision_sha256"));

  const Json::Value committed = edge_consensus_runtime_loop_result_json(cfg, loop, true, "");
  assert(committed["ok"].asBool());
  assert(committed["node_id"].asString() == "node-a");
  assert(committed["leader_node_id"].isString());
  assert(committed["committed_decision_sha256"].isString());
  assert(committed["status"].isObject());
}

}  // namespace

int main() {
  test_self_identity_carries_cluster_membership_and_trust();
  test_node_loop_config_carries_runtime_policy_fields();
  test_result_json_surfaces_error_status_and_commit_fields();
  return 0;
}
