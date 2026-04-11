#include "agent/edge_interop.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int umbmp_char_ok(char c) {
  const int ok =
    (c >= 'a' && c <= 'z') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= '0' && c <= '9') ||
    (c == '-') || (c == '_') || (c == '.') || (c == ':');
  return ok ? 1 : 0;
}

static int umbmp_trace_char_ok(char c) {
  // trace_id extends the id token set with '@' (UM‑EAIS v0.2, platform trace_id validator).
  if (umbmp_char_ok(c)) return 1;
  return (c == '@') ? 1 : 0;
}

static int hex_char_ok(char c) {
  const int ok =
    (c >= '0' && c <= '9') ||
    (c >= 'a' && c <= 'f') ||
    (c >= 'A' && c <= 'F');
  return ok ? 1 : 0;
}

int agent_umbmp_id_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0 || len > AGENT_UM_BMP_MAX_ID_LEN) return 0;
  for (size_t i = 0; i < len; i++) {
    if (!umbmp_char_ok(s[i])) return 0;
  }
  return 1;
}

int agent_umbmp_trace_id_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0 || len > AGENT_UM_BMP_MAX_ID_LEN) return 0;
  for (size_t i = 0; i < len; i++) {
    if (!umbmp_trace_char_ok(s[i])) return 0;
  }
  return 1;
}

int agent_umbmp_sha256_token_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0) return 0;

  const char* p = s;
  size_t n = len;
  static const char* kPrefix = "sha256:";
  static const size_t kPrefixLen = 7;
  if (n > kPrefixLen && memcmp(p, kPrefix, kPrefixLen) == 0) {
    p += kPrefixLen;
    n -= kPrefixLen;
  }
  if (n != 64) return 0;
  for (size_t i = 0; i < n; i++) {
    if (!hex_char_ok(p[i])) return 0;
  }
  return 1;
}

agent_status_t agent_umbmp_sanitize_id_token(
  const char* in,
  size_t in_len,
  char* out,
  size_t out_cap,
  size_t max_len,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';

  const size_t cap_len = out_cap - 1; // reserve NUL
  if (cap_len == 0) return AGENT_ERR_BOUNDS;

  if (max_len == 0 || max_len > cap_len) max_len = cap_len;
  size_t n = in_len;
  if (n > max_len) n = max_len;

  // Map invalid characters to '_' and truncate.
  for (size_t i = 0; i < n; i++) {
    const char c = in ? in[i] : '\0';
    out[i] = umbmp_char_ok(c) ? c : '_';
  }

  // Trim leading/trailing underscores.
  size_t start = 0;
  while (start < n && out[start] == '_') start++;
  size_t end = n;
  while (end > start && out[end - 1] == '_') end--;

  if (end <= start) {
    // Default token.
    static const char* kDefault = "msg";
    const size_t kDefaultLen = 3;
    if (out_cap <= kDefaultLen) return AGENT_ERR_BOUNDS;
    memcpy(out, kDefault, kDefaultLen);
    out[kDefaultLen] = '\0';
    if (out_len) *out_len = kDefaultLen;
    return AGENT_OK;
  }

  const size_t trimmed_len = end - start;
  if (start != 0) memmove(out, out + start, trimmed_len);
  out[trimmed_len] = '\0';
  if (out_len) *out_len = trimmed_len;
  return AGENT_OK;
}

