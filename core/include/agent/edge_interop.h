#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// UM‑BMP / UM‑EAIS interop helpers intended for embedded `agent_core` bring-up.
//
// Goals:
// - share an "id-safe" character set across MCU nodes and the platform (`agentd`)
// - provide a deterministic token sanitizer suitable for generating workflow_id / idempotency keys
// - centralize message type string constants so firmware doesn't sprinkle raw literals

// Max ID length used across the platform for workflow_id/trace_id/idempotency_key/msg_id-like tokens.
// (Matches platform validators.)
#define AGENT_UM_BMP_MAX_ID_LEN 128

// Allowed id token character set (ASCII):
//   [A-Za-z0-9-_.:]
int agent_umbmp_id_is_safe(const char* s, size_t len);

// Allowed trace_id token character set (ASCII) for envelope correlation:
//   [A-Za-z0-9-_.:@]
//
// This matches the platform's trace_id validator and UM‑EAIS v0.2+ docs.
int agent_umbmp_trace_id_is_safe(const char* s, size_t len);

// Best-effort validation for sha256 tokens used in manifests and heartbeats.
//
// Accepted forms:
// - 64 hex chars (lower/upper)
// - "sha256:" + 64 hex chars
int agent_umbmp_sha256_token_is_safe(const char* s, size_t len);

// Sanitizes an arbitrary token into an id-safe token by:
// - truncating to `max_len` (clamped to out buffer capacity)
// - mapping disallowed characters to '_'
// - trimming leading/trailing '_' runs
// - ensuring a non-empty output (uses "msg" when empty after sanitization)
//
// Output is always NUL-terminated.
agent_status_t agent_umbmp_sanitize_id_token(
  const char* in,
  size_t in_len,
  char* out,
  size_t out_cap,
  size_t max_len,
  size_t* out_len
);

// Common UM‑EAIS/UM‑BMP message types used by embedded nodes.
// (Not exhaustive; add as needed.)
#define AGENT_UM_BMP_TYPE_NODE_HELLO "NODE_HELLO"
#define AGENT_UM_BMP_TYPE_NODE_HEARTBEAT "NODE_HEARTBEAT"
#define AGENT_UM_BMP_TYPE_NODE_CAPS_RSP "NODE_CAPS_RSP"
#define AGENT_UM_BMP_TYPE_PLATFORM_CAPS_REQ "PLATFORM_CAPS_REQ"

// Node-native consensus messages and signed platform policy bundles.
#define AGENT_UM_BMP_TYPE_CONSENSUS_FRAME "CONSENSUS_FRAME"
#define AGENT_UM_BMP_TYPE_PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE "PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE"

// Consensus schemas shared by embedded nodes and agentd.
#define AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1 "edge_node_consensus_frame_v1"
#define AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1 "edge_consensus_membership_v1"
#define AGENT_EDGE_CONSENSUS_MEMBERSHIP_ATTEST_SCHEMA_V1 "edge_consensus_membership_attest_v1"

// Consensus frame kinds.
#define AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST "vote_request"
#define AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT "vote_grant"
#define AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT "leader_commit"

// Task lifecycle messages.
#define AGENT_UM_BMP_TYPE_TASK_ASSIGN "TASK_ASSIGN"
#define AGENT_UM_BMP_TYPE_TASK_ACK "TASK_ACK"
#define AGENT_UM_BMP_TYPE_TASK_EVENT "TASK_EVENT"
#define AGENT_UM_BMP_TYPE_TASK_DONE "TASK_DONE"
#define AGENT_UM_BMP_TYPE_TASK_FAILED "TASK_FAILED"

// Sensor/event messages.
#define AGENT_UM_BMP_TYPE_SENSOR_EVENT "SENSOR_EVENT"

// Platform extensions: node → platform workflow handoff.
#define AGENT_UM_BMP_TYPE_WORKFLOW_SUBMIT "WORKFLOW_SUBMIT"
#define AGENT_UM_BMP_TYPE_WORKFLOW_CANCEL "WORKFLOW_CANCEL"
#define AGENT_UM_BMP_TYPE_WORKFLOW_ACK "WORKFLOW_ACK"

// Durable workflow handoff over UM‑BMP ingress (platform durable workflow engine).
#define AGENT_UM_BMP_TYPE_DURABLE_WORKFLOW_SUBMIT "DURABLE_WORKFLOW_SUBMIT"
#define AGENT_UM_BMP_TYPE_DURABLE_WORKFLOW_CANCEL "DURABLE_WORKFLOW_CANCEL"
#define AGENT_UM_BMP_TYPE_DURABLE_WORKFLOW_ACK "DURABLE_WORKFLOW_ACK"

// Conventional prefixes used by the platform when defaulting ids from msg_id.
#define AGENT_UM_BMP_WORKFLOW_ID_PREFIX "wf:"
#define AGENT_UM_BMP_IDEMPOTENCY_PREFIX "edge_msg:"
#define AGENT_UM_BMP_IDEMPOTENCY_WORKFLOW_PREFIX "edge_wf:"

