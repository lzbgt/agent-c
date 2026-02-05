#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_det.h"
#include "agent/cbor_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// Deterministic CBOR body encoder for UM‑EAIS `NODE_CAPS_RSP`.
//
// Motivation:
// - `NODE_CAPS_RSP.body.manifest` can be large and schema-driven (UM‑ACDS).
// - Many MCU projects already have a full CBOR library; they can encode the manifest using their
//   existing stack, then let `agent_core` wrap it into a correct UM‑EAIS body map with deterministic
//   key ordering.
//
// This module intentionally does NOT implement "JSON schema -> CBOR" for the manifest. Instead it:
// - accepts a caller-provided CBOR item for `manifest`
// - validates it is a definite-length CBOR map with text keys
// - (optionally) validates it is deterministically ordered (len, then bytes) and has no duplicate keys
//
// Typical usage (MCU):
// - Encode `manifest` with your existing CBOR library into `manifest_cbor` bytes.
// - Call `agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1(...)` as the envelope's `encode_body`.

typedef struct agent_um_eais_node_caps_rsp_body {
  agent_cbor_text_view_t node_id;

  // CBOR-encoded manifest item (must be a CBOR map item).
  // The bytes are copied verbatim into the output body.
  agent_cbor_view_t manifest_cbor;

  // When non-zero, validates that the manifest map's text keys are ordered deterministically and
  // contain no duplicates. This is recommended when using `auth.alg="*-cbor"` signatures because the
  // signing input is wire bytes.
  int enforce_deterministic_keys;
} agent_um_eais_node_caps_rsp_body_t;

agent_status_t agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