agent_status_t agent_um_eais_result_attest_signing_input_v0_1(
  const char* task_id,
  size_t task_id_len,
  const char* step_id,
  size_t step_id_len,
  const char* idempotency_key,
  size_t idempotency_key_len,
  const char* result_sha256_token,
  size_t result_sha256_token_len,
  int64_t ts_utc_ms,
  char* out,
  size_t out_cap,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';

  if (!agent_umbmp_id_is_safe(task_id, task_id_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(step_id, step_id_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(idempotency_key, idempotency_key_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_sha256_token_is_safe(result_sha256_token, result_sha256_token_len)) return AGENT_ERR_INVALID_ARGUMENT;

  // Reserve NUL for convenience, but return the byte length excluding it.
  const size_t cap_len = out_cap - 1;
  if (cap_len == 0) return AGENT_ERR_BOUNDS;

  // Convert timestamp once so we can compute/validate bounds.
  char ts_buf[32];
  const int ts_n = snprintf(ts_buf, sizeof(ts_buf), "%" PRId64 "\n", ts_utc_ms);
  if (ts_n <= 0) return AGENT_ERR_INTERNAL;
  const size_t ts_len = (size_t)ts_n;
  if (ts_len >= sizeof(ts_buf)) return AGENT_ERR_INTERNAL;

  static const char* kPrefix = AGENT_UM_EAIS_RESULT_ATTEST_SIGNING_PREFIX;
  static const size_t kPrefixLen = sizeof("UM_EAIS_RESULT_ATTEST_v0_1\n") - 1;

  // Compute required size (bytes), including newlines after each field.
  const size_t need =
    kPrefixLen +
    task_id_len + 1 +
    step_id_len + 1 +
    idempotency_key_len + 1 +
    result_sha256_token_len + 1 +
    ts_len;

  if (need > cap_len) return AGENT_ERR_BOUNDS;

  size_t pos = 0;
  memcpy(out + pos, kPrefix, kPrefixLen);
  pos += kPrefixLen;

  memcpy(out + pos, task_id, task_id_len);
  pos += task_id_len;
  out[pos++] = '\n';

  memcpy(out + pos, step_id, step_id_len);
  pos += step_id_len;
  out[pos++] = '\n';

  memcpy(out + pos, idempotency_key, idempotency_key_len);
  pos += idempotency_key_len;
  out[pos++] = '\n';

  memcpy(out + pos, result_sha256_token, result_sha256_token_len);
  pos += result_sha256_token_len;
  out[pos++] = '\n';

  memcpy(out + pos, ts_buf, ts_len);
  pos += ts_len;

  out[pos] = '\0';
  if (out_len) *out_len = pos;
  return AGENT_OK;
}

size_t agent_edge_consensus_cluster_size_normalize(size_t cluster_size) {
  if (cluster_size < 1) cluster_size = 1;
  return cluster_size;
}

size_t agent_edge_consensus_cluster_size_from_peer_count(size_t peer_count) {
  if (peer_count == (size_t)-1) return (size_t)-1;
  return agent_edge_consensus_cluster_size_normalize(peer_count + 1);
}

size_t agent_edge_consensus_cluster_size_from_member_count(size_t member_count) {
  return agent_edge_consensus_cluster_size_normalize(member_count);
}

size_t agent_edge_consensus_quorum_for_cluster_size(size_t cluster_size) {
  cluster_size = agent_edge_consensus_cluster_size_normalize(cluster_size);
  return (cluster_size / 2) + 1;
}

int agent_edge_consensus_has_quorum(size_t cluster_size, size_t vote_count) {
  return vote_count >= agent_edge_consensus_quorum_for_cluster_size(cluster_size) ? 1 : 0;
}

static int consensus_string_eq(const char* s, size_t s_len, const char* lit) {
  if (!s || !lit) return 0;
  const size_t lit_len = strlen(lit);
  return s_len == lit_len && memcmp(s, lit, lit_len) == 0;
}

agent_edge_consensus_message_type_t agent_edge_consensus_message_type_classify(
  const char* type,
  size_t type_len
) {
  if (consensus_string_eq(type, type_len, AGENT_UM_BMP_TYPE_CONSENSUS_FRAME)) {
    return AGENT_EDGE_CONSENSUS_MESSAGE_FRAME;
  }
  if (consensus_string_eq(type, type_len, AGENT_UM_BMP_TYPE_PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE)) {
    return AGENT_EDGE_CONSENSUS_MESSAGE_MEMBERSHIP_BUNDLE;
  }
  return AGENT_EDGE_CONSENSUS_MESSAGE_OTHER;
}

int agent_edge_consensus_frame_kind_is_valid(const char* kind, size_t kind_len) {
  return
    consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST) ||
    consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT) ||
    consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT);
}

