#include "edge_node_consensus.h"

#include "edge_util.h"
#include "json_util.h"
#include "string_util.h"

#include <algorithm>
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
  if (!edge_sha256_token_is_safe(v)) {
    if (out_error) *out_error = std::string(key) + " must be a sha256 token";
    return false;
  }
  if (out) *out = v;
  return true;
}

static bool parse_identity_like(const EdgeConsensusIdentity& id, std::string* out_error) {
  if (out_error) out_error->clear();
  if (!edge_id_is_safe(id.cluster_id)) {
    if (out_error) *out_error = "cluster_id invalid";
    return false;
  }
  if (!edge_id_is_safe(id.node_id)) {
    if (out_error) *out_error = "node_id invalid";
    return false;
  }
  if (!id.manifest_sha256.empty() && !edge_sha256_token_is_safe(id.manifest_sha256)) {
    if (out_error) *out_error = "manifest_sha256 invalid";
    return false;
  }
  return true;
}

static std::set<std::string> dedupe_member_ids(
  const std::vector<std::string>& raw_ids,
  const std::string& self_node_id
) {
  std::set<std::string> out;
  if (edge_id_is_safe(self_node_id)) out.insert(self_node_id);
  for (const auto& raw : raw_ids) {
    const std::string node_id = trim_copy(raw);
    if (!edge_id_is_safe(node_id)) continue;
    out.insert(node_id);
  }
  return out;
}

static bool frame_is_valid(const EdgeConsensusFrame& frame, std::string* out_error) {
  if (out_error) out_error->clear();
  if (frame.schema != "edge_node_consensus_frame_v1") {
    if (out_error) *out_error = "schema invalid";
    return false;
  }
  if (frame.kind != "vote_request" && frame.kind != "vote_grant" && frame.kind != "leader_commit") {
    if (out_error) *out_error = "kind invalid";
    return false;
  }
  if (!edge_id_is_safe(frame.frame_id)) {
    if (out_error) *out_error = "frame_id invalid";
    return false;
  }
  if (frame.term < 1) {
    if (out_error) *out_error = "term must be >= 1";
    return false;
  }
  if (!frame.decision_sha256.empty() && !edge_sha256_token_is_safe(frame.decision_sha256)) {
    if (out_error) *out_error = "decision_sha256 invalid";
    return false;
  }
  if (!parse_identity_like(frame.from, out_error)) return false;
  if (frame.kind == "vote_request" || frame.kind == "vote_grant") {
    if (!edge_id_is_safe(frame.candidate_node_id)) {
      if (out_error) *out_error = "candidate_node_id invalid";
      return false;
    }
  }
  if (frame.kind == "leader_commit" && !edge_id_is_safe(frame.leader_node_id)) {
    if (out_error) *out_error = "leader_node_id invalid";
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
    if (!edge_id_is_safe(node_id) || node_id == self_node_id) continue;
    if (!seen.insert(node_id).second) continue;
    out.push_back(node_id);
  }
  return out;
}

static int64_t clamp_retry_backoff_factor(int64_t v) {
  return std::max<int64_t>(1, std::min<int64_t>(v, 8));
}

static int64_t clamp_leader_heartbeat_ms(int64_t v) {
  return std::max<int64_t>(0, std::min<int64_t>(v, 120000));
}

static int64_t clamp_leader_lease_ms(int64_t heartbeat_ms, int64_t lease_ms) {
  const int64_t clamped_heartbeat = clamp_leader_heartbeat_ms(heartbeat_ms);
  const int64_t clamped_lease = std::max<int64_t>(0, std::min<int64_t>(lease_ms, 300000));
  return std::max<int64_t>(clamped_heartbeat, clamped_lease);
}

static int64_t clamp_lease_expiry_recampaign_delay_ms(int64_t v) {
  return std::max<int64_t>(0, std::min<int64_t>(v, 300000));
}

static int64_t compute_campaign_retry_delay_ms(
  int64_t campaign_retry_ms,
  int64_t campaign_retry_max_ms,
  int64_t campaign_retry_backoff_factor,
  uint64_t campaign_attempts
) {
  if (campaign_retry_ms <= 0 || campaign_attempts < 1) return 0;
  const int64_t factor = clamp_retry_backoff_factor(campaign_retry_backoff_factor);
  int64_t delay = campaign_retry_ms;
  for (uint64_t i = 1; i < campaign_attempts; i++) {
    if (delay > INT64_MAX / factor) {
      delay = INT64_MAX;
      break;
    }
    delay *= factor;
  }
  const int64_t cap = campaign_retry_max_ms > 0 ? campaign_retry_max_ms : campaign_retry_ms;
  if (cap > 0) delay = std::min<int64_t>(delay, cap);
  return std::max<int64_t>(0, delay);
}

}  // namespace

