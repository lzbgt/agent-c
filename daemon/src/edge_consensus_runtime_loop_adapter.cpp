#include "edge_consensus_runtime_loop_adapter.h"

namespace agentd {

EdgeConsensusIdentity edge_consensus_runtime_self_identity(
  const EdgeConsensusRuntimeConfig& cfg
) {
  EdgeConsensusIdentity self;
  self.cluster_id = cfg.cluster_id;
  self.node_id = cfg.node_id;
  self.manifest_sha256 = cfg.manifest_sha256;
  self.membership_epoch = cfg.membership_epoch;
  self.trust_epochs.trust_roots_epoch = cfg.trust_roots_epoch;
  self.trust_epochs.revocations_epoch = cfg.revocations_epoch;
  self.trust_epochs.cert_roots_epoch = cfg.cert_roots_epoch;
  return self;
}

EdgeConsensusNodeLoopConfig edge_consensus_runtime_node_loop_config(
  const EdgeConsensusRuntimeConfig& cfg
) {
  EdgeConsensusNodeLoopConfig loop_cfg;
  loop_cfg.self = edge_consensus_runtime_self_identity(cfg);
  loop_cfg.peer_node_ids = cfg.peer_node_ids;
  loop_cfg.member_node_ids = cfg.member_node_ids;
  loop_cfg.cluster_size = cfg.cluster_size;
  loop_cfg.campaign_delay_ms = cfg.campaign_delay_ms;
  loop_cfg.campaign_retry_ms = cfg.campaign_retry_ms;
  loop_cfg.campaign_retry_max_ms = cfg.campaign_retry_max_ms;
  loop_cfg.campaign_retry_backoff_factor = cfg.campaign_retry_backoff_factor;
  loop_cfg.leader_heartbeat_ms = cfg.leader_heartbeat_ms;
  loop_cfg.leader_lease_ms = cfg.leader_lease_ms;
  loop_cfg.lease_expiry_recampaign_delay_ms = cfg.lease_expiry_recampaign_delay_ms;
  loop_cfg.decision_sha256 = cfg.decision_sha256;
  return loop_cfg;
}

Json::Value edge_consensus_runtime_loop_result_json(
  const EdgeConsensusRuntimeConfig& cfg,
  const EdgeConsensusNodeLoop& loop,
  bool ok,
  const std::string& error
) {
  Json::Value result(Json::objectValue);
  result["ok"] = ok;
  result["node_id"] = cfg.node_id;
  if (!error.empty()) result["error"] = error;
  if (ok) {
    result["leader_node_id"] = loop.leader_node_id();
    result["committed_decision_sha256"] = loop.committed_decision_sha256();
  }
  result["current_term"] = Json::UInt64(loop.replica().current_term());
  result["status"] = loop.status_to_json();
  return result;
}

}  // namespace agentd