agent_status_t agent_edge_consensus_frame_id_format(
  const char* node_id,
  size_t node_id_len,
  const char* kind,
  size_t kind_len,
  uint64_t number,
  const char* suffix,
  size_t suffix_len,
  char* out,
  size_t out_cap,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';
  if (!agent_edge_consensus_member_node_id_is_valid(node_id, node_id_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_edge_consensus_frame_kind_is_valid(kind, kind_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (suffix_len > 0 && !agent_edge_consensus_member_node_id_is_valid(suffix, suffix_len)) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  char number_buf[32];
  const int number_n = snprintf(number_buf, sizeof(number_buf), "%" PRIu64, number);
  if (number_n <= 0 || (size_t)number_n >= sizeof(number_buf)) return AGENT_ERR_INTERNAL;
  const size_t number_len = (size_t)number_n;
  const size_t cap_len = out_cap - 1;
  const size_t need = node_id_len + 1 + kind_len + 1 + number_len + (suffix_len > 0 ? 1 + suffix_len : 0);
  if (need > cap_len || need > AGENT_UM_BMP_MAX_ID_LEN) return AGENT_ERR_BOUNDS;

  size_t pos = 0;
  memcpy(out + pos, node_id, node_id_len);
  pos += node_id_len;
  out[pos++] = ':';
  memcpy(out + pos, kind, kind_len);
  pos += kind_len;
  out[pos++] = ':';
  memcpy(out + pos, number_buf, number_len);
  pos += number_len;
  if (suffix_len > 0) {
    out[pos++] = ':';
    memcpy(out + pos, suffix, suffix_len);
    pos += suffix_len;
  }
  out[pos] = '\0';
  if (!agent_umbmp_id_is_safe(out, pos)) return AGENT_ERR_INTERNAL;
  if (out_len) *out_len = pos;
  return AGENT_OK;
}

agent_edge_consensus_identity_validation_t agent_edge_consensus_identity_validate(
  const char* cluster_id,
  size_t cluster_id_len,
  const char* node_id,
  size_t node_id_len,
  const char* manifest_sha256,
  size_t manifest_sha256_len
) {
  if (!agent_umbmp_id_is_safe(cluster_id, cluster_id_len)) {
    return AGENT_EDGE_CONSENSUS_IDENTITY_CLUSTER_ID_INVALID;
  }
  if (!agent_edge_consensus_member_node_id_is_valid(node_id, node_id_len)) {
    return AGENT_EDGE_CONSENSUS_IDENTITY_NODE_ID_INVALID;
  }
  if (manifest_sha256_len > 0 && !agent_umbmp_sha256_token_is_safe(manifest_sha256, manifest_sha256_len)) {
    return AGENT_EDGE_CONSENSUS_IDENTITY_MANIFEST_SHA256_INVALID;
  }
  return AGENT_EDGE_CONSENSUS_IDENTITY_OK;
}

agent_edge_consensus_frame_validation_t agent_edge_consensus_frame_validate(
  const char* schema,
  size_t schema_len,
  const char* kind,
  size_t kind_len,
  const char* frame_id,
  size_t frame_id_len,
  uint64_t term,
  const char* decision_sha256,
  size_t decision_sha256_len,
  const char* from_cluster_id,
  size_t from_cluster_id_len,
  const char* from_node_id,
  size_t from_node_id_len,
  const char* from_manifest_sha256,
  size_t from_manifest_sha256_len,
  const char* candidate_node_id,
  size_t candidate_node_id_len,
  const char* leader_node_id,
  size_t leader_node_id_len
) {
  if (!consensus_string_eq(schema, schema_len, AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1)) {
    return AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_INVALID;
  }
  if (!agent_edge_consensus_frame_kind_is_valid(kind, kind_len)) {
    return AGENT_EDGE_CONSENSUS_FRAME_KIND_INVALID;
  }
  if (!agent_umbmp_id_is_safe(frame_id, frame_id_len)) {
    return AGENT_EDGE_CONSENSUS_FRAME_ID_INVALID;
  }
  if (term < 1) {
    return AGENT_EDGE_CONSENSUS_FRAME_TERM_INVALID;
  }
  if (decision_sha256_len > 0 && !agent_umbmp_sha256_token_is_safe(decision_sha256, decision_sha256_len)) {
    return AGENT_EDGE_CONSENSUS_FRAME_DECISION_SHA256_INVALID;
  }
  const agent_edge_consensus_identity_validation_t from_validation = agent_edge_consensus_identity_validate(
    from_cluster_id,
    from_cluster_id_len,
    from_node_id,
    from_node_id_len,
    from_manifest_sha256,
    from_manifest_sha256_len);
  if (from_validation == AGENT_EDGE_CONSENSUS_IDENTITY_CLUSTER_ID_INVALID) {
    return AGENT_EDGE_CONSENSUS_FRAME_FROM_CLUSTER_ID_INVALID;
  }
  if (from_validation == AGENT_EDGE_CONSENSUS_IDENTITY_NODE_ID_INVALID) {
    return AGENT_EDGE_CONSENSUS_FRAME_FROM_NODE_ID_INVALID;
  }
  if (from_validation == AGENT_EDGE_CONSENSUS_IDENTITY_MANIFEST_SHA256_INVALID) {
    return AGENT_EDGE_CONSENSUS_FRAME_FROM_MANIFEST_SHA256_INVALID;
  }
  if (consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST) ||
      consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT)) {
    if (!agent_edge_consensus_member_node_id_is_valid(candidate_node_id, candidate_node_id_len)) {
      return AGENT_EDGE_CONSENSUS_FRAME_CANDIDATE_NODE_ID_INVALID;
    }
  }
  if (consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT) &&
      !agent_edge_consensus_member_node_id_is_valid(leader_node_id, leader_node_id_len)) {
    return AGENT_EDGE_CONSENSUS_FRAME_LEADER_NODE_ID_INVALID;
  }
  return AGENT_EDGE_CONSENSUS_FRAME_OK;
}