EdgeConsensusReplica::EdgeConsensusReplica(const EdgeConsensusIdentity& self, size_t cluster_size)
    : self_(self), cluster_size_(cluster_size < 1 ? 1 : cluster_size) {
  member_node_ids_.insert(self_.node_id);
}

EdgeConsensusNodeLoop::EdgeConsensusNodeLoop(const EdgeConsensusNodeLoopConfig& cfg)
    : cfg_(cfg), replica_(cfg.self, cfg.cluster_size == 0 ? cfg.peer_node_ids.size() + 1 : cfg.cluster_size) {
  cfg_.peer_node_ids = dedupe_loop_targets(cfg.peer_node_ids, cfg.self.node_id);
  if (cfg_.member_node_ids.empty()) {
    cfg_.member_node_ids = cfg_.peer_node_ids;
    cfg_.member_node_ids.push_back(cfg_.self.node_id);
  }
  const std::set<std::string> member_ids = dedupe_member_ids(cfg_.member_node_ids, cfg_.self.node_id);
  cfg_.member_node_ids.assign(member_ids.begin(), member_ids.end());
  if (cfg_.cluster_size == 0) cfg_.cluster_size = cfg_.member_node_ids.size();
  if (cfg_.cluster_size < 1) cfg_.cluster_size = 1;
  if (cfg_.campaign_delay_ms < 0) cfg_.campaign_delay_ms = 0;
  if (cfg_.campaign_retry_ms < 0) cfg_.campaign_retry_ms = 0;
  if (cfg_.campaign_retry_max_ms <= 0) cfg_.campaign_retry_max_ms = cfg_.campaign_retry_ms;
  cfg_.campaign_retry_max_ms = std::max<int64_t>(cfg_.campaign_retry_ms, cfg_.campaign_retry_max_ms);
  cfg_.campaign_retry_backoff_factor = clamp_retry_backoff_factor(cfg_.campaign_retry_backoff_factor);
  cfg_.leader_heartbeat_ms = clamp_leader_heartbeat_ms(cfg_.leader_heartbeat_ms);
  cfg_.leader_lease_ms = clamp_leader_lease_ms(cfg_.leader_heartbeat_ms, cfg_.leader_lease_ms);
  cfg_.lease_expiry_recampaign_delay_ms =
    clamp_lease_expiry_recampaign_delay_ms(cfg_.lease_expiry_recampaign_delay_ms);
  remember_decision(cfg_.decision_sha256);
  replica_.set_membership(cfg_.self.membership_epoch, cfg_.member_node_ids);
}

void EdgeConsensusReplica::set_trust_epochs(const EdgeConsensusEpochs& epochs) {
  self_.trust_epochs = epochs;
}

void EdgeConsensusReplica::set_membership(uint64_t membership_epoch, const std::vector<std::string>& member_node_ids) {
  self_.membership_epoch = membership_epoch;
  member_node_ids_ = dedupe_member_ids(member_node_ids, self_.node_id);
  cluster_size_ = std::max<size_t>(1, member_node_ids_.empty() ? 1 : member_node_ids_.size());
}

std::string EdgeConsensusReplica::next_frame_id(const char* kind) {
  ++frame_seq_;
  return self_.node_id + ":" + std::string(kind ? kind : "frame") + ":" + std::to_string(frame_seq_);
}

void EdgeConsensusReplica::maybe_reset_for_new_term(uint64_t term) {
  if (term <= current_term_) return;
  current_term_ = term;
  voted_for_node_id_.clear();
  leader_node_id_.clear();
  campaign_decision_sha256_.clear();
  committed_decision_sha256_.clear();
  committed_vote_witnesses_.clear();
  grant_witnesses_by_node_id_.clear();
}

bool EdgeConsensusReplica::trust_epochs_match(const EdgeConsensusEpochs& other) const {
  return self_.trust_epochs.trust_roots_epoch == other.trust_roots_epoch &&
         self_.trust_epochs.revocations_epoch == other.revocations_epoch &&
         self_.trust_epochs.cert_roots_epoch == other.cert_roots_epoch;
}

bool EdgeConsensusReplica::node_is_member(const std::string& node_id) const {
  return edge_id_is_safe(node_id) && member_node_ids_.find(node_id) != member_node_ids_.end();
}

bool EdgeConsensusReplica::membership_matches(const EdgeConsensusIdentity& other) const {
  return self_.membership_epoch == other.membership_epoch && node_is_member(other.node_id);
}

bool EdgeConsensusReplica::has_quorum() const {
  const size_t votes = 1 + grant_witnesses_by_node_id_.size();
  return votes >= ((cluster_size_ / 2) + 1);
}

