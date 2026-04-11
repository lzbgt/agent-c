#pragma once

#include "agent/edge_interop.h"

#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace agentd {

struct EdgeConsensusEpochs {
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
};

struct EdgeConsensusIdentity {
  std::string cluster_id;
  std::string node_id;
  std::string manifest_sha256;
  uint64_t membership_epoch = 0;
  EdgeConsensusEpochs trust_epochs;
};

struct EdgeConsensusFrame {
  std::string schema = AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1;
  std::string frame_id;
  std::string kind;
  uint64_t term = 0;
  std::string decision_sha256;
  std::string candidate_node_id;
  std::string leader_node_id;
  bool granted = false;
  EdgeConsensusIdentity from;
  std::vector<EdgeConsensusIdentity> vote_witnesses;
};

struct EdgeConsensusNodeLoopConfig {
  EdgeConsensusIdentity self;
  std::vector<std::string> peer_node_ids;
  std::vector<std::string> member_node_ids;
  size_t cluster_size = 0;
  int64_t campaign_delay_ms = 0;
  int64_t campaign_retry_ms = 0;
  int64_t campaign_retry_max_ms = 0;
  int64_t campaign_retry_backoff_factor = 1;
  int64_t leader_heartbeat_ms = 1000;
  int64_t leader_lease_ms = 5000;
  int64_t lease_expiry_recampaign_delay_ms = 0;
  std::string decision_sha256;
};

struct EdgeConsensusMembershipPolicyUpdate {
  std::string schema = AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1;
  std::string cluster_id;
  uint64_t membership_epoch = 0;
  std::vector<std::string> member_node_ids;
  int64_t campaign_delay_ms = 0;
  int64_t campaign_retry_ms = 0;
  int64_t campaign_retry_max_ms = 0;
  int64_t campaign_retry_backoff_factor = 1;
  int64_t leader_heartbeat_ms = 1000;
  int64_t leader_lease_ms = 5000;
  int64_t lease_expiry_recampaign_delay_ms = 0;
};

class EdgeConsensusReplica {
 public:
  EdgeConsensusReplica(const EdgeConsensusIdentity& self, size_t cluster_size);

  const EdgeConsensusIdentity& self() const { return self_; }
  uint64_t current_term() const { return current_term_; }
  const std::string& leader_node_id() const { return leader_node_id_; }
  const std::string& committed_decision_sha256() const { return committed_decision_sha256_; }
  uint64_t membership_epoch() const { return self_.membership_epoch; }
  const std::set<std::string>& member_node_ids() const { return member_node_ids_; }
  bool leader_is_self() const { return !leader_node_id_.empty() && leader_node_id_ == self_.node_id; }

  void set_trust_epochs(const EdgeConsensusEpochs& epochs);
  void set_membership(uint64_t membership_epoch, const std::vector<std::string>& member_node_ids);
  EdgeConsensusFrame start_election(const std::string& decision_sha256);
  EdgeConsensusFrame current_leader_commit_frame() const;
  void expire_leader_lease();
  bool handle_frame(const EdgeConsensusFrame& frame, std::vector<EdgeConsensusFrame>* out_frames, std::string* out_error);
  Json::Value status_to_json() const;

 private:
  bool trust_epochs_match(const EdgeConsensusEpochs& other) const;
  bool membership_matches(const EdgeConsensusIdentity& other) const;
  bool node_is_member(const std::string& node_id) const;
  bool has_quorum() const;
  bool leader_commit_witnesses_valid(const EdgeConsensusFrame& frame) const;
  EdgeConsensusFrame make_vote_grant_frame(const std::string& candidate_node_id, const std::string& decision_sha256) const;
  EdgeConsensusFrame make_leader_commit_frame() const;
  void reset_consensus_state();
  void maybe_reset_for_new_term(uint64_t term);
  std::string next_frame_id(const char* kind);

  EdgeConsensusIdentity self_;
  size_t cluster_size_ = 0;
  uint64_t current_term_ = 0;
  uint64_t frame_seq_ = 0;
  std::string voted_for_node_id_;
  std::string leader_node_id_;
  std::string campaign_decision_sha256_;
  std::string committed_decision_sha256_;
  std::set<std::string> member_node_ids_;
  std::map<std::string, EdgeConsensusIdentity> grant_witnesses_by_node_id_;
  std::vector<EdgeConsensusIdentity> committed_vote_witnesses_;
  std::map<std::string, uint64_t> seen_frame_term_by_id_;
};

class EdgeConsensusNodeLoop {
 public:
  explicit EdgeConsensusNodeLoop(const EdgeConsensusNodeLoopConfig& cfg);

  const EdgeConsensusNodeLoopConfig& config() const { return cfg_; }
  const EdgeConsensusReplica& replica() const { return replica_; }
  bool election_started() const { return election_started_; }
  uint64_t campaign_attempts() const { return campaign_attempts_; }
  const std::string& leader_node_id() const { return replica_.leader_node_id(); }
  const std::string& committed_decision_sha256() const { return replica_.committed_decision_sha256(); }
  int64_t current_campaign_delay_ms() const;

  bool adopt_membership_policy(
    const EdgeConsensusMembershipPolicyUpdate& update,
    bool* out_adopted,
    std::string* out_reason
  );
  std::vector<EdgeConsensusFrame> tick(int64_t now_utc_ms);
  bool handle_frame(
    const EdgeConsensusFrame& frame,
    std::vector<EdgeConsensusFrame>* out_frames,
    std::string* out_error,
    int64_t now_utc_ms = 0
  );
  std::vector<std::string> target_node_ids_for_frame(const EdgeConsensusFrame& frame) const;
  Json::Value status_to_json() const;

 private:
  void remember_decision(const std::string& decision_sha256);
  bool leader_lease_expired(int64_t now_utc_ms) const;
  bool lease_expiry_recampaign_delay_active(int64_t now_utc_ms) const;
  void observe_leader_activity(const EdgeConsensusFrame& frame, int64_t now_utc_ms);

  EdgeConsensusNodeLoopConfig cfg_;
  EdgeConsensusReplica replica_;
  int64_t started_utc_ms_ = 0;
  int64_t last_campaign_started_utc_ms_ = 0;
  int64_t last_leader_contact_utc_ms_ = 0;
  int64_t last_leader_heartbeat_sent_utc_ms_ = 0;
  int64_t last_leader_lease_expired_utc_ms_ = 0;
  bool election_started_ = false;
  uint64_t campaign_attempts_ = 0;
  uint64_t leader_lease_expired_count_ = 0;
  std::string last_known_decision_sha256_;
};

Json::Value edge_consensus_epochs_to_json(const EdgeConsensusEpochs& epochs);
Json::Value edge_consensus_identity_to_json(const EdgeConsensusIdentity& identity);
Json::Value edge_consensus_frame_to_json(const EdgeConsensusFrame& frame);

bool edge_consensus_epochs_from_json(const Json::Value& root, EdgeConsensusEpochs* out, std::string* out_error);
bool edge_consensus_identity_from_json(const Json::Value& root, EdgeConsensusIdentity* out, std::string* out_error);
bool edge_consensus_frame_from_json(const Json::Value& root, EdgeConsensusFrame* out, std::string* out_error);
bool edge_consensus_membership_policy_update_from_json(
  const Json::Value& root,
  EdgeConsensusMembershipPolicyUpdate* out,
  std::string* out_error
);

}  // namespace agentd