int agent_edge_consensus_identity_membership_matches(
  uint64_t local_membership_epoch,
  uint64_t identity_membership_epoch,
  int identity_node_is_member
) {
  return local_membership_epoch == identity_membership_epoch && identity_node_is_member ? 1 : 0;
}

int agent_edge_consensus_cluster_id_matches(
  const char* local_cluster_id,
  size_t local_cluster_id_len,
  const char* peer_cluster_id,
  size_t peer_cluster_id_len
) {
  if (!agent_umbmp_id_is_safe(local_cluster_id, local_cluster_id_len)) return 0;
  if (!agent_umbmp_id_is_safe(peer_cluster_id, peer_cluster_id_len)) return 0;
  return local_cluster_id_len == peer_cluster_id_len &&
         memcmp(local_cluster_id, peer_cluster_id, peer_cluster_id_len) == 0 ? 1 : 0;
}

int agent_edge_consensus_node_id_matches(
  const char* local_node_id,
  size_t local_node_id_len,
  const char* peer_node_id,
  size_t peer_node_id_len
) {
  if (!agent_edge_consensus_member_node_id_is_valid(local_node_id, local_node_id_len)) return 0;
  if (!agent_edge_consensus_member_node_id_is_valid(peer_node_id, peer_node_id_len)) return 0;
  return local_node_id_len == peer_node_id_len &&
         memcmp(local_node_id, peer_node_id, peer_node_id_len) == 0 ? 1 : 0;
}

