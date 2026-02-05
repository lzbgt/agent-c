#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// UM‑BMP envelope CBOR decoder helpers for embedded nodes and gateways.
//
// These helpers intentionally return *views* into the input buffer (no allocations).
// The caller owns the input buffer lifetime.
//
// This module decodes the UM‑EAIS / UM‑BMP CBOR wire profile v0.1:
// - top-level is a definite-length map
// - map keys are text strings
// - values are JSON-shaped (strings, numbers, nested objects)
//
// It does NOT attempt a full JSON<->CBOR mapping layer; it only extracts common
// envelope metadata and returns `body` as an opaque CBOR slice for message-type-specific code.

typedef struct agent_umbmp_auth_view {
  agent_cbor_text_view_t alg;   // required when auth is present
  agent_cbor_text_view_t kid;   // required when auth is present
  uint64_t seq;                // optional
  int has_seq;
  agent_cbor_text_view_t sig;  // optional (base64 text for v0.1 profile)
  int has_sig;
} agent_umbmp_auth_view_t;

typedef struct agent_umbmp_trace_view {
  agent_cbor_text_view_t trace_id; // optional
  int has_trace_id;
} agent_umbmp_trace_view_t;

typedef struct agent_umbmp_envelope_view {
  agent_cbor_text_view_t msg_id;
  agent_cbor_text_view_t type;
  agent_cbor_text_view_t from;
  agent_cbor_text_view_t to;
  uint64_t ts_utc_ms;

  // Optional nested objects.
  agent_umbmp_trace_view_t trace;
  int has_trace;

  agent_umbmp_auth_view_t auth;
  int has_auth;

  // Opaque CBOR bytes for the `body` value (typically a map).
  // The slice includes the CBOR header byte for the item.
  agent_cbor_view_t body_item;
  int has_body;
} agent_umbmp_envelope_view_t;

// Decodes a CBOR-encoded UM‑BMP envelope (wire profile v0.1) into a view struct.
//
// Returns:
// - AGENT_OK on success
// - AGENT_ERR_INVALID_ARGUMENT for malformed CBOR/profile violations
// - AGENT_ERR_BOUNDS when the buffer is truncated
// - AGENT_ERR_LIMIT for nesting/size limit violations
//
// Notes:
// - Required fields: msg_id, ts_utc_ms, type, from, to, body.
// - Unknown keys are ignored (forward-compatible).
agent_status_t agent_umbmp_envelope_read_cbor_v0_1(
  const uint8_t* buf,
  size_t len,
  agent_umbmp_envelope_view_t* out_env
);

#ifdef __cplusplus
}  // extern "C"
#endif

