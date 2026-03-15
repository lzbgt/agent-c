#pragma once

#include <json/json.h>

#include <cstddef>
#include <cstdint>
#include <map>
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
  EdgeConsensusEpochs trust_epochs;
};

struct EdgeConsensusFrame {
  std::string schema = "edge_node_consensus_frame_v1";
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
  size_t cluster_size = 0;
  int64_t campaign_delay_ms = 0;
  std::string decision_sha256;
};

class EdgeConsensusReplica {
 public:
  EdgeConsensusReplica(const EdgeConsensusIdentity& self, size_t cluster_size);

  const EdgeConsensusIdentity& self() const { return self_; }
  uint64_t current_term() const { return current_term_; }
  const std::string& leader_node_id() const { return leader_node_id_; }
  const std::string& committed_decision_sha256() const { return committed_decision_sha256_; }

  void set_trust_epochs(const EdgeConsensusEpochs& epochs);
  EdgeConsensusFrame start_election(const std::string& decision_sha256);
  bool handle_frame(const EdgeConsensusFrame& frame, std::vector<EdgeConsensusFrame>* out_frames, std::string* out_error);
  Json::Value status_to_json() const;

 private:
  bool trust_epochs_match(const EdgeConsensusEpochs& other) const;
  bool has_quorum() const;
  EdgeConsensusFrame make_vote_grant_frame(const std::string& candidate_node_id, const std::string& decision_sha256) const;
  EdgeConsensusFrame make_leader_commit_frame() const;
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
  const std::string& leader_node_id() const { return replica_.leader_node_id(); }
  const std::string& committed_decision_sha256() const { return replica_.committed_decision_sha256(); }

  std::vector<EdgeConsensusFrame> tick(int64_t now_utc_ms);
  bool handle_frame(const EdgeConsensusFrame& frame, std::vector<EdgeConsensusFrame>* out_frames, std::string* out_error);
  std::vector<std::string> target_node_ids_for_frame(const EdgeConsensusFrame& frame) const;
  Json::Value status_to_json() const;

 private:
  EdgeConsensusNodeLoopConfig cfg_;
  EdgeConsensusReplica replica_;
  int64_t started_utc_ms_ = 0;
  bool election_started_ = false;
};

Json::Value edge_consensus_epochs_to_json(const EdgeConsensusEpochs& epochs);
Json::Value edge_consensus_identity_to_json(const EdgeConsensusIdentity& identity);
Json::Value edge_consensus_frame_to_json(const EdgeConsensusFrame& frame);

bool edge_consensus_epochs_from_json(const Json::Value& root, EdgeConsensusEpochs* out, std::string* out_error);
bool edge_consensus_identity_from_json(const Json::Value& root, EdgeConsensusIdentity* out, std::string* out_error);
bool edge_consensus_frame_from_json(const Json::Value& root, EdgeConsensusFrame* out, std::string* out_error);

}  // namespace agentd