int agent_edge_consensus_trust_epochs_match(
  uint64_t local_trust_roots_epoch,
  uint64_t local_revocations_epoch,
  uint64_t local_cert_roots_epoch,
  uint64_t peer_trust_roots_epoch,
  uint64_t peer_revocations_epoch,
  uint64_t peer_cert_roots_epoch
) {
  return local_trust_roots_epoch == peer_trust_roots_epoch &&
         local_revocations_epoch == peer_revocations_epoch &&
         local_cert_roots_epoch == peer_cert_roots_epoch ? 1 : 0;
}

int agent_edge_consensus_decision_sha256_matches(
  const char* local_decision_sha256,
  size_t local_decision_sha256_len,
  const char* peer_decision_sha256,
  size_t peer_decision_sha256_len
) {
  if (!agent_umbmp_sha256_token_is_safe(local_decision_sha256, local_decision_sha256_len)) return 0;
  if (!agent_umbmp_sha256_token_is_safe(peer_decision_sha256, peer_decision_sha256_len)) return 0;
  return local_decision_sha256_len == peer_decision_sha256_len &&
         memcmp(local_decision_sha256, peer_decision_sha256, peer_decision_sha256_len) == 0 ? 1 : 0;
}

static int consensus_space_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int agent_edge_consensus_decision_sha256_is_set(
  const char* decision_sha256,
  size_t decision_sha256_len
) {
  if (!decision_sha256) return 0;
  for (size_t i = 0; i < decision_sha256_len; i++) {
    if (!consensus_space_char(decision_sha256[i])) return 1;
  }
  return 0;
}

agent_edge_consensus_campaign_decision_source_t agent_edge_consensus_campaign_decision_source(
  const char* configured_decision_sha256,
  size_t configured_decision_sha256_len,
  const char* last_known_decision_sha256,
  size_t last_known_decision_sha256_len
) {
  if (agent_edge_consensus_decision_sha256_is_set(
        configured_decision_sha256,
        configured_decision_sha256_len)) {
    return AGENT_EDGE_CONSENSUS_CAMPAIGN_DECISION_CONFIG;
  }
  if (agent_edge_consensus_decision_sha256_is_set(
        last_known_decision_sha256,
        last_known_decision_sha256_len)) {
    return AGENT_EDGE_CONSENSUS_CAMPAIGN_DECISION_LAST_KNOWN;
  }
  return AGENT_EDGE_CONSENSUS_CAMPAIGN_DECISION_NONE;
}

int agent_edge_consensus_vote_request_can_grant(
  uint64_t current_term,
  uint64_t request_term,
  int candidate_node_is_member,
  int candidate_is_sender,
  int trust_epochs_match,
  const char* voted_for_node_id,
  size_t voted_for_node_id_len,
  const char* candidate_node_id,
  size_t candidate_node_id_len
) {
  if (!candidate_node_is_member || !candidate_is_sender || !trust_epochs_match) return 0;
  if (request_term < current_term) return 0;
  if (request_term > current_term) return 1;
  if (voted_for_node_id_len == 0) return 1;
  if (!voted_for_node_id || !candidate_node_id) return 0;
  return agent_edge_consensus_node_id_matches(
    voted_for_node_id,
    voted_for_node_id_len,
    candidate_node_id,
    candidate_node_id_len);
}

int agent_edge_consensus_vote_grant_can_count(
  uint64_t current_term,
  uint64_t grant_term,
  int candidate_node_is_member,
  int candidate_is_self,
  int grant_sender_is_candidate,
  int campaign_decision_matches,
  int granted,
  int trust_epochs_match
) {
  return grant_term >= current_term &&
         candidate_node_is_member &&
         candidate_is_self &&
         !grant_sender_is_candidate &&
         campaign_decision_matches &&
         granted &&
         trust_epochs_match ? 1 : 0;
}

int agent_edge_consensus_leader_commit_can_accept(
  uint64_t current_term,
  uint64_t commit_term,
  int leader_node_is_member,
  int leader_is_sender,
  int trust_epochs_match
) {
  return commit_term >= current_term && leader_node_is_member && leader_is_sender && trust_epochs_match ? 1 : 0;
}