// Portable signing input for UM‑EAIS result attestation signatures.
//
// Format (bytes, UTF-8; trailing newline included):
//   UM_EAIS_RESULT_ATTEST_v0_1\n
//   <task_id>\n
//   <step_id>\n
//   <idempotency_key>\n
//   <result_sha256_token>\n
//   <ts_utc_ms>\n
//
// Notes:
// - The id fields must pass `agent_umbmp_id_is_safe`.
// - `result_sha256_token` must pass `agent_umbmp_sha256_token_is_safe`.
// - This helper exists so MCU firmware and the platform can sign the exact same bytes without
//   duplicating string formatting logic.
#define AGENT_UM_EAIS_RESULT_ATTEST_SIGNING_PREFIX "UM_EAIS_RESULT_ATTEST_v0_1\n"

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
);

// Majority quorum and cluster-size helpers for embedded consensus replicas.
//
// The platform clamps cluster size to at least 1; the portable helper mirrors
// that behavior so firmware and agentd do not duplicate quorum math.
size_t agent_edge_consensus_cluster_size_normalize(size_t cluster_size);

size_t agent_edge_consensus_cluster_size_from_peer_count(size_t peer_count);

size_t agent_edge_consensus_cluster_size_from_member_count(size_t member_count);

size_t agent_edge_consensus_quorum_for_cluster_size(size_t cluster_size);

int agent_edge_consensus_has_quorum(size_t cluster_size, size_t vote_count);

int agent_edge_consensus_frame_kind_is_valid(const char* kind, size_t kind_len);

typedef enum agent_edge_consensus_frame_route_t {
  AGENT_EDGE_CONSENSUS_FRAME_ROUTE_DROP = 0,
  AGENT_EDGE_CONSENSUS_FRAME_ROUTE_CANDIDATE = 1,
  AGENT_EDGE_CONSENSUS_FRAME_ROUTE_PEERS = 2
} agent_edge_consensus_frame_route_t;

typedef enum agent_edge_consensus_identity_validation_t {
  AGENT_EDGE_CONSENSUS_IDENTITY_OK = 0,
  AGENT_EDGE_CONSENSUS_IDENTITY_CLUSTER_ID_INVALID = 1,
  AGENT_EDGE_CONSENSUS_IDENTITY_NODE_ID_INVALID = 2,
  AGENT_EDGE_CONSENSUS_IDENTITY_MANIFEST_SHA256_INVALID = 3
} agent_edge_consensus_identity_validation_t;

typedef enum agent_edge_consensus_frame_validation_t {
  AGENT_EDGE_CONSENSUS_FRAME_OK = 0,
  AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_INVALID = 1,
  AGENT_EDGE_CONSENSUS_FRAME_KIND_INVALID = 2,
  AGENT_EDGE_CONSENSUS_FRAME_ID_INVALID = 3,
  AGENT_EDGE_CONSENSUS_FRAME_TERM_INVALID = 4,
  AGENT_EDGE_CONSENSUS_FRAME_DECISION_SHA256_INVALID = 5,
  AGENT_EDGE_CONSENSUS_FRAME_FROM_CLUSTER_ID_INVALID = 6,
  AGENT_EDGE_CONSENSUS_FRAME_FROM_NODE_ID_INVALID = 7,
  AGENT_EDGE_CONSENSUS_FRAME_FROM_MANIFEST_SHA256_INVALID = 8,
  AGENT_EDGE_CONSENSUS_FRAME_CANDIDATE_NODE_ID_INVALID = 9,
  AGENT_EDGE_CONSENSUS_FRAME_LEADER_NODE_ID_INVALID = 10
} agent_edge_consensus_frame_validation_t;

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
);

agent_edge_consensus_identity_validation_t agent_edge_consensus_identity_validate(
  const char* cluster_id,
  size_t cluster_id_len,
  const char* node_id,
  size_t node_id_len,
  const char* manifest_sha256,
  size_t manifest_sha256_len
);

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
);

int agent_edge_consensus_identity_membership_matches(
  uint64_t local_membership_epoch,
  uint64_t identity_membership_epoch,
  int identity_node_is_member
);

int agent_edge_consensus_cluster_id_matches(
  const char* local_cluster_id,
  size_t local_cluster_id_len,
  const char* peer_cluster_id,
  size_t peer_cluster_id_len
);

int agent_edge_consensus_trust_epochs_match(
  uint64_t local_trust_roots_epoch,
  uint64_t local_revocations_epoch,
  uint64_t local_cert_roots_epoch,
  uint64_t peer_trust_roots_epoch,
  uint64_t peer_revocations_epoch,
  uint64_t peer_cert_roots_epoch
);

int agent_edge_consensus_decision_sha256_matches(
  const char* local_decision_sha256,
  size_t local_decision_sha256_len,
  const char* peer_decision_sha256,
  size_t peer_decision_sha256_len
);

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
);

int agent_edge_consensus_vote_grant_can_count(
  uint64_t current_term,
  uint64_t grant_term,
  int candidate_node_is_member,
  int candidate_is_self,
  int grant_sender_is_candidate,
  int campaign_decision_matches,
  int granted,
  int trust_epochs_match
);