EdgeConsensusFrame EdgeConsensusReplica::make_vote_grant_frame(
  const std::string& candidate_node_id,
  const std::string& decision_sha256
) const {
  EdgeConsensusFrame out;
  out.frame_id = self_.node_id + ":vote_grant:" + std::to_string(current_term_) + ":" + candidate_node_id;
  out.kind = "vote_grant";
  out.term = current_term_;
  out.decision_sha256 = decision_sha256;
  out.candidate_node_id = candidate_node_id;
  out.granted = true;
  out.from = self_;
  return out;
}

EdgeConsensusFrame EdgeConsensusReplica::make_leader_commit_frame() const {
  EdgeConsensusFrame out;
  out.frame_id = self_.node_id + ":leader_commit:" + std::to_string(current_term_) + ":" + leader_node_id_;
  out.kind = "leader_commit";
  out.term = current_term_;
  out.decision_sha256 = committed_decision_sha256_;
  out.leader_node_id = leader_node_id_;
  out.from = self_;
  out.vote_witnesses = committed_vote_witnesses_;
  return out;
}

EdgeConsensusFrame EdgeConsensusReplica::current_leader_commit_frame() const {
  return make_leader_commit_frame();
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
  current_term_ += 1;
  voted_for_node_id_ = self_.node_id;

  EdgeConsensusFrame out;
  out.frame_id = next_frame_id("vote_request");
  out.kind = "vote_request";
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
  std::string* out_error
) {
  if (out_error) out_error->clear();
  if (out_frames) out_frames->clear();
  std::string verr;
  if (!frame_is_valid(frame, &verr)) {
    if (out_error) *out_error = verr;
    return false;
  }
  if (frame.from.cluster_id != self_.cluster_id) {
    if (out_error) *out_error = "cluster_id mismatch";
    return false;
  }
  if (!membership_matches(frame.from)) return true;
  auto seen_it = seen_frame_term_by_id_.find(frame.frame_id);
  if (seen_it != seen_frame_term_by_id_.end() && seen_it->second == frame.term) return true;
  seen_frame_term_by_id_[frame.frame_id] = frame.term;

  if (frame.kind == "vote_request") {
    if (!node_is_member(frame.candidate_node_id)) return true;
    if (frame.term < current_term_) return true;
    maybe_reset_for_new_term(frame.term);
    if (!trust_epochs_match(frame.from.trust_epochs)) return true;
    if (!voted_for_node_id_.empty() && voted_for_node_id_ != frame.candidate_node_id) return true;
    voted_for_node_id_ = frame.candidate_node_id;
    if (out_frames) out_frames->push_back(make_vote_grant_frame(frame.candidate_node_id, frame.decision_sha256));
    return true;
  }

  if (frame.kind == "vote_grant") {
    if (!node_is_member(frame.candidate_node_id)) return true;
    if (frame.term < current_term_) return true;
    maybe_reset_for_new_term(frame.term);
    if (frame.candidate_node_id != self_.node_id) return true;
    if (campaign_decision_sha256_.empty() || frame.decision_sha256 != campaign_decision_sha256_) return true;
    if (!frame.granted || !trust_epochs_match(frame.from.trust_epochs)) return true;
    grant_witnesses_by_node_id_[frame.from.node_id] = frame.from;
    if (!leader_node_id_.empty() || !has_quorum()) return true;
    leader_node_id_ = self_.node_id;
    committed_decision_sha256_ = campaign_decision_sha256_;
    committed_vote_witnesses_.clear();
    committed_vote_witnesses_.push_back(self_);
    for (const auto& kv : grant_witnesses_by_node_id_) committed_vote_witnesses_.push_back(kv.second);
    if (out_frames) out_frames->push_back(make_leader_commit_frame());
    return true;
  }

  if (frame.term < current_term_) return true;
  if (!node_is_member(frame.leader_node_id)) return true;
  if (!trust_epochs_match(frame.from.trust_epochs)) return true;
  maybe_reset_for_new_term(frame.term);
  leader_node_id_ = frame.leader_node_id;
  voted_for_node_id_ = frame.leader_node_id;
  campaign_decision_sha256_.clear();
  committed_decision_sha256_ = frame.decision_sha256;
  committed_vote_witnesses_ = frame.vote_witnesses;
  return true;
}

int64_t EdgeConsensusNodeLoop::current_campaign_delay_ms() const {
  return compute_campaign_retry_delay_ms(
    cfg_.campaign_retry_ms, cfg_.campaign_retry_max_ms, cfg_.campaign_retry_backoff_factor, campaign_attempts_);
}