int agent_edge_consensus_leader_commit_witness_can_count(
  int witness_membership_matches,
  int witness_trust_epochs_match,
  int witness_already_seen
) {
  return witness_membership_matches && witness_trust_epochs_match && !witness_already_seen ? 1 : 0;
}

int agent_edge_consensus_leader_commit_witnesses_can_accept(
  size_t cluster_size,
  size_t valid_witness_count,
  int leader_is_witness
) {
  return leader_is_witness && agent_edge_consensus_has_quorum(cluster_size, valid_witness_count) ? 1 : 0;
}

size_t agent_edge_consensus_vote_count_with_self(size_t grant_witness_count) {
  if (grant_witness_count == SIZE_MAX) return SIZE_MAX;
  return grant_witness_count + 1;
}

int agent_edge_consensus_candidate_can_commit(
  int has_leader,
  int has_quorum
) {
  return !has_leader && has_quorum ? 1 : 0;
}

int agent_edge_consensus_incoming_term_advances(
  uint64_t current_term,
  uint64_t incoming_term
) {
  return incoming_term > current_term ? 1 : 0;
}

int agent_edge_consensus_incoming_term_is_stale(
  uint64_t current_term,
  uint64_t incoming_term
) {
  return incoming_term < current_term ? 1 : 0;
}

int agent_edge_consensus_seen_frame_should_drop(
  int frame_id_seen,
  uint64_t seen_term,
  uint64_t frame_term
) {
  return frame_id_seen && seen_term == frame_term ? 1 : 0;
}

agent_edge_consensus_frame_route_t agent_edge_consensus_frame_route(
  const char* kind,
  size_t kind_len,
  int candidate_node_id_is_valid,
  int candidate_is_self
) {
  if (consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT)) {
    return candidate_node_id_is_valid && !candidate_is_self
      ? AGENT_EDGE_CONSENSUS_FRAME_ROUTE_CANDIDATE
      : AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP;
  }
  if (consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST) ||
      consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT)) {
    return AGENT_EDGE_CONSENSUS_FRAME_ROUTE_PEERS;
  }
  return AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP;
}

int agent_edge_consensus_member_node_id_is_valid(const char* node_id, size_t node_id_len) {
  return agent_umbmp_id_is_safe(node_id, node_id_len);
}

int agent_edge_consensus_membership_epoch_can_advance(
  uint64_t current_epoch,
  uint64_t next_epoch
) {
  return next_epoch > current_epoch ? 1 : 0;
}

int agent_edge_consensus_membership_policy_can_adopt(
  uint64_t current_epoch,
  uint64_t next_epoch,
  int self_node_is_member
) {
  return self_node_is_member &&
    agent_edge_consensus_membership_epoch_can_advance(current_epoch, next_epoch) ? 1 : 0;
}

agent_edge_consensus_membership_policy_header_validation_t
agent_edge_consensus_membership_policy_header_validate(
  const char* schema,
  size_t schema_len,
  const char* cluster_id,
  size_t cluster_id_len
) {
  if (!consensus_string_eq(schema, schema_len, AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1)) {
    return AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_SCHEMA_INVALID;
  }
  if (!agent_umbmp_id_is_safe(cluster_id, cluster_id_len)) {
    return AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_CLUSTER_ID_INVALID;
  }
  return AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_HEADER_OK;
}

int agent_edge_consensus_membership_member_set_is_nonempty(size_t member_count) {
  return member_count > 0 ? 1 : 0;
}

int agent_edge_consensus_membership_lineage_is_valid(
  uint64_t previous_epoch,
  uint64_t current_epoch
) {
  if (previous_epoch == 0) return 1;
  return current_epoch > 0 && previous_epoch < current_epoch ? 1 : 0;
}

