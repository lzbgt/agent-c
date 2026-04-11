#include "edge_node_consensus.h"

#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include "agent/edge_interop.h"

#include <algorithm>
#include <limits>
#include <set>

namespace agentd {
namespace {

static bool parse_nonempty_string(
  const Json::Value& root,
  const char* key,
  std::string* out,
  std::string* out_error
) {
  if (out) out->clear();
  if (!root.isMember(key) || !root[key].isString() || trim_copy(root[key].asString()).empty()) {
    if (out_error) *out_error = std::string(key) + " must be a non-empty string";
    return false;
  }
  if (out) *out = trim_copy(root[key].asString());
  return true;
}

static bool consensus_member_node_id_is_valid(const std::string& node_id) {
  return agent_edge_consensus_member_node_id_is_valid(node_id.data(), node_id.size()) == 1;
}

static bool consensus_node_id_matches(const std::string& a, const std::string& b) {
  return agent_edge_consensus_node_id_matches(a.data(), a.size(), b.data(), b.size()) == 1;
}

static bool consensus_sha256_token_is_valid(const std::string& token) {
  return agent_umbmp_sha256_token_is_safe(token.data(), token.size()) == 1;
}

static const char* consensus_identity_validation_error(agent_edge_consensus_identity_validation_t validation) {
  switch (validation) {
    case AGENT_EDGE_CONSENSUS_IDENTITY_OK:
      return "";
    case AGENT_EDGE_CONSENSUS_IDENTITY_CLUSTER_ID_INVALID:
      return "cluster_id invalid";
    case AGENT_EDGE_CONSENSUS_IDENTITY_NODE_ID_INVALID:
      return "node_id invalid";
    case AGENT_EDGE_CONSENSUS_IDENTITY_MANIFEST_SHA256_INVALID:
      return "manifest_sha256 invalid";
  }
  return "identity invalid";
}

static const char* consensus_frame_validation_error(agent_edge_consensus_frame_validation_t validation) {
  switch (validation) {
    case AGENT_EDGE_CONSENSUS_FRAME_OK:
      return "";
    case AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_INVALID:
      return "schema invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_KIND_INVALID:
      return "kind invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_ID_INVALID:
      return "frame_id invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_TERM_INVALID:
      return "term must be >= 1";
    case AGENT_EDGE_CONSENSUS_FRAME_DECISION_SHA256_INVALID:
      return "decision_sha256 invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_FROM_CLUSTER_ID_INVALID:
      return "cluster_id invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_FROM_NODE_ID_INVALID:
      return "node_id invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_FROM_MANIFEST_SHA256_INVALID:
      return "manifest_sha256 invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_CANDIDATE_NODE_ID_INVALID:
      return "candidate_node_id invalid";
    case AGENT_EDGE_CONSENSUS_FRAME_LEADER_NODE_ID_INVALID:
      return "leader_node_id invalid";
  }
  return "frame invalid";
}

static const char* consensus_membership_policy_header_validation_error(
  agent_edge_consensus_membership_policy_header_validation_t validation
) {
  switch (validation) {
    case AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_HEADER_OK:
      return "";
    case AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_SCHEMA_INVALID:
      return "membership schema invalid";
    case AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_CLUSTER_ID_INVALID:
      return "cluster_id invalid";
  }
  return "membership policy invalid";
}

static bool parse_optional_sha256(
  const Json::Value& root,
  const char* key,
  std::string* out,
  std::string* out_error
) {
  if (out) out->clear();
  if (!root.isMember(key) || root[key].isNull()) return true;
  if (!root[key].isString()) {
    if (out_error) *out_error = std::string(key) + " must be a string";
    return false;
  }
  const std::string v = trim_copy(root[key].asString());
  if (v.empty()) return true;
  if (!consensus_sha256_token_is_valid(v)) {
    if (out_error) *out_error = std::string(key) + " must be a sha256 token";
    return false;
  }
  if (out) *out = v;
  return true;
}

static bool parse_identity_like(const EdgeConsensusIdentity& id, std::string* out_error) {
  if (out_error) out_error->clear();
  const agent_edge_consensus_identity_validation_t validation = agent_edge_consensus_identity_validate(
    id.cluster_id.data(),
    id.cluster_id.size(),
    id.node_id.data(),
    id.node_id.size(),
    id.manifest_sha256.data(),
    id.manifest_sha256.size());
  if (validation != AGENT_EDGE_CONSENSUS_IDENTITY_OK) {
    if (out_error) *out_error = consensus_identity_validation_error(validation);
    return false;
  }
  return true;
}

static std::set<std::string> dedupe_member_ids(
  const std::vector<std::string>& raw_ids,
  const std::string& self_node_id
) {
  std::set<std::string> out;
  if (consensus_member_node_id_is_valid(self_node_id)) out.insert(self_node_id);
  for (const auto& raw : raw_ids) {
    const std::string node_id = trim_copy(raw);
    if (!consensus_member_node_id_is_valid(node_id)) continue;
    out.insert(node_id);
  }
  return out;
}

static bool frame_is_valid(const EdgeConsensusFrame& frame, std::string* out_error) {
  if (out_error) out_error->clear();
  const agent_edge_consensus_frame_validation_t validation = agent_edge_consensus_frame_validate(
    frame.schema.data(),
    frame.schema.size(),
    frame.kind.data(),
    frame.kind.size(),
    frame.frame_id.data(),
    frame.frame_id.size(),
    frame.term,
    frame.decision_sha256.data(),
    frame.decision_sha256.size(),
    frame.from.cluster_id.data(),
    frame.from.cluster_id.size(),
    frame.from.node_id.data(),
    frame.from.node_id.size(),
    frame.from.manifest_sha256.data(),
    frame.from.manifest_sha256.size(),
    frame.candidate_node_id.data(),
    frame.candidate_node_id.size(),
    frame.leader_node_id.data(),
    frame.leader_node_id.size());
  if (validation != AGENT_EDGE_CONSENSUS_FRAME_OK) {
    if (out_error) *out_error = consensus_frame_validation_error(validation);
    return false;
  }
  for (const auto& witness : frame.vote_witnesses) {
    if (!parse_identity_like(witness, out_error)) return false;
  }
  return true;
}

static std::vector<std::string> dedupe_loop_targets(
  const std::vector<std::string>& raw_ids,
  const std::string& self_node_id
) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& raw : raw_ids) {
    const std::string node_id = trim_copy(raw);
    if (!consensus_member_node_id_is_valid(node_id) || consensus_node_id_matches(node_id, self_node_id)) continue;
    if (!seen.insert(node_id).second) continue;
    out.push_back(node_id);
  }
  return out;
}

