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

// Majority quorum helper for embedded consensus replicas.
//
// The platform clamps cluster size to at least 1; the portable helper mirrors
// that behavior so firmware and agentd do not duplicate quorum math.
size_t agent_edge_consensus_quorum_for_cluster_size(size_t cluster_size);

#ifdef __cplusplus
}  // extern "C"
#endif