int agent_edge_consensus_leader_commit_can_accept(
  uint64_t current_term,
  uint64_t commit_term,
  int leader_node_is_member,
  int leader_is_sender,
  int trust_epochs_match
);

int agent_edge_consensus_leader_commit_witnesses_can_accept(
  size_t cluster_size,
  size_t valid_witness_count,
  int leader_is_witness
);

size_t agent_edge_consensus_vote_count_with_self(size_t grant_witness_count);

int agent_edge_consensus_candidate_can_commit(
  int has_leader,
  int has_quorum
);

int agent_edge_consensus_incoming_term_advances(
  uint64_t current_term,
  uint64_t incoming_term
);

int agent_edge_consensus_incoming_term_is_stale(
  uint64_t current_term,
  uint64_t incoming_term
);

int agent_edge_consensus_seen_frame_should_drop(
  int frame_id_seen,
  uint64_t seen_term,
  uint64_t frame_term
);

agent_edge_consensus_frame_route_t agent_edge_consensus_frame_route(
  const char* kind,
  size_t kind_len,
  int candidate_node_id_is_valid,
  int candidate_is_self
);

// Consensus membership validation helpers shared by firmware and agentd.
#define AGENT_EDGE_CONSENSUS_MEMBERSHIP_LINEAGE_MAX 8

int agent_edge_consensus_member_node_id_is_valid(const char* node_id, size_t node_id_len);

int agent_edge_consensus_membership_epoch_can_advance(
  uint64_t current_epoch,
  uint64_t next_epoch
);

int agent_edge_consensus_membership_policy_can_adopt(
  uint64_t current_epoch,
  uint64_t next_epoch,
  int self_node_is_member
);

typedef enum agent_edge_consensus_membership_policy_header_validation_t {
  AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_HEADER_OK = 0,
  AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_SCHEMA_INVALID = 1,
  AGENT_EDGE_CONSENSUS_MEMBERSHIP_POLICY_CLUSTER_ID_INVALID = 2
} agent_edge_consensus_membership_policy_header_validation_t;

agent_edge_consensus_membership_policy_header_validation_t
agent_edge_consensus_membership_policy_header_validate(
  const char* schema,
  size_t schema_len,
  const char* cluster_id,
  size_t cluster_id_len
);

int agent_edge_consensus_membership_member_set_is_nonempty(size_t member_count);

int agent_edge_consensus_membership_lineage_is_valid(
  uint64_t previous_epoch,
  uint64_t current_epoch
);

int agent_edge_consensus_membership_epoch_is_recoverable(
  uint64_t runtime_epoch,
  uint64_t current_epoch,
  uint64_t previous_epoch,
  const uint64_t* lineage_epochs,
  size_t lineage_len
);

// Durable consensus policy timing bounds shared by embedded firmware and agentd.
#define AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS 120000
#define AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS 300000
#define AGENT_EDGE_CONSENSUS_POLICY_STALE_RUNTIME_RECOVERY_GRACE_MAX_MS 86400000
#define AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MIN 1
#define AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX 8

typedef struct agent_edge_consensus_policy_timing_t {
  int64_t campaign_delay_ms;
  int64_t campaign_retry_ms;
  int64_t campaign_retry_max_ms;
  int64_t campaign_retry_backoff_factor;
  int64_t leader_heartbeat_ms;
  int64_t leader_lease_ms;
  int64_t lease_expiry_recampaign_delay_ms;
  int64_t stale_runtime_recovery_grace_ms;
} agent_edge_consensus_policy_timing_t;

agent_status_t agent_edge_consensus_policy_timing_normalize(
  agent_edge_consensus_policy_timing_t* timing
);

int64_t agent_edge_consensus_campaign_retry_delay_ms(
  int64_t campaign_retry_ms,
  int64_t campaign_retry_max_ms,
  int64_t campaign_retry_backoff_factor,
  uint64_t campaign_attempts
);

int agent_edge_consensus_leader_heartbeat_due(
  int64_t leader_heartbeat_ms,
  int64_t now_utc_ms,
  int64_t last_leader_heartbeat_sent_utc_ms,
  int leader_is_self,
  int has_committed_decision
);

int agent_edge_consensus_leader_activity_can_observe(
  const char* kind,
  size_t kind_len,
  const char* leader_node_id,
  size_t leader_node_id_len,
  int64_t now_utc_ms
);

int agent_edge_consensus_leader_lease_expired(
  int64_t leader_lease_ms,
  int64_t now_utc_ms,
  int64_t last_leader_contact_utc_ms,
  int has_leader,
  int leader_is_self
);

int agent_edge_consensus_lease_expiry_recampaign_delay_active(
  int64_t lease_expiry_recampaign_delay_ms,
  int64_t now_utc_ms,
  int64_t last_leader_lease_expired_utc_ms
);

int agent_edge_consensus_campaign_start_due(
  int64_t now_utc_ms,
  int64_t started_utc_ms,
  int64_t campaign_delay_ms,
  int election_started,
  int64_t last_campaign_started_utc_ms,
  int64_t retry_delay_ms
);

#ifdef __cplusplus
}  // extern "C"
#endif