static std::vector<std::string> dedupe_member_vector_without_forcing_self(
  const std::vector<std::string>& raw_ids
) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& raw : raw_ids) {
    const std::string node_id = trim_copy(raw);
    if (!consensus_member_node_id_is_valid(node_id)) continue;
    if (!seen.insert(node_id).second) continue;
    out.push_back(node_id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

static bool json_value_to_i64_strict(const Json::Value& v, int64_t* out) {
  if (!out) return false;
  if (v.isInt64()) {
    *out = v.asInt64();
    return true;
  }
  if (v.isUInt64()) {
    const Json::UInt64 x = v.asUInt64();
    if (x > (Json::UInt64)std::numeric_limits<int64_t>::max()) return false;
    *out = (int64_t)x;
    return true;
  }
  if (v.isInt()) {
    *out = (int64_t)v.asInt();
    return true;
  }
  if (v.isUInt()) {
    *out = (int64_t)v.asUInt();
    return true;
  }
  return false;
}

static bool parse_optional_i64_field(
  const Json::Value& root,
  const char* key,
  int64_t* out,
  std::string* out_error
) {
  if (!root.isMember(key) || root[key].isNull()) return true;
  if (!json_value_to_i64_strict(root[key], out)) {
    if (out_error) *out_error = std::string(key) + " must be int64";
    return false;
  }
  return true;
}

static std::string consensus_frame_id(
  const std::string& node_id,
  const char* kind,
  uint64_t number,
  const std::string& suffix = std::string()
) {
  char out[AGENT_UM_BMP_MAX_ID_LEN + 1];
  size_t out_len = 0;
  const std::string kind_s = kind ? std::string(kind) : std::string();
  const agent_status_t st = agent_edge_consensus_frame_id_format(
    node_id.data(),
    node_id.size(),
    kind_s.data(),
    kind_s.size(),
    number,
    suffix.empty() ? nullptr : suffix.data(),
    suffix.size(),
    out,
    sizeof(out),
    &out_len);
  if (st != AGENT_OK) return {};
  return std::string(out, out_len);
}

}  // namespace

EdgeConsensusReplica::EdgeConsensusReplica(const EdgeConsensusIdentity& self, size_t cluster_size)
    : self_(self), cluster_size_(agent_edge_consensus_cluster_size_normalize(cluster_size)) {
  member_node_ids_.insert(self_.node_id);
}

EdgeConsensusNodeLoop::EdgeConsensusNodeLoop(const EdgeConsensusNodeLoopConfig& cfg)
    : cfg_(cfg),
      replica_(
        cfg.self,
        cfg.cluster_size == 0
          ? agent_edge_consensus_cluster_size_from_peer_count(cfg.peer_node_ids.size())
          : agent_edge_consensus_cluster_size_normalize(cfg.cluster_size)) {
  cfg_.peer_node_ids = dedupe_loop_targets(cfg.peer_node_ids, cfg.self.node_id);
  if (cfg_.member_node_ids.empty()) {
    cfg_.member_node_ids = cfg_.peer_node_ids;
    cfg_.member_node_ids.push_back(cfg_.self.node_id);
  }
  const std::set<std::string> member_ids = dedupe_member_ids(cfg_.member_node_ids, cfg_.self.node_id);
  cfg_.member_node_ids.assign(member_ids.begin(), member_ids.end());
  if (cfg_.cluster_size == 0) {
    cfg_.cluster_size = agent_edge_consensus_cluster_size_from_member_count(cfg_.member_node_ids.size());
  } else {
    cfg_.cluster_size = agent_edge_consensus_cluster_size_normalize(cfg_.cluster_size);
  }
  agent_edge_consensus_policy_timing_t timing;
  timing.campaign_delay_ms = cfg_.campaign_delay_ms;
  timing.campaign_retry_ms = cfg_.campaign_retry_ms;
  timing.campaign_retry_max_ms = cfg_.campaign_retry_max_ms <= 0
    ? cfg_.campaign_retry_ms
    : cfg_.campaign_retry_max_ms;
  timing.campaign_retry_backoff_factor = cfg_.campaign_retry_backoff_factor;
  timing.leader_heartbeat_ms = cfg_.leader_heartbeat_ms;
  timing.leader_lease_ms = cfg_.leader_lease_ms;
  timing.lease_expiry_recampaign_delay_ms = cfg_.lease_expiry_recampaign_delay_ms;
  timing.stale_runtime_recovery_grace_ms = 0;
  if (agent_edge_consensus_policy_timing_normalize(&timing) == AGENT_OK) {
    cfg_.campaign_delay_ms = timing.campaign_delay_ms;
    cfg_.campaign_retry_ms = timing.campaign_retry_ms;
    cfg_.campaign_retry_max_ms = timing.campaign_retry_max_ms;
    cfg_.campaign_retry_backoff_factor = timing.campaign_retry_backoff_factor;
    cfg_.leader_heartbeat_ms = timing.leader_heartbeat_ms;
    cfg_.leader_lease_ms = timing.leader_lease_ms;
    cfg_.lease_expiry_recampaign_delay_ms = timing.lease_expiry_recampaign_delay_ms;
  }
  remember_decision(cfg_.decision_sha256);
  replica_.set_membership(cfg_.self.membership_epoch, cfg_.member_node_ids);
}

void EdgeConsensusReplica::set_trust_epochs(const EdgeConsensusEpochs& epochs) {
  self_.trust_epochs = epochs;
}

void EdgeConsensusReplica::set_membership(uint64_t membership_epoch, const std::vector<std::string>& member_node_ids) {
  const std::set<std::string> next_member_node_ids = dedupe_member_ids(member_node_ids, self_.node_id);
  const bool changed = self_.membership_epoch != membership_epoch || member_node_ids_ != next_member_node_ids;
  self_.membership_epoch = membership_epoch;
  member_node_ids_ = next_member_node_ids;
  cluster_size_ = agent_edge_consensus_cluster_size_from_member_count(member_node_ids_.size());
  if (changed) reset_consensus_state();
}

void EdgeConsensusReplica::reset_consensus_state() {
  current_term_ = 0;
  voted_for_node_id_.clear();
  leader_node_id_.clear();
  campaign_decision_sha256_.clear();
  committed_decision_sha256_.clear();
  committed_vote_witnesses_.clear();
  grant_witnesses_by_node_id_.clear();
  seen_frame_term_by_id_.clear();
}

std::string EdgeConsensusReplica::next_frame_id(const char* kind) {
  frame_seq_ = agent_edge_consensus_next_frame_sequence(frame_seq_);
  return consensus_frame_id(self_.node_id, kind, frame_seq_);
}

void EdgeConsensusReplica::maybe_reset_for_new_term(uint64_t term) {
  if (!agent_edge_consensus_incoming_term_advances(current_term_, term)) return;
  current_term_ = term;
  voted_for_node_id_.clear();
  leader_node_id_.clear();
  campaign_decision_sha256_.clear();
  committed_decision_sha256_.clear();
  committed_vote_witnesses_.clear();
  grant_witnesses_by_node_id_.clear();
}

bool EdgeConsensusReplica::trust_epochs_match(const EdgeConsensusEpochs& other) const {
  return agent_edge_consensus_trust_epochs_match(
           self_.trust_epochs.trust_roots_epoch,
           self_.trust_epochs.revocations_epoch,
           self_.trust_epochs.cert_roots_epoch,
           other.trust_roots_epoch,
           other.revocations_epoch,
           other.cert_roots_epoch) != 0;
}

bool EdgeConsensusReplica::node_is_member(const std::string& node_id) const {
  return consensus_member_node_id_is_valid(node_id) && member_node_ids_.find(node_id) != member_node_ids_.end();
}

bool EdgeConsensusReplica::membership_matches(const EdgeConsensusIdentity& other) const {
  return agent_edge_consensus_identity_membership_matches(
           self_.membership_epoch,
           other.membership_epoch,
           node_is_member(other.node_id) ? 1 : 0) != 0;
}

bool EdgeConsensusReplica::has_quorum() const {
  const size_t votes = agent_edge_consensus_vote_count_with_self(grant_witnesses_by_node_id_.size());
  return agent_edge_consensus_has_quorum(cluster_size_, votes) != 0;
}

bool EdgeConsensusReplica::leader_commit_witnesses_valid(const EdgeConsensusFrame& frame) const {
  size_t valid_witness_count = 0;
  bool leader_is_witness = false;
  std::set<std::string> seen_node_ids;
  for (const auto& witness : frame.vote_witnesses) {
    const bool inserted = seen_node_ids.insert(witness.node_id).second;
    if (!agent_edge_consensus_leader_commit_witness_can_count(
          membership_matches(witness) ? 1 : 0,
          trust_epochs_match(witness.trust_epochs) ? 1 : 0,
          inserted ? 0 : 1)) continue;
    valid_witness_count++;
    if (consensus_node_id_matches(witness.node_id, frame.leader_node_id)) leader_is_witness = true;
  }
  return agent_edge_consensus_leader_commit_witnesses_can_accept(
           cluster_size_, valid_witness_count, leader_is_witness ? 1 : 0) != 0;
}

EdgeConsensusFrame EdgeConsensusReplica::make_vote_grant_frame(
  const std::string& candidate_node_id,
  const std::string& decision_sha256
) const {
  EdgeConsensusFrame out;
  out.frame_id = consensus_frame_id(
    self_.node_id,
    AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT,
    current_term_,
    candidate_node_id);
  out.kind = AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT;
  out.term = current_term_;
  out.decision_sha256 = decision_sha256;
  out.candidate_node_id = candidate_node_id;
  out.granted = true;
  out.from = self_;
  return out;
}

EdgeConsensusFrame EdgeConsensusReplica::make_leader_commit_frame(const std::string& frame_id) const {
  EdgeConsensusFrame out;
  out.frame_id = frame_id;
  out.kind = AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT;
  out.term = current_term_;
  out.decision_sha256 = committed_decision_sha256_;
  out.leader_node_id = leader_node_id_;
  out.from = self_;
  out.vote_witnesses = committed_vote_witnesses_;
  return out;
}

EdgeConsensusFrame EdgeConsensusReplica::current_leader_commit_frame() {
  return make_leader_commit_frame(next_frame_id(AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT));
}

void EdgeConsensusReplica::expire_leader_lease() {
  if (leader_is_self()) return;
  leader_node_id_.clear();
  voted_for_node_id_.clear();
  campaign_decision_sha256_.clear();
  committed_decision_sha256_.clear();
  committed_vote_witnesses_.clear();
  grant_witnesses_by_node_id_.clear();
}

EdgeConsensusFrame EdgeConsensusReplica::start_election(const std::string& decision_sha256) {
  campaign_decision_sha256_ = decision_sha256;
  committed_decision_sha256_.clear();
  leader_node_id_.clear();
  grant_witnesses_by_node_id_.clear();
  committed_vote_witnesses_.clear();
  current_term_ = agent_edge_consensus_next_term(current_term_);
  voted_for_node_id_ = self_.node_id;

  EdgeConsensusFrame out;
  out.frame_id = next_frame_id(AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST);
  out.kind = AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST;
  out.term = current_term_;
  out.decision_sha256 = decision_sha256;
  out.candidate_node_id = self_.node_id;
  out.from = self_;
  return out;
}

void EdgeConsensusNodeLoop::remember_decision(const std::string& decision_sha256) {
  const std::string sha = trim_copy(decision_sha256);
  if (!sha.empty()) last_known_decision_sha256_ = sha;
}

bool EdgeConsensusReplica::handle_frame(
  const EdgeConsensusFrame& frame,
  std::vector<EdgeConsensusFrame>* out_frames,
  std::string* out_error,
  bool* out_accepted
) {
  if (out_error) out_error->clear();
  if (out_frames) out_frames->clear();
  if (out_accepted) *out_accepted = false;
  std::string verr;
  if (!frame_is_valid(frame, &verr)) {
    if (out_error) *out_error = verr;
    return false;
  }
  if (!agent_edge_consensus_cluster_id_matches(
        self_.cluster_id.data(),
        self_.cluster_id.size(),
        frame.from.cluster_id.data(),
        frame.from.cluster_id.size())) {
    if (out_error) *out_error = "cluster_id mismatch";
    return false;
  }
  if (!membership_matches(frame.from)) return true;
  auto seen_it = seen_frame_term_by_id_.find(frame.frame_id);
  if (agent_edge_consensus_seen_frame_should_drop(
        seen_it != seen_frame_term_by_id_.end() ? 1 : 0,
        seen_it != seen_frame_term_by_id_.end() ? seen_it->second : 0,
        frame.term)) return true;
  auto mark_frame_seen = [&]() {
    seen_frame_term_by_id_[frame.frame_id] = frame.term;
  };

  if (frame.kind == AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST) {
    if (!agent_edge_consensus_vote_request_can_grant(
          current_term_,
          frame.term,
          node_is_member(frame.candidate_node_id) ? 1 : 0,
          consensus_node_id_matches(frame.candidate_node_id, frame.from.node_id) ? 1 : 0,
          trust_epochs_match(frame.from.trust_epochs) ? 1 : 0,
          voted_for_node_id_.data(),
          voted_for_node_id_.size(),
          frame.candidate_node_id.data(),
          frame.candidate_node_id.size())) return true;
    mark_frame_seen();
    if (out_accepted) *out_accepted = true;
    maybe_reset_for_new_term(frame.term);
    voted_for_node_id_ = frame.candidate_node_id;
    if (out_frames) out_frames->push_back(make_vote_grant_frame(frame.candidate_node_id, frame.decision_sha256));
    return true;
  }

  if (frame.kind == AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT) {
    if (agent_edge_consensus_incoming_term_is_stale(current_term_, frame.term)) return true;
    maybe_reset_for_new_term(frame.term);
    if (!agent_edge_consensus_vote_grant_can_count(
          current_term_,
          frame.term,
          node_is_member(frame.candidate_node_id) ? 1 : 0,
          consensus_node_id_matches(frame.candidate_node_id, self_.node_id) ? 1 : 0,
          consensus_node_id_matches(frame.from.node_id, frame.candidate_node_id) ? 1 : 0,
          agent_edge_consensus_decision_sha256_matches(
            campaign_decision_sha256_.data(),
            campaign_decision_sha256_.size(),
            frame.decision_sha256.data(),
            frame.decision_sha256.size()),
          frame.granted ? 1 : 0,
          trust_epochs_match(frame.from.trust_epochs) ? 1 : 0)) return true;
    mark_frame_seen();
    if (out_accepted) *out_accepted = true;
    grant_witnesses_by_node_id_[frame.from.node_id] = frame.from;
    if (!agent_edge_consensus_candidate_can_commit(
          leader_node_id_.empty() ? 0 : 1,
          has_quorum() ? 1 : 0)) return true;
    leader_node_id_ = self_.node_id;
    committed_decision_sha256_ = campaign_decision_sha256_;
    committed_vote_witnesses_.clear();
    committed_vote_witnesses_.push_back(self_);
    for (const auto& kv : grant_witnesses_by_node_id_) committed_vote_witnesses_.push_back(kv.second);
    if (out_frames) out_frames->push_back(make_leader_commit_frame(
      consensus_frame_id(
        self_.node_id,
        AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT,
        current_term_,
        leader_node_id_)));
    return true;
  }

  if (!agent_edge_consensus_leader_commit_can_accept(
        current_term_,
        frame.term,
        node_is_member(frame.leader_node_id) ? 1 : 0,
        consensus_node_id_matches(frame.from.node_id, frame.leader_node_id) ? 1 : 0,
        trust_epochs_match(frame.from.trust_epochs) ? 1 : 0)) return true;
  if (!leader_commit_witnesses_valid(frame)) return true;
  mark_frame_seen();
  if (out_accepted) *out_accepted = true;
  maybe_reset_for_new_term(frame.term);
  leader_node_id_ = frame.leader_node_id;
  voted_for_node_id_ = frame.leader_node_id;
  campaign_decision_sha256_.clear();
  committed_decision_sha256_ = frame.decision_sha256;
  committed_vote_witnesses_ = frame.vote_witnesses;
  return true;
}

int64_t EdgeConsensusNodeLoop::current_campaign_delay_ms() const {
  return agent_edge_consensus_campaign_retry_delay_ms(
    cfg_.campaign_retry_ms, cfg_.campaign_retry_max_ms, cfg_.campaign_retry_backoff_factor, campaign_attempts_);
}

bool EdgeConsensusNodeLoop::adopt_membership_policy(
  const EdgeConsensusMembershipPolicyUpdate& update,
  bool* out_adopted,
  std::string* out_reason
) {
  if (out_adopted) *out_adopted = false;
  if (out_reason) out_reason->clear();
  if (update.schema != AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1) {
    if (out_reason) *out_reason = "membership schema invalid";
    return false;
  }
  if (!agent_edge_consensus_cluster_id_matches(
        cfg_.self.cluster_id.data(),
        cfg_.self.cluster_id.size(),
        update.cluster_id.data(),
        update.cluster_id.size())) {
    if (out_reason) *out_reason = "cluster_id mismatch";
    return true;
  }
  if (!agent_edge_consensus_membership_epoch_can_advance(cfg_.self.membership_epoch, update.membership_epoch)) {
    if (out_reason) *out_reason = "membership_epoch not newer";
    return true;
  }

  std::vector<std::string> members = dedupe_member_vector_without_forcing_self(update.member_node_ids);
  if (members.empty()) {
    if (out_reason) *out_reason = "member_node_ids empty";
    return false;
  }
  const bool self_node_is_member =
    std::any_of(members.begin(), members.end(), [&](const std::string& node_id) {
      return consensus_node_id_matches(node_id, cfg_.self.node_id);
    });
  if (!agent_edge_consensus_membership_policy_can_adopt(
        cfg_.self.membership_epoch,
        update.membership_epoch,
        self_node_is_member ? 1 : 0)) {
    if (out_reason) *out_reason = "self node missing from membership";
    return true;
  }

  agent_edge_consensus_policy_timing_t timing;
  timing.campaign_delay_ms = update.campaign_delay_ms;
  timing.campaign_retry_ms = update.campaign_retry_ms;
  timing.campaign_retry_max_ms = update.campaign_retry_max_ms <= 0
    ? update.campaign_retry_ms
    : update.campaign_retry_max_ms;
  timing.campaign_retry_backoff_factor = update.campaign_retry_backoff_factor;
  timing.leader_heartbeat_ms = update.leader_heartbeat_ms;
  timing.leader_lease_ms = update.leader_lease_ms;
  timing.lease_expiry_recampaign_delay_ms = update.lease_expiry_recampaign_delay_ms;
  timing.stale_runtime_recovery_grace_ms = 0;
  if (agent_edge_consensus_policy_timing_normalize(&timing) != AGENT_OK) {
    if (out_reason) *out_reason = "membership timing invalid";
    return false;
  }

  cfg_.self.membership_epoch = update.membership_epoch;
  cfg_.member_node_ids = members;
  cfg_.peer_node_ids = dedupe_loop_targets(members, cfg_.self.node_id);
  cfg_.cluster_size = agent_edge_consensus_cluster_size_from_member_count(members.size());
  cfg_.campaign_delay_ms = timing.campaign_delay_ms;
  cfg_.campaign_retry_ms = timing.campaign_retry_ms;
  cfg_.campaign_retry_max_ms = timing.campaign_retry_max_ms;
  cfg_.campaign_retry_backoff_factor = timing.campaign_retry_backoff_factor;
  cfg_.leader_heartbeat_ms = timing.leader_heartbeat_ms;
  cfg_.leader_lease_ms = timing.leader_lease_ms;
  cfg_.lease_expiry_recampaign_delay_ms = timing.lease_expiry_recampaign_delay_ms;

  replica_.set_membership(cfg_.self.membership_epoch, cfg_.member_node_ids);
  started_utc_ms_ = 0;
  last_campaign_started_utc_ms_ = 0;
  last_leader_contact_utc_ms_ = 0;
  last_leader_heartbeat_sent_utc_ms_ = 0;
  last_leader_lease_expired_utc_ms_ = 0;
  election_started_ = false;
  campaign_attempts_ = 0;
  leader_lease_expired_count_ = 0;
  if (out_adopted) *out_adopted = true;
  if (out_reason) *out_reason = "adopted";
  return true;
}

bool EdgeConsensusNodeLoop::leader_lease_expired(int64_t now_utc_ms) const {
  return agent_edge_consensus_leader_lease_expired(
           cfg_.leader_lease_ms,
           now_utc_ms,
           last_leader_contact_utc_ms_,
           replica_.leader_node_id().empty() ? 0 : 1,
           replica_.leader_is_self() ? 1 : 0) != 0;
}

bool EdgeConsensusNodeLoop::lease_expiry_recampaign_delay_active(int64_t now_utc_ms) const {
  return agent_edge_consensus_lease_expiry_recampaign_delay_active(
           cfg_.lease_expiry_recampaign_delay_ms,
           now_utc_ms,
           last_leader_lease_expired_utc_ms_) != 0;
}

void EdgeConsensusNodeLoop::observe_leader_activity(const EdgeConsensusFrame& frame, int64_t now_utc_ms) {
  if (!agent_edge_consensus_leader_activity_can_observe(
        frame.kind.data(),
        frame.kind.size(),
        frame.leader_node_id.data(),
        frame.leader_node_id.size(),
        now_utc_ms)) return;
  last_leader_contact_utc_ms_ = now_utc_ms;
  remember_decision(frame.decision_sha256);
}

std::vector<EdgeConsensusFrame> EdgeConsensusNodeLoop::tick(int64_t now_utc_ms) {
  std::vector<EdgeConsensusFrame> out;
  if (started_utc_ms_ == 0) started_utc_ms_ = now_utc_ms;
  remember_decision(replica_.committed_decision_sha256());
  const std::string committed_decision_sha256 = replica_.committed_decision_sha256();
  const int has_committed_decision = agent_edge_consensus_decision_sha256_is_set(
    committed_decision_sha256.data(),
    committed_decision_sha256.size());

  if (has_committed_decision && replica_.leader_is_self()) {
    if (agent_edge_consensus_leader_heartbeat_due(
          cfg_.leader_heartbeat_ms,
          now_utc_ms,
          last_leader_heartbeat_sent_utc_ms_,
          replica_.leader_is_self() ? 1 : 0,
          has_committed_decision)) {
      out.push_back(replica_.current_leader_commit_frame());
      last_leader_heartbeat_sent_utc_ms_ = now_utc_ms;
      last_leader_contact_utc_ms_ = now_utc_ms;
    }
    return out;
  }

  if (leader_lease_expired(now_utc_ms)) {
    replica_.expire_leader_lease();
    last_leader_contact_utc_ms_ = 0;
    last_leader_lease_expired_utc_ms_ = now_utc_ms;
    leader_lease_expired_count_ += 1;
    last_campaign_started_utc_ms_ = agent_edge_consensus_campaign_last_started_after_lease_expiry(
      now_utc_ms,
      current_campaign_delay_ms(),
      election_started_ ? 1 : 0,
      last_campaign_started_utc_ms_);
  }

  if (lease_expiry_recampaign_delay_active(now_utc_ms)) return out;

  const agent_edge_consensus_campaign_decision_source_t campaign_decision_source =
    agent_edge_consensus_campaign_decision_source(
      cfg_.decision_sha256.data(),
      cfg_.decision_sha256.size(),
      last_known_decision_sha256_.data(),
      last_known_decision_sha256_.size());
  const std::string campaign_decision =
    campaign_decision_source == AGENT_EDGE_CONSENSUS_CAMPAIGN_DECISION_CONFIG
      ? trim_copy(cfg_.decision_sha256)
      : (campaign_decision_source == AGENT_EDGE_CONSENSUS_CAMPAIGN_DECISION_LAST_KNOWN
           ? trim_copy(last_known_decision_sha256_)
           : "");

  const int64_t retry_delay_ms = election_started_ ? current_campaign_delay_ms() : 0;
  const int start_due = agent_edge_consensus_campaign_start_due(
    now_utc_ms,
    started_utc_ms_,
    cfg_.campaign_delay_ms,
    election_started_ ? 1 : 0,
    last_campaign_started_utc_ms_,
    retry_delay_ms);
  if (!agent_edge_consensus_campaign_can_start(
        campaign_decision_source != AGENT_EDGE_CONSENSUS_CAMPAIGN_DECISION_NONE ? 1 : 0,
        has_committed_decision,
        start_due)) return out;
  out.push_back(replica_.start_election(campaign_decision));
  election_started_ = true;
  last_campaign_started_utc_ms_ = now_utc_ms;
  campaign_attempts_ += 1;
  return out;
}

bool EdgeConsensusNodeLoop::handle_frame(
  const EdgeConsensusFrame& frame,
  std::vector<EdgeConsensusFrame>* out_frames,
  std::string* out_error,
  int64_t now_utc_ms
) {
  bool accepted = false;
  const bool ok = replica_.handle_frame(frame, out_frames, out_error, &accepted);
  if (!ok) return false;
  if (accepted) {
    remember_decision(frame.decision_sha256);
    observe_leader_activity(frame, now_utc_ms);
  }
  if (out_frames) {
    for (const auto& generated : *out_frames) {
      if (agent_edge_consensus_leader_activity_can_observe(
            generated.kind.data(),
            generated.kind.size(),
            generated.leader_node_id.data(),
            generated.leader_node_id.size(),
            now_utc_ms)) {
        last_leader_contact_utc_ms_ = now_utc_ms;
        last_leader_heartbeat_sent_utc_ms_ = now_utc_ms;
      }
      if (generated.kind == AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT) {
        remember_decision(generated.decision_sha256);
      }
    }
  }
  return true;
}

std::vector<std::string> EdgeConsensusNodeLoop::target_node_ids_for_frame(const EdgeConsensusFrame& frame) const {
  switch (agent_edge_consensus_frame_route(
    frame.kind.data(),
    frame.kind.size(),
    consensus_member_node_id_is_valid(frame.candidate_node_id) ? 1 : 0,
    consensus_node_id_matches(frame.candidate_node_id, cfg_.self.node_id) ? 1 : 0)) {
    case AGENT_EDGE_CONSENSUS_FRAME_ROUTE_CANDIDATE:
      return {frame.candidate_node_id};
    case AGENT_EDGE_CONSENSUS_FRAME_ROUTE_PEERS:
      return cfg_.peer_node_ids;
    case AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP:
      break;
  }
  return {};
}

Json::Value EdgeConsensusNodeLoop::status_to_json() const {
  Json::Value out(Json::objectValue);
  out["self"] = edge_consensus_identity_to_json(cfg_.self);
  out["cluster_size"] = Json::UInt64(cfg_.cluster_size);
  out["campaign_delay_ms"] = (Json::Int64)cfg_.campaign_delay_ms;
  out["campaign_retry_ms"] = (Json::Int64)cfg_.campaign_retry_ms;
  out["campaign_retry_max_ms"] = (Json::Int64)cfg_.campaign_retry_max_ms;
  out["campaign_retry_backoff_factor"] = (Json::Int64)cfg_.campaign_retry_backoff_factor;
  out["leader_heartbeat_ms"] = (Json::Int64)cfg_.leader_heartbeat_ms;
  out["leader_lease_ms"] = (Json::Int64)cfg_.leader_lease_ms;
  out["lease_expiry_recampaign_delay_ms"] = (Json::Int64)cfg_.lease_expiry_recampaign_delay_ms;
  if (!cfg_.decision_sha256.empty()) out["decision_sha256"] = cfg_.decision_sha256;
  if (!last_known_decision_sha256_.empty()) out["last_known_decision_sha256"] = last_known_decision_sha256_;
  out["election_started"] = election_started_;
  out["campaign_attempts"] = Json::UInt64(campaign_attempts_);
  out["current_campaign_delay_ms"] = (Json::Int64)current_campaign_delay_ms();
  out["leader_lease_expired_count"] = Json::UInt64(leader_lease_expired_count_);
  if (last_leader_contact_utc_ms_ > 0) {
    out["last_leader_contact_utc_ms"] = (Json::Int64)last_leader_contact_utc_ms_;
    if (cfg_.leader_lease_ms > 0 && !replica_.leader_node_id().empty() && !replica_.leader_is_self()) {
      out["leader_lease_deadline_utc_ms"] = (Json::Int64)(last_leader_contact_utc_ms_ + cfg_.leader_lease_ms);
    }
  }
  if (last_leader_lease_expired_utc_ms_ > 0) {
    out["last_leader_lease_expired_utc_ms"] = (Json::Int64)last_leader_lease_expired_utc_ms_;
    if (cfg_.lease_expiry_recampaign_delay_ms > 0 && replica_.leader_node_id().empty()) {
      out["lease_expiry_recampaign_ready_utc_ms"] =
        (Json::Int64)(last_leader_lease_expired_utc_ms_ + cfg_.lease_expiry_recampaign_delay_ms);
    }
  }
  if (last_leader_heartbeat_sent_utc_ms_ > 0) {
    out["last_leader_heartbeat_sent_utc_ms"] = (Json::Int64)last_leader_heartbeat_sent_utc_ms_;
    const std::string committed_decision_sha256 = replica_.committed_decision_sha256();
    if (cfg_.leader_heartbeat_ms > 0 && replica_.leader_is_self() &&
        agent_edge_consensus_decision_sha256_is_set(
          committed_decision_sha256.data(),
          committed_decision_sha256.size())) {
      out["next_leader_heartbeat_utc_ms"] = (Json::Int64)(last_leader_heartbeat_sent_utc_ms_ + cfg_.leader_heartbeat_ms);
    }
  }
  if (last_campaign_started_utc_ms_ > 0) {
    out["last_campaign_started_utc_ms"] = (Json::Int64)last_campaign_started_utc_ms_;
    const std::string committed_decision_sha256 = replica_.committed_decision_sha256();
    if (current_campaign_delay_ms() > 0 &&
        !agent_edge_consensus_decision_sha256_is_set(
          committed_decision_sha256.data(),
          committed_decision_sha256.size())) {
      out["next_campaign_utc_ms"] = (Json::Int64)(last_campaign_started_utc_ms_ + current_campaign_delay_ms());
    }
  }
  Json::Value peers(Json::arrayValue);
  for (const auto& peer : cfg_.peer_node_ids) peers.append(peer);
  out["peer_node_ids"] = peers;
  Json::Value members(Json::arrayValue);
  for (const auto& member : cfg_.member_node_ids) members.append(member);
  out["member_node_ids"] = members;
  out["replica"] = replica_.status_to_json();
  return out;
}

Json::Value EdgeConsensusReplica::status_to_json() const {
  Json::Value out(Json::objectValue);
  out["self"] = edge_consensus_identity_to_json(self_);
  Json::Value members(Json::arrayValue);
  for (const auto& member : member_node_ids_) members.append(member);
  out["member_node_ids"] = members;
  out["current_term"] = Json::UInt64(current_term_);
  if (!voted_for_node_id_.empty()) out["voted_for_node_id"] = voted_for_node_id_;
  if (!leader_node_id_.empty()) out["leader_node_id"] = leader_node_id_;
  if (!campaign_decision_sha256_.empty()) out["campaign_decision_sha256"] = campaign_decision_sha256_;
  if (!committed_decision_sha256_.empty()) out["committed_decision_sha256"] = committed_decision_sha256_;
  Json::Value grants(Json::arrayValue);
  for (const auto& kv : grant_witnesses_by_node_id_) grants.append(edge_consensus_identity_to_json(kv.second));
  if (!grants.empty()) out["grant_witnesses"] = grants;
  Json::Value committed(Json::arrayValue);
  for (const auto& witness : committed_vote_witnesses_) committed.append(edge_consensus_identity_to_json(witness));
  if (!committed.empty()) out["committed_vote_witnesses"] = committed;
  out["quorum"] = Json::UInt64(agent_edge_consensus_quorum_for_cluster_size(cluster_size_));
  return out;
}

Json::Value edge_consensus_epochs_to_json(const EdgeConsensusEpochs& epochs) {
  Json::Value out(Json::objectValue);
  out["trust_roots_epoch"] = Json::UInt64(epochs.trust_roots_epoch);
  out["revocations_epoch"] = Json::UInt64(epochs.revocations_epoch);
  out["cert_roots_epoch"] = Json::UInt64(epochs.cert_roots_epoch);
  return out;
}

Json::Value edge_consensus_identity_to_json(const EdgeConsensusIdentity& identity) {
  Json::Value out(Json::objectValue);
  out["cluster_id"] = identity.cluster_id;
  out["node_id"] = identity.node_id;
  if (!identity.manifest_sha256.empty()) out["manifest_sha256"] = identity.manifest_sha256;
  out["membership_epoch"] = Json::UInt64(identity.membership_epoch);
  out["trust_epochs"] = edge_consensus_epochs_to_json(identity.trust_epochs);
  return out;
}

Json::Value edge_consensus_frame_to_json(const EdgeConsensusFrame& frame) {
  Json::Value out(Json::objectValue);
  out["schema"] = frame.schema;
  out["frame_id"] = frame.frame_id;
  out["kind"] = frame.kind;
  out["term"] = Json::UInt64(frame.term);
  if (!frame.decision_sha256.empty()) out["decision_sha256"] = frame.decision_sha256;
  if (!frame.candidate_node_id.empty()) out["candidate_node_id"] = frame.candidate_node_id;
  if (!frame.leader_node_id.empty()) out["leader_node_id"] = frame.leader_node_id;
  if (frame.kind == AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT) out["granted"] = frame.granted;
  out["from"] = edge_consensus_identity_to_json(frame.from);
  if (!frame.vote_witnesses.empty()) {
    Json::Value arr(Json::arrayValue);
    for (const auto& witness : frame.vote_witnesses) arr.append(edge_consensus_identity_to_json(witness));
    out["vote_witnesses"] = arr;
  }
  return out;
}

bool edge_consensus_epochs_from_json(const Json::Value& root, EdgeConsensusEpochs* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  *out = EdgeConsensusEpochs{};
  if (!root.isObject()) {
    if (out_error) *out_error = "trust_epochs must be an object";
    return false;
  }
  if (!json_get_u64_nonneg(root, "trust_roots_epoch", &out->trust_roots_epoch) ||
      !json_get_u64_nonneg(root, "revocations_epoch", &out->revocations_epoch) ||
      !json_get_u64_nonneg(root, "cert_roots_epoch", &out->cert_roots_epoch)) {
    if (out_error) *out_error = "trust_epochs fields must be uint64";
    return false;
  }
  return true;
}

bool edge_consensus_identity_from_json(const Json::Value& root, EdgeConsensusIdentity* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  *out = EdgeConsensusIdentity{};
  if (!root.isObject()) {
    if (out_error) *out_error = "identity must be an object";
    return false;
  }
  if (!parse_nonempty_string(root, "cluster_id", &out->cluster_id, out_error) ||
      !parse_nonempty_string(root, "node_id", &out->node_id, out_error) ||
      !parse_optional_sha256(root, "manifest_sha256", &out->manifest_sha256, out_error)) {
    return false;
  }
  if (root.isMember("membership_epoch") && !root["membership_epoch"].isNull()) {
    if (!json_get_u64_nonneg(root, "membership_epoch", &out->membership_epoch)) {
      if (out_error) *out_error = "membership_epoch must be uint64";
      return false;
    }
  }
  if (!root.isMember("trust_epochs") || !edge_consensus_epochs_from_json(root["trust_epochs"], &out->trust_epochs, out_error)) {
    if (out_error && out_error->empty()) *out_error = "trust_epochs missing or invalid";
    return false;
  }
  return parse_identity_like(*out, out_error);
}

bool edge_consensus_frame_from_json(const Json::Value& root, EdgeConsensusFrame* out, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!out) return false;
  *out = EdgeConsensusFrame{};
  if (!root.isObject()) {
    if (out_error) *out_error = "frame must be an object";
    return false;
  }
  if (!parse_nonempty_string(root, "schema", &out->schema, out_error) ||
      !parse_nonempty_string(root, "frame_id", &out->frame_id, out_error) ||
      !parse_nonempty_string(root, "kind", &out->kind, out_error)) {
    return false;
  }
  if (!json_get_u64_nonneg(root, "term", &out->term)) {
    if (out_error) *out_error = "term must be uint64";
    return false;
  }
  if (!parse_optional_sha256(root, "decision_sha256", &out->decision_sha256, out_error)) return false;
  if (root.isMember("candidate_node_id") && !root["candidate_node_id"].isNull()) {
    if (!parse_nonempty_string(root, "candidate_node_id", &out->candidate_node_id, out_error)) return false;
  }
  if (root.isMember("leader_node_id") && !root["leader_node_id"].isNull()) {
    if (!parse_nonempty_string(root, "leader_node_id", &out->leader_node_id, out_error)) return false;
  }
  if (root.isMember("granted") && !root["granted"].isNull()) {
    if (!root["granted"].isBool()) {
      if (out_error) *out_error = "granted must be bool";
      return false;
    }
    out->granted = root["granted"].asBool();
  }
  if (!root.isMember("from") || !edge_consensus_identity_from_json(root["from"], &out->from, out_error)) {
    if (out_error && out_error->empty()) *out_error = "from missing or invalid";
    return false;
  }
  if (root.isMember("vote_witnesses") && !root["vote_witnesses"].isNull()) {
    if (!root["vote_witnesses"].isArray()) {
      if (out_error) *out_error = "vote_witnesses must be an array";
      return false;
    }
    for (Json::ArrayIndex i = 0; i < root["vote_witnesses"].size(); i++) {
      EdgeConsensusIdentity witness;
      std::string werr;
      if (!edge_consensus_identity_from_json(root["vote_witnesses"][i], &witness, &werr)) {
        if (out_error) *out_error = werr;
        return false;
      }
      out->vote_witnesses.push_back(witness);
    }
  }
  return frame_is_valid(*out, out_error);
}

