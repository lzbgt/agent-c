#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// UM‑EAIS task body decode helpers for embedded nodes (CBOR wire profile v0.1).
//
// This module focuses on parsing the `TASK_ASSIGN` body object, returning *views* into the
// caller-provided CBOR buffer (no allocations).
//
// Typical flow on a node:
// 1) Decode the UM‑BMP envelope (top-level) via `agent_umbmp_envelope_read_cbor_v0_1(...)`
// 2) If `env.type == "TASK_ASSIGN"`, decode `env.body_item` with:
//      `agent_um_eais_task_assign_body_read_cbor_v0_1(env.body_item.ptr, env.body_item.len, &out)`

typedef struct agent_um_eais_task_assign_view {
  // Optional: the intended node_id (v0.3 schema). Nodes can compare it to their own identity
  // as a defense-in-depth check.
  agent_cbor_text_view_t node_id;
  int has_node_id;

  agent_cbor_text_view_t task_id;
  agent_cbor_text_view_t step_id;
  agent_cbor_text_view_t idempotency_key;
  agent_cbor_text_view_t mode;  // "invoke" or "agent"
  uint64_t deadline_utc_ms;
  uint64_t attempt;
  int has_attempt;

  // Opaque CBOR bytes for the `payload` value (typically a map).
  agent_cbor_view_t payload_item;
} agent_um_eais_task_assign_view_t;

// Decodes the `TASK_ASSIGN.body` map.
//
// Required keys (v0.1 / platform implementation):
// - task_id, step_id, idempotency_key, mode, deadline_utc_ms, payload
// Optional keys:
// - attempt (uint)
agent_status_t agent_um_eais_task_assign_body_read_cbor_v0_1(
  const uint8_t* body_item,
  size_t body_item_len,
  agent_um_eais_task_assign_view_t* out
);

#ifdef __cplusplus
}  // extern "C"
#endif
