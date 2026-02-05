#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// UM‑EAIS PLATFORM_CAPS_REQ body decoder (platform → node), CBOR wire profile v0.1.
//
// Decodes `body` for message type "PLATFORM_CAPS_REQ":
//   { "node_id": <id_token>, "want": "full"|"hash" }
//
// This returns views into the caller-provided CBOR bytes (no allocations).

typedef struct agent_um_eais_platform_caps_req_view {
  agent_cbor_text_view_t node_id;
  agent_cbor_text_view_t want; // "full" or "hash"
} agent_um_eais_platform_caps_req_view_t;

agent_status_t agent_um_eais_platform_caps_req_body_read_cbor_v0_1(
  const uint8_t* body_item,
  size_t body_item_len,
  agent_um_eais_platform_caps_req_view_t* out
);

#ifdef __cplusplus
}  // extern "C"
#endif