bool edge_consensus_membership_policy_update_from_json(
  const Json::Value& root,
  EdgeConsensusMembershipPolicyUpdate* out,
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (!out) return false;
  *out = EdgeConsensusMembershipPolicyUpdate{};
  if (!root.isObject()) {
    if (out_error) *out_error = "membership bundle must be an object";
    return false;
  }
  if (!parse_nonempty_string(root, "schema", &out->schema, out_error) ||
      !parse_nonempty_string(root, "cluster_id", &out->cluster_id, out_error)) {
    return false;
  }
  const agent_edge_consensus_membership_policy_header_validation_t header_validation =
    agent_edge_consensus_membership_policy_header_validate(
      out->schema.data(),
      out->schema.size(),
      out->cluster_id.data(),
      out->cluster_id.size());
  if (header_validation != AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_HEADER_OK) {
    if (out_error) *out_error = consensus_membership_policy_header_validation_error(header_validation);
    return false;
  }
  if (!json_get_u64_nonneg(root, "membership_epoch", &out->membership_epoch)) {
    if (out_error) *out_error = "membership_epoch must be uint64";
    return false;
  }
  if (!root.isMember("member_node_ids") || !root["member_node_ids"].isArray()) {
    if (out_error) *out_error = "member_node_ids must be an array";
    return false;
  }
  std::vector<std::string> members;
  for (Json::ArrayIndex i = 0; i < root["member_node_ids"].size(); i++) {
    if (!root["member_node_ids"][i].isString()) {
      if (out_error) *out_error = "member_node_ids must contain strings";
      return false;
    }
    const std::string member = trim_copy(root["member_node_ids"][i].asString());
    if (!consensus_member_node_id_is_valid(member)) {
      if (out_error) *out_error = "member_node_id invalid";
      return false;
    }
    members.push_back(member);
  }
  out->member_node_ids = dedupe_member_vector_without_forcing_self(members);
  if (!agent_edge_consensus_membership_member_set_is_nonempty(out->member_node_ids.size())) {
    if (out_error) *out_error = "member_node_ids empty";
    return false;
  }

  if (!parse_optional_i64_field(root, "campaign_delay_ms", &out->campaign_delay_ms, out_error) ||
      !parse_optional_i64_field(root, "campaign_retry_ms", &out->campaign_retry_ms, out_error) ||
      !parse_optional_i64_field(root, "campaign_retry_max_ms", &out->campaign_retry_max_ms, out_error) ||
      !parse_optional_i64_field(root, "campaign_retry_backoff_factor", &out->campaign_retry_backoff_factor, out_error) ||
      !parse_optional_i64_field(root, "leader_heartbeat_ms", &out->leader_heartbeat_ms, out_error) ||
      !parse_optional_i64_field(root, "leader_lease_ms", &out->leader_lease_ms, out_error) ||
      !parse_optional_i64_field(root, "lease_expiry_recampaign_delay_ms", &out->lease_expiry_recampaign_delay_ms, out_error)) {
    return false;
  }
  agent_edge_consensus_policy_timing_t timing;
  timing.campaign_delay_ms = out->campaign_delay_ms;
  timing.campaign_retry_ms = out->campaign_retry_ms;
  timing.campaign_retry_max_ms = out->campaign_retry_max_ms <= 0
    ? out->campaign_retry_ms
    : out->campaign_retry_max_ms;
  timing.campaign_retry_backoff_factor = out->campaign_retry_backoff_factor;
  timing.leader_heartbeat_ms = out->leader_heartbeat_ms;
  timing.leader_lease_ms = out->leader_lease_ms;
  timing.lease_expiry_recampaign_delay_ms = out->lease_expiry_recampaign_delay_ms;
  timing.stale_runtime_recovery_grace_ms = 0;
  if (agent_edge_consensus_policy_timing_normalize(&timing) != AGENT_OK) {
    if (out_error) *out_error = "membership timing invalid";
    return false;
  }
  out->campaign_delay_ms = timing.campaign_delay_ms;
  out->campaign_retry_ms = timing.campaign_retry_ms;
  out->campaign_retry_max_ms = timing.campaign_retry_max_ms;
  out->campaign_retry_backoff_factor = timing.campaign_retry_backoff_factor;
  out->leader_heartbeat_ms = timing.leader_heartbeat_ms;
  out->leader_lease_ms = timing.leader_lease_ms;
  out->lease_expiry_recampaign_delay_ms = timing.lease_expiry_recampaign_delay_ms;
  return true;
}

}  // namespace agentd
