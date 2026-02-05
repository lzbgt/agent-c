#pragma once

#include <stddef.h>

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

#ifdef __cplusplus
}  // extern "C"
#endif