bool EdgeConsensusNodeLoop::leader_lease_expired(int64_t now_utc_ms) const {
  if (cfg_.leader_lease_ms <= 0 || now_utc_ms <= 0) return false;
  if (replica_.leader_node_id().empty() || replica_.leader_is_self()) return false;
  if (last_leader_contact_utc_ms_ <= 0) return false;
  return now_utc_ms - last_leader_contact_utc_ms_ >= cfg_.leader_lease_ms;
}

bool EdgeConsensusNodeLoop::lease_expiry_recampaign_delay_active(int64_t now_utc_ms) const {
  if (cfg_.lease_expiry_recampaign_delay_ms <= 0 || now_utc_ms <= 0) return false;
  if (last_leader_lease_expired_utc_ms_ <= 0) return false;
  return now_utc_ms - last_leader_lease_expired_utc_ms_ < cfg_.lease_expiry_recampaign_delay_ms;
}

void EdgeConsensusNodeLoop::observe_leader_activity(const EdgeConsensusFrame& frame, int64_t now_utc_ms) {
  if (frame.kind != "leader_commit" || now_utc_ms <= 0) return;
  if (frame.leader_node_id.empty()) return;
  last_leader_contact_utc_ms_ = now_utc_ms;
  remember_decision(frame.decision_sha256);
}

std::vector<EdgeConsensusFrame> EdgeConsensusNodeLoop::tick(int64_t now_utc_ms) {
  std::vector<EdgeConsensusFrame> out;
  if (started_utc_ms_ == 0) started_utc_ms_ = now_utc_ms;
  remember_decision(replica_.committed_decision_sha256());

  if (!trim_copy(replica_.committed_decision_sha256()).empty() && replica_.leader_is_self()) {
    if (cfg_.leader_heartbeat_ms > 0 &&
        (last_leader_heartbeat_sent_utc_ms_ <= 0 || now_utc_ms - last_leader_heartbeat_sent_utc_ms_ >= cfg_.leader_heartbeat_ms)) {
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
    if (election_started_ && last_campaign_started_utc_ms_ > 0) {
      last_campaign_started_utc_ms_ = now_utc_ms - current_campaign_delay_ms();
    }
  }

  if (lease_expiry_recampaign_delay_active(now_utc_ms)) return out;

  const std::string campaign_decision =
    trim_copy(cfg_.decision_sha256).empty() ? trim_copy(last_known_decision_sha256_) : trim_copy(cfg_.decision_sha256);
  if (campaign_decision.empty()) return out;
  if (!trim_copy(replica_.committed_decision_sha256()).empty()) return out;

  if (!election_started_) {
    if (now_utc_ms - started_utc_ms_ < cfg_.campaign_delay_ms) return out;
  } else {
    const int64_t retry_delay_ms = current_campaign_delay_ms();
    if (retry_delay_ms <= 0) return out;
    if (last_campaign_started_utc_ms_ > 0 && now_utc_ms - last_campaign_started_utc_ms_ < retry_delay_ms) {
      return out;
    }
  }
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
  const bool ok = replica_.handle_frame(frame, out_frames, out_error);
  if (!ok) return false;
  remember_decision(frame.decision_sha256);
  observe_leader_activity(frame, now_utc_ms);
  if (out_frames) {
    for (const auto& generated : *out_frames) {
      if (generated.kind == "leader_commit") {
        last_leader_contact_utc_ms_ = now_utc_ms;
        last_leader_heartbeat_sent_utc_ms_ = now_utc_ms;
        remember_decision(generated.decision_sha256);
      }
    }
  }
  return true;
}

std::vector<std::string> EdgeConsensusNodeLoop::target_node_ids_for_frame(const EdgeConsensusFrame& frame) const {
  if (frame.kind == "vote_grant") {
    if (edge_id_is_safe(frame.candidate_node_id) && frame.candidate_node_id != cfg_.self.node_id) {
      return {frame.candidate_node_id};
    }
    return {};
  }
  if (frame.kind == "vote_request" || frame.kind == "leader_commit") return cfg_.peer_node_ids;
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
    if (cfg_.leader_heartbeat_ms > 0 && replica_.leader_is_self() && !trim_copy(replica_.committed_decision_sha256()).empty()) {
      out["next_leader_heartbeat_utc_ms"] = (Json::Int64)(last_leader_heartbeat_sent_utc_ms_ + cfg_.leader_heartbeat_ms);
    }
  }
  if (last_campaign_started_utc_ms_ > 0) {
    out["last_campaign_started_utc_ms"] = (Json::Int64)last_campaign_started_utc_ms_;
    if (current_campaign_delay_ms() > 0 && trim_copy(replica_.committed_decision_sha256()).empty()) {
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
  out["quorum"] = Json::UInt64((cluster_size_ / 2) + 1);
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
  if (frame.kind == "vote_grant") out["granted"] = frame.granted;
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

}  // namespace agentd
