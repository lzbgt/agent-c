#include "agent/edge_interop.h"

#include <array>
#include <cassert>
#include <cstring>

namespace {

constexpr size_t kMaxMembers = 5;
constexpr size_t kMaxSeenFrames = 16;
constexpr size_t kMaxWitnesses = 5;
constexpr size_t kTokenCap = AGENT_UM_BMP_MAX_ID_LEN + 1;

constexpr const char* kClusterId = "firmware-consensus";
constexpr const char* kDecisionSha =
  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kManifestA =
  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kManifestB =
  "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr const char* kManifestC =
  "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr const char* kManifestD =
  "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
constexpr const char* kManifestE =
  "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

struct FirmwareIdentity {
  const char* cluster_id = "";
  const char* node_id = "";
  const char* manifest_sha256 = "";
  uint64_t membership_epoch = 0;
  uint64_t trust_roots_epoch = 0;
  uint64_t revocations_epoch = 0;
  uint64_t cert_roots_epoch = 0;
};

struct FirmwareConsensusFrame {
  char frame_id[kTokenCap] = {};
  const char* kind = "";
  uint64_t term = 0;
  const char* decision_sha256 = "";
  const char* candidate_node_id = "";
  const char* leader_node_id = "";
  bool granted = false;
  FirmwareIdentity from;
  std::array<FirmwareIdentity, kMaxWitnesses> vote_witnesses = {};
  size_t vote_witness_count = 0;
};

struct SeenFrame {
  char frame_id[kTokenCap] = {};
  uint64_t term = 0;
  bool used = false;
};

struct FirmwareNode {
  FirmwareIdentity self;
  std::array<const char*, kMaxMembers> member_node_ids = {};
  size_t member_count = 0;
  size_t cluster_size = 1;
  uint64_t current_term = 0;
  uint64_t frame_sequence = 0;
  char voted_for_node_id[kTokenCap] = {};
  char leader_node_id[kTokenCap] = {};
  char campaign_decision_sha256[80] = {};
  char committed_decision_sha256[80] = {};
  std::array<SeenFrame, kMaxSeenFrames> seen_frames = {};
  std::array<FirmwareIdentity, kMaxWitnesses> grant_witnesses = {};
  size_t grant_witness_count = 0;
  std::array<FirmwareIdentity, kMaxWitnesses> committed_witnesses = {};
  size_t committed_witness_count = 0;
};

struct FirmwareMembershipBundle {
  const char* schema = AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1;
  const char* cluster_id = kClusterId;
  uint64_t previous_epoch = 0;
  uint64_t membership_epoch = 0;
  std::array<const char*, kMaxMembers> member_node_ids = {};
  size_t member_count = 0;
};

static size_t token_len(const char* token) {
  return token ? std::strlen(token) : 0;
}

static bool token_eq(const char* lhs, const char* rhs) {
  return std::strcmp(lhs ? lhs : "", rhs ? rhs : "") == 0;
}

static void store_token(char* dst, size_t dst_cap, const char* src) {
  assert(dst);
  assert(dst_cap > 0);
  const char* in = src ? src : "";
  const size_t n = token_len(in);
  assert(n < dst_cap);
  std::memcpy(dst, in, n);
  dst[n] = '\0';
}

static FirmwareIdentity make_identity(const char* node_id, const char* manifest_sha256, uint64_t membership_epoch) {
  FirmwareIdentity out;
  out.cluster_id = kClusterId;
  out.node_id = node_id;
  out.manifest_sha256 = manifest_sha256;
  out.membership_epoch = membership_epoch;
  out.trust_roots_epoch = 7;
  out.revocations_epoch = 3;
  out.cert_roots_epoch = 5;
  assert(agent_edge_consensus_identity_validate(
           out.cluster_id,
           token_len(out.cluster_id),
           out.node_id,
           token_len(out.node_id),
           out.manifest_sha256,
           token_len(out.manifest_sha256)) == AGENT_EDGE_CONSENSUS_IDENTITY_OK);
  return out;
}

static std::array<const char*, kMaxMembers> default_members() {
  return {"node-a", "node-b", "node-c", "node-d", "node-e"};
}

static const char* manifest_for_node_id(const char* node_id) {
  if (token_eq(node_id, "node-a")) return kManifestA;
  if (token_eq(node_id, "node-b")) return kManifestB;
  if (token_eq(node_id, "node-c")) return kManifestC;
  if (token_eq(node_id, "node-d")) return kManifestD;
  if (token_eq(node_id, "node-e")) return kManifestE;
  return "";
}

static FirmwareNode make_node(const char* node_id) {
  FirmwareNode out;
  out.self = make_identity(node_id, manifest_for_node_id(node_id), 1);
  out.member_node_ids = default_members();
  out.member_count = out.member_node_ids.size();
  out.cluster_size = agent_edge_consensus_cluster_size_from_member_count(out.member_count);
  return out;
}

static bool node_id_matches(const char* lhs, const char* rhs) {
  return agent_edge_consensus_node_id_matches(lhs, token_len(lhs), rhs, token_len(rhs)) != 0;
}

static bool node_is_member(const FirmwareNode& node, const char* node_id) {
  if (!agent_edge_consensus_member_node_id_is_valid(node_id, token_len(node_id))) return false;
  for (size_t i = 0; i < node.member_count; ++i) {
    if (node_id_matches(node.member_node_ids[i], node_id)) return true;
  }
  return false;
}

static bool cluster_matches(const FirmwareNode& node, const FirmwareIdentity& identity) {
  return agent_edge_consensus_cluster_id_matches(
           node.self.cluster_id,
           token_len(node.self.cluster_id),
           identity.cluster_id,
           token_len(identity.cluster_id)) != 0;
}

static bool membership_matches(const FirmwareNode& node, const FirmwareIdentity& identity) {
  return agent_edge_consensus_identity_membership_matches(
           node.self.membership_epoch,
           identity.membership_epoch,
           node_is_member(node, identity.node_id) ? 1 : 0) != 0;
}

static bool trust_epochs_match(const FirmwareNode& node, const FirmwareIdentity& identity) {
  return agent_edge_consensus_trust_epochs_match(
           node.self.trust_roots_epoch,
           node.self.revocations_epoch,
           node.self.cert_roots_epoch,
           identity.trust_roots_epoch,
           identity.revocations_epoch,
           identity.cert_roots_epoch) != 0;
}

static bool decision_matches(const char* lhs, const char* rhs) {
  return agent_edge_consensus_decision_sha256_matches(lhs, token_len(lhs), rhs, token_len(rhs)) != 0;
}

static bool frame_is_valid(const FirmwareConsensusFrame& frame) {
  return agent_edge_consensus_frame_validate(
           AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1,
           token_len(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1),
           frame.kind,
           token_len(frame.kind),
           frame.frame_id,
           token_len(frame.frame_id),
           frame.term,
           frame.decision_sha256,
           token_len(frame.decision_sha256),
           frame.from.cluster_id,
           token_len(frame.from.cluster_id),
           frame.from.node_id,
           token_len(frame.from.node_id),
           frame.from.manifest_sha256,
           token_len(frame.from.manifest_sha256),
           frame.candidate_node_id,
           token_len(frame.candidate_node_id),
           frame.leader_node_id,
           token_len(frame.leader_node_id)) == AGENT_EDGE_CONSENSUS_FRAME_OK;
}

static void reset_consensus_state(FirmwareNode* node) {
  assert(node);
  node->current_term = 0;
  node->voted_for_node_id[0] = '\0';
  node->leader_node_id[0] = '\0';
  node->campaign_decision_sha256[0] = '\0';
  node->committed_decision_sha256[0] = '\0';
  node->grant_witness_count = 0;
  node->committed_witness_count = 0;
  for (SeenFrame& seen : node->seen_frames) {
    seen.used = false;
    seen.frame_id[0] = '\0';
    seen.term = 0;
  }
}

static void reset_for_new_term(FirmwareNode* node, uint64_t term) {
  assert(node);
  if (!agent_edge_consensus_incoming_term_advances(node->current_term, term)) return;
  node->current_term = term;
  node->voted_for_node_id[0] = '\0';
  node->leader_node_id[0] = '\0';
  node->campaign_decision_sha256[0] = '\0';
  node->committed_decision_sha256[0] = '\0';
  node->grant_witness_count = 0;
  node->committed_witness_count = 0;
}

static bool seen_frame_should_drop(FirmwareNode* node, const FirmwareConsensusFrame& frame) {
  assert(node);
  for (SeenFrame& seen : node->seen_frames) {
    if (!seen.used || !token_eq(seen.frame_id, frame.frame_id)) continue;
    if (agent_edge_consensus_seen_frame_should_drop(1, seen.term, frame.term)) return true;
    seen.term = frame.term;
    return false;
  }
  for (SeenFrame& seen : node->seen_frames) {
    if (seen.used) continue;
    seen.used = true;
    seen.term = frame.term;
    store_token(seen.frame_id, sizeof(seen.frame_id), frame.frame_id);
    return false;
  }
  assert(false && "firmware test seen-frame window exhausted");
  return true;
}

static void format_frame_id(
  const char* node_id,
  const char* kind,
  uint64_t number,
  const char* suffix,
  char* out,
  size_t out_cap
) {
  size_t out_len = 0;
  assert(agent_edge_consensus_frame_id_format(
           node_id,
           token_len(node_id),
           kind,
           token_len(kind),
           number,
           suffix,
           token_len(suffix),
           out,
           out_cap,
           &out_len) == AGENT_OK);
  assert(out_len == token_len(out));
}

static FirmwareConsensusFrame make_vote_grant_frame(
  const FirmwareNode& node,
  const char* candidate_node_id,
  const char* decision_sha256
) {
  FirmwareConsensusFrame out;
  format_frame_id(
    node.self.node_id,
    AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT,
    node.current_term,
    candidate_node_id,
    out.frame_id,
    sizeof(out.frame_id));
  out.kind = AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT;
  out.term = node.current_term;
  out.decision_sha256 = decision_sha256;
  out.candidate_node_id = candidate_node_id;
  out.granted = true;
  out.from = node.self;
  assert(frame_is_valid(out));
  return out;
}

static FirmwareConsensusFrame make_leader_commit_frame(const FirmwareNode& node) {
  FirmwareConsensusFrame out;
  format_frame_id(
    node.self.node_id,
    AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT,
    node.current_term,
    node.leader_node_id,
    out.frame_id,
    sizeof(out.frame_id));
  out.kind = AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT;
  out.term = node.current_term;
  out.decision_sha256 = node.committed_decision_sha256;
  out.leader_node_id = node.leader_node_id;
  out.from = node.self;
  out.vote_witness_count = node.committed_witness_count;
  for (size_t i = 0; i < out.vote_witness_count; ++i) {
    out.vote_witnesses[i] = node.committed_witnesses[i];
  }
  assert(frame_is_valid(out));
  return out;
}

static FirmwareConsensusFrame start_campaign(FirmwareNode* node, const char* decision_sha256) {
  assert(node);
  store_token(node->campaign_decision_sha256, sizeof(node->campaign_decision_sha256), decision_sha256);
  node->committed_decision_sha256[0] = '\0';
  node->leader_node_id[0] = '\0';
  node->grant_witness_count = 0;
  node->committed_witness_count = 0;
  node->current_term = agent_edge_consensus_next_term(node->current_term);
  node->frame_sequence = agent_edge_consensus_next_frame_sequence(node->frame_sequence);
  store_token(node->voted_for_node_id, sizeof(node->voted_for_node_id), node->self.node_id);

  FirmwareConsensusFrame out;
  format_frame_id(
    node->self.node_id,
    AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST,
    node->frame_sequence,
    nullptr,
    out.frame_id,
    sizeof(out.frame_id));
  out.kind = AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST;
  out.term = node->current_term;
  out.decision_sha256 = node->campaign_decision_sha256;
  out.candidate_node_id = node->self.node_id;
  out.from = node->self;
  assert(frame_is_valid(out));
  return out;
}

static bool leader_commit_witnesses_valid(const FirmwareNode& node, const FirmwareConsensusFrame& frame) {
  size_t valid_witness_count = 0;
  bool leader_is_witness = false;
  std::array<const char*, kMaxWitnesses> seen_node_ids = {};
  size_t seen_node_id_count = 0;

  for (size_t i = 0; i < frame.vote_witness_count; ++i) {
    const FirmwareIdentity& witness = frame.vote_witnesses[i];
    bool already_seen = false;
    for (size_t j = 0; j < seen_node_id_count; ++j) {
      if (node_id_matches(seen_node_ids[j], witness.node_id)) {
        already_seen = true;
        break;
      }
    }
    if (seen_node_id_count < seen_node_ids.size()) {
      seen_node_ids[seen_node_id_count++] = witness.node_id;
    }
    if (!agent_edge_consensus_leader_commit_witness_can_count(
          membership_matches(node, witness) ? 1 : 0,
          trust_epochs_match(node, witness) ? 1 : 0,
          already_seen ? 1 : 0)) {
      continue;
    }
    valid_witness_count++;
    if (node_id_matches(witness.node_id, frame.leader_node_id)) leader_is_witness = true;
  }

  return agent_edge_consensus_leader_commit_witnesses_can_accept(
           node.cluster_size,
           valid_witness_count,
           leader_is_witness ? 1 : 0) != 0;
}

static bool add_grant_witness(FirmwareNode* node, const FirmwareIdentity& witness) {
  assert(node);
  for (size_t i = 0; i < node->grant_witness_count; ++i) {
    if (node_id_matches(node->grant_witnesses[i].node_id, witness.node_id)) return false;
  }
  assert(node->grant_witness_count < node->grant_witnesses.size());
  node->grant_witnesses[node->grant_witness_count++] = witness;
  return true;
}

static bool handle_frame(
  FirmwareNode* node,
  const FirmwareConsensusFrame& frame,
  std::array<FirmwareConsensusFrame, 2>* out_frames,
  size_t* out_count,
  bool* out_accepted
) {
  assert(node);
  assert(out_frames);
  assert(out_count);
  assert(out_accepted);
  *out_count = 0;
  *out_accepted = false;
  if (!frame_is_valid(frame)) return false;
  if (!cluster_matches(*node, frame.from)) return false;
  if (!membership_matches(*node, frame.from)) return true;
  if (seen_frame_should_drop(node, frame)) return true;

  if (token_eq(frame.kind, AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST)) {
    if (!agent_edge_consensus_vote_request_can_grant(
          node->current_term,
          frame.term,
          node_is_member(*node, frame.candidate_node_id) ? 1 : 0,
          node_id_matches(frame.candidate_node_id, frame.from.node_id) ? 1 : 0,
          trust_epochs_match(*node, frame.from) ? 1 : 0,
          node->voted_for_node_id,
          token_len(node->voted_for_node_id),
          frame.candidate_node_id,
          token_len(frame.candidate_node_id))) {
      return true;
    }
    reset_for_new_term(node, frame.term);
    store_token(node->voted_for_node_id, sizeof(node->voted_for_node_id), frame.candidate_node_id);
    (*out_frames)[(*out_count)++] = make_vote_grant_frame(*node, frame.candidate_node_id, frame.decision_sha256);
    *out_accepted = true;
    return true;
  }

  if (token_eq(frame.kind, AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT)) {
    if (agent_edge_consensus_incoming_term_is_stale(node->current_term, frame.term)) return true;
    reset_for_new_term(node, frame.term);
    if (!agent_edge_consensus_vote_grant_can_count(
          node->current_term,
          frame.term,
          node_is_member(*node, frame.candidate_node_id) ? 1 : 0,
          node_id_matches(frame.candidate_node_id, node->self.node_id) ? 1 : 0,
          node_id_matches(frame.from.node_id, frame.candidate_node_id) ? 1 : 0,
          decision_matches(node->campaign_decision_sha256, frame.decision_sha256) ? 1 : 0,
          frame.granted ? 1 : 0,
          trust_epochs_match(*node, frame.from) ? 1 : 0)) {
      return true;
    }
    (void)add_grant_witness(node, frame.from);
    *out_accepted = true;
    const bool has_quorum = agent_edge_consensus_has_quorum(
                              node->cluster_size,
                              agent_edge_consensus_vote_count_with_self(node->grant_witness_count)) != 0;
    if (!agent_edge_consensus_candidate_can_commit(
          token_len(node->leader_node_id) > 0 ? 1 : 0,
          has_quorum ? 1 : 0)) {
      return true;
    }
    store_token(node->leader_node_id, sizeof(node->leader_node_id), node->self.node_id);
    store_token(node->committed_decision_sha256, sizeof(node->committed_decision_sha256), node->campaign_decision_sha256);
    node->committed_witness_count = 0;
    node->committed_witnesses[node->committed_witness_count++] = node->self;
    for (size_t i = 0; i < node->grant_witness_count; ++i) {
      node->committed_witnesses[node->committed_witness_count++] = node->grant_witnesses[i];
    }
    (*out_frames)[(*out_count)++] = make_leader_commit_frame(*node);
    return true;
  }

  if (!agent_edge_consensus_leader_commit_can_accept(
        node->current_term,
        frame.term,
        node_is_member(*node, frame.leader_node_id) ? 1 : 0,
        node_id_matches(frame.from.node_id, frame.leader_node_id) ? 1 : 0,
        trust_epochs_match(*node, frame.from) ? 1 : 0)) {
    return true;
  }
  if (!leader_commit_witnesses_valid(*node, frame)) return true;
  reset_for_new_term(node, frame.term);
  store_token(node->leader_node_id, sizeof(node->leader_node_id), frame.leader_node_id);
  store_token(node->voted_for_node_id, sizeof(node->voted_for_node_id), frame.leader_node_id);
  node->campaign_decision_sha256[0] = '\0';
  store_token(node->committed_decision_sha256, sizeof(node->committed_decision_sha256), frame.decision_sha256);
  node->committed_witness_count = frame.vote_witness_count;
  for (size_t i = 0; i < frame.vote_witness_count; ++i) {
    node->committed_witnesses[i] = frame.vote_witnesses[i];
  }
  *out_accepted = true;
  return true;
}

static FirmwareMembershipBundle make_membership_bundle(uint64_t membership_epoch, uint64_t previous_epoch) {
  FirmwareMembershipBundle out;
  out.previous_epoch = previous_epoch;
  out.membership_epoch = membership_epoch;
  out.member_node_ids = default_members();
  out.member_count = out.member_node_ids.size();
  return out;
}

static bool bundle_contains_self(const FirmwareNode& node, const FirmwareMembershipBundle& bundle) {
  for (size_t i = 0; i < bundle.member_count; ++i) {
    if (node_id_matches(node.self.node_id, bundle.member_node_ids[i])) return true;
  }
  return false;
}

static bool apply_membership_bundle(
  FirmwareNode* node,
  const FirmwareMembershipBundle& bundle,
  bool* out_adopted
) {
  assert(node);
  assert(out_adopted);
  *out_adopted = false;
  if (agent_edge_consensus_membership_policy_header_validate(
        bundle.schema,
        token_len(bundle.schema),
        bundle.cluster_id,
        token_len(bundle.cluster_id)) != AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_HEADER_OK) {
    return false;
  }
  if (!agent_edge_consensus_cluster_id_matches(
        node->self.cluster_id,
        token_len(node->self.cluster_id),
        bundle.cluster_id,
        token_len(bundle.cluster_id))) {
    return false;
  }
  if (!agent_edge_consensus_membership_member_set_is_nonempty(bundle.member_count)) return false;
  for (size_t i = 0; i < bundle.member_count; ++i) {
    if (!agent_edge_consensus_member_node_id_is_valid(
          bundle.member_node_ids[i],
          token_len(bundle.member_node_ids[i]))) {
      return false;
    }
  }
  if (!agent_edge_consensus_membership_lineage_is_valid(
        bundle.previous_epoch,
        bundle.membership_epoch)) {
    return false;
  }
  if (!agent_edge_consensus_membership_policy_can_adopt(
        node->self.membership_epoch,
        bundle.membership_epoch,
        bundle_contains_self(*node, bundle) ? 1 : 0)) {
    return true;
  }
  const uint64_t lineage[1] = {bundle.previous_epoch};
  if (!agent_edge_consensus_membership_epoch_is_recoverable(
        node->self.membership_epoch,
        bundle.membership_epoch,
        bundle.previous_epoch,
        lineage,
        1)) {
    return false;
  }
  if (!agent_edge_consensus_membership_epoch_member_set_can_recover(
        bundle.membership_epoch,
        bundle.membership_epoch,
        bundle_contains_self(*node, bundle) ? 1 : 0)) {
    return false;
  }
  node->self.membership_epoch = bundle.membership_epoch;
  node->member_count = bundle.member_count;
  for (size_t i = 0; i < bundle.member_count; ++i) {
    node->member_node_ids[i] = bundle.member_node_ids[i];
  }
  node->cluster_size = agent_edge_consensus_cluster_size_from_member_count(node->member_count);
  reset_consensus_state(node);
  *out_adopted = true;
  return true;
}

static void test_firmware_loop_survives_lossy_frame_and_membership_replay() {
  FirmwareNode node_a = make_node("node-a");
  FirmwareNode node_b = make_node("node-b");
  FirmwareNode node_c = make_node("node-c");
  bool adopted = false;

  const FirmwareMembershipBundle epoch_one = make_membership_bundle(1, 0);
  assert(apply_membership_bundle(&node_a, epoch_one, &adopted));
  assert(!adopted);

  const FirmwareMembershipBundle epoch_two = make_membership_bundle(2, 1);
  assert(apply_membership_bundle(&node_a, epoch_two, &adopted));
  assert(adopted);
  assert(apply_membership_bundle(&node_b, epoch_two, &adopted));
  assert(adopted);
  assert(apply_membership_bundle(&node_c, epoch_two, &adopted));
  assert(adopted);

  assert(apply_membership_bundle(&node_b, epoch_two, &adopted));
  assert(!adopted);
  assert(apply_membership_bundle(&node_c, epoch_one, &adopted));
  assert(!adopted);
  assert(node_c.self.membership_epoch == 2);

  const FirmwareConsensusFrame request = start_campaign(&node_a, kDecisionSha);
  assert(agent_edge_consensus_frame_route(
           request.kind,
           token_len(request.kind),
           1,
           1) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_PEERS);

  std::array<FirmwareConsensusFrame, 2> emitted = {};
  size_t emitted_count = 0;
  bool accepted = false;

  assert(handle_frame(&node_b, request, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(emitted_count == 1);
  FirmwareConsensusFrame grant_b = emitted[0];
  assert(agent_edge_consensus_frame_route(
           grant_b.kind,
           token_len(grant_b.kind),
           1,
           0) == AGENT_EDGE_CONSENSUS_FRAME_ROUTE_CANDIDATE);

  assert(handle_frame(&node_b, request, &emitted, &emitted_count, &accepted));
  assert(!accepted);
  assert(emitted_count == 0);

  assert(handle_frame(&node_a, grant_b, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(emitted_count == 0);
  assert(token_len(node_a.leader_node_id) == 0);

  assert(handle_frame(&node_a, grant_b, &emitted, &emitted_count, &accepted));
  assert(!accepted);
  assert(emitted_count == 0);
  assert(token_len(node_a.leader_node_id) == 0);

  assert(handle_frame(&node_c, request, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(emitted_count == 1);
  const FirmwareConsensusFrame grant_c = emitted[0];

  assert(handle_frame(&node_a, grant_c, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(emitted_count == 1);
  const FirmwareConsensusFrame commit = emitted[0];
  assert(token_eq(commit.kind, AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT));
  assert(token_eq(node_a.leader_node_id, "node-a"));
  assert(token_eq(node_a.committed_decision_sha256, kDecisionSha));
  assert(commit.vote_witness_count == 3);

  assert(handle_frame(&node_b, commit, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(emitted_count == 0);
  assert(token_eq(node_b.leader_node_id, "node-a"));
  assert(token_eq(node_b.committed_decision_sha256, kDecisionSha));

  assert(handle_frame(&node_b, commit, &emitted, &emitted_count, &accepted));
  assert(!accepted);
  assert(emitted_count == 0);
  assert(token_eq(node_b.leader_node_id, "node-a"));

  assert(handle_frame(&node_c, commit, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(token_eq(node_c.leader_node_id, "node-a"));

  const FirmwareMembershipBundle epoch_three = make_membership_bundle(3, 2);
  assert(node_a.self.membership_epoch == 2);
  assert(apply_membership_bundle(&node_a, epoch_three, &adopted));
  assert(adopted);
  assert(node_a.self.membership_epoch == 3);
  assert(token_len(node_a.leader_node_id) == 0);
}

static void test_firmware_loop_rejects_stale_and_mismatched_frames() {
  FirmwareNode node_a = make_node("node-a");
  FirmwareNode node_b = make_node("node-b");
  bool adopted = false;
  const FirmwareMembershipBundle epoch_two = make_membership_bundle(2, 1);
  assert(apply_membership_bundle(&node_a, epoch_two, &adopted));
  assert(adopted);
  assert(apply_membership_bundle(&node_b, epoch_two, &adopted));
  assert(adopted);

  FirmwareConsensusFrame request = start_campaign(&node_a, kDecisionSha);
  std::array<FirmwareConsensusFrame, 2> emitted = {};
  size_t emitted_count = 0;
  bool accepted = false;
  assert(handle_frame(&node_b, request, &emitted, &emitted_count, &accepted));
  assert(accepted);
  assert(node_b.current_term == 1);

  request.term = 0;
  assert(!frame_is_valid(request));
  request.term = 1;
  request.from.cluster_id = "other-cluster";
  assert(!handle_frame(&node_b, request, &emitted, &emitted_count, &accepted));
}

}  // namespace

int main() {
  test_firmware_loop_survives_lossy_frame_and_membership_replay();
  test_firmware_loop_rejects_stale_and_mismatched_frames();
  return 0;
}