int agent_edge_consensus_membership_epoch_is_recoverable(
  uint64_t runtime_epoch,
  uint64_t current_epoch,
  uint64_t previous_epoch,
  const uint64_t* lineage_epochs,
  size_t lineage_len
) {
  if (runtime_epoch == 0 || current_epoch == 0) return 1;
  if (runtime_epoch == current_epoch) return 1;
  if (previous_epoch != 0 && runtime_epoch == previous_epoch) return 1;
  if (lineage_epochs) {
    for (size_t i = 0; i < lineage_len; i++) {
      if (lineage_epochs[i] != 0 && runtime_epoch == lineage_epochs[i]) return 1;
    }
  }
  return 0;
}

int agent_edge_consensus_membership_epoch_member_set_can_recover(
  uint64_t runtime_epoch,
  uint64_t policy_epoch,
  int runtime_node_is_member
) {
  if (runtime_epoch == 0 || policy_epoch == 0) return 1;
  return runtime_epoch == policy_epoch && runtime_node_is_member ? 1 : 0;
}

static int64_t consensus_clamp_i64(int64_t value, int64_t lo, int64_t hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

agent_status_t agent_edge_consensus_policy_timing_normalize(
  agent_edge_consensus_policy_timing_t* timing
) {
  if (!timing) return AGENT_ERR_INVALID_ARGUMENT;
  timing->campaign_delay_ms = consensus_clamp_i64(
    timing->campaign_delay_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  timing->campaign_retry_ms = consensus_clamp_i64(
    timing->campaign_retry_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);

  const int64_t retry_max_upper = AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS;
  if (timing->campaign_retry_max_ms > retry_max_upper) timing->campaign_retry_max_ms = retry_max_upper;
  if (timing->campaign_retry_max_ms < timing->campaign_retry_ms) {
    timing->campaign_retry_max_ms = timing->campaign_retry_ms;
  }

  timing->campaign_retry_backoff_factor = consensus_clamp_i64(
    timing->campaign_retry_backoff_factor,
    AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MIN,
    AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX);
  timing->leader_heartbeat_ms = consensus_clamp_i64(
    timing->leader_heartbeat_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);

  if (timing->leader_lease_ms > AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS) {
    timing->leader_lease_ms = AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS;
  }
  if (timing->leader_lease_ms < timing->leader_heartbeat_ms) {
    timing->leader_lease_ms = timing->leader_heartbeat_ms;
  }

  timing->lease_expiry_recampaign_delay_ms = consensus_clamp_i64(
    timing->lease_expiry_recampaign_delay_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS);
  timing->stale_runtime_recovery_grace_ms = consensus_clamp_i64(
    timing->stale_runtime_recovery_grace_ms,
    0,
    AGENT_EDGE_CONSENSUS_POLICY_STALE_RUNTIME_RECOVERY_GRACE_MAX_MS);
  return AGENT_OK;
}

int64_t agent_edge_consensus_campaign_retry_delay_ms(
  int64_t campaign_retry_ms,
  int64_t campaign_retry_max_ms,
  int64_t campaign_retry_backoff_factor,
  uint64_t campaign_attempts
) {
  if (campaign_retry_ms <= 0 || campaign_attempts < 1) return 0;
  const int64_t factor = consensus_clamp_i64(
    campaign_retry_backoff_factor,
    AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MIN,
    AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX);
  int64_t delay = campaign_retry_ms;
  for (uint64_t i = 1; i < campaign_attempts; i++) {
    if (delay > INT64_MAX / factor) {
      delay = INT64_MAX;
      break;
    }
    delay *= factor;
  }
  const int64_t cap = campaign_retry_max_ms > 0 ? campaign_retry_max_ms : campaign_retry_ms;
  if (cap > 0 && delay > cap) delay = cap;
  return delay < 0 ? 0 : delay;
}

int agent_edge_consensus_leader_heartbeat_due(
  int64_t leader_heartbeat_ms,
  int64_t now_utc_ms,
  int64_t last_leader_heartbeat_sent_utc_ms,
  int leader_is_self,
  int has_committed_decision
) {
  if (leader_heartbeat_ms <= 0 || now_utc_ms <= 0) return 0;
  if (!leader_is_self || !has_committed_decision) return 0;
  if (last_leader_heartbeat_sent_utc_ms <= 0) return 1;
  return now_utc_ms - last_leader_heartbeat_sent_utc_ms >= leader_heartbeat_ms ? 1 : 0;
}

int agent_edge_consensus_leader_activity_can_observe(
  const char* kind,
  size_t kind_len,
  const char* leader_node_id,
  size_t leader_node_id_len,
  int64_t now_utc_ms
) {
  if (now_utc_ms <= 0) return 0;
  if (!consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT)) return 0;
  return agent_edge_consensus_member_node_id_is_valid(leader_node_id, leader_node_id_len);
}

int agent_edge_consensus_leader_lease_expired(
  int64_t leader_lease_ms,
  int64_t now_utc_ms,
  int64_t last_leader_contact_utc_ms,
  int has_leader,
  int leader_is_self
) {
  if (leader_lease_ms <= 0 || now_utc_ms <= 0) return 0;
  if (!has_leader || leader_is_self) return 0;
  if (last_leader_contact_utc_ms <= 0) return 0;
  return now_utc_ms - last_leader_contact_utc_ms >= leader_lease_ms ? 1 : 0;
}

int agent_edge_consensus_lease_expiry_recampaign_delay_active(
  int64_t lease_expiry_recampaign_delay_ms,
  int64_t now_utc_ms,
  int64_t last_leader_lease_expired_utc_ms
) {
  if (lease_expiry_recampaign_delay_ms <= 0 || now_utc_ms <= 0) return 0;
  if (last_leader_lease_expired_utc_ms <= 0) return 0;
  return now_utc_ms - last_leader_lease_expired_utc_ms < lease_expiry_recampaign_delay_ms ? 1 : 0;
}

int64_t agent_edge_consensus_campaign_last_started_after_lease_expiry(
  int64_t now_utc_ms,
  int64_t retry_delay_ms,
  int election_started,
  int64_t last_campaign_started_utc_ms
) {
  if (!election_started) return last_campaign_started_utc_ms;
  if (last_campaign_started_utc_ms <= 0) return last_campaign_started_utc_ms;
  if (now_utc_ms <= 0) return last_campaign_started_utc_ms;
  if (retry_delay_ms < 0) retry_delay_ms = 0;
  return now_utc_ms - retry_delay_ms;
}

int agent_edge_consensus_campaign_start_due(
  int64_t now_utc_ms,
  int64_t started_utc_ms,
  int64_t campaign_delay_ms,
  int election_started,
  int64_t last_campaign_started_utc_ms,
  int64_t retry_delay_ms
) {
  if (now_utc_ms <= 0) return 0;
  if (!election_started) {
    if (started_utc_ms <= 0) return campaign_delay_ms <= 0 ? 1 : 0;
    return now_utc_ms - started_utc_ms >= campaign_delay_ms ? 1 : 0;
  }
  if (retry_delay_ms <= 0) return 0;
  if (last_campaign_started_utc_ms <= 0) return 1;
  return now_utc_ms - last_campaign_started_utc_ms >= retry_delay_ms ? 1 : 0;
}

int agent_edge_consensus_campaign_can_start(
  int has_campaign_decision,
  int has_committed_decision,
  int campaign_start_due
) {
  return has_campaign_decision && !has_committed_decision && campaign_start_due ? 1 : 0;
}

uint64_t agent_edge_consensus_next_term(uint64_t current_term) {
  return current_term == UINT64_MAX ? UINT64_MAX : current_term + 1;
}

uint64_t agent_edge_consensus_next_frame_sequence(uint64_t current_frame_sequence) {
  return current_frame_sequence == UINT64_MAX ? UINT64_MAX : current_frame_sequence + 1;
}
