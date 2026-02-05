#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_det.h"
#include "agent/cbor_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tiny deterministic CBOR encoders for common UM‑EAIS task lifecycle bodies.
//
// Intended use:
// - MCU nodes can encode TASK_ACK / TASK_EVENT / TASK_FAILED bodies without pulling in a full
//   JSON<->CBOR mapping layer.
// - Callers typically pass these encoders as `encode_body` to:
//     `agent_umbmp_envelope_no_sig_cbor_v0_4(...)` / `agent_umbmp_envelope_cbor_v0_4(...)`.
//
// Determinism:
// - Uses `agent_cbor_write_map_sorted(...)` so map key order matches the platform profile used for
//   envelope signatures (length, then lexicographic bytes).

typedef struct agent_um_eais_task_ack_body {
  agent_cbor_text_view_t task_id;
  agent_cbor_text_view_t step_id;
  agent_cbor_text_view_t idempotency_key;
  int accepted;  // boolean

  agent_cbor_text_view_t reason;  // optional
  int has_reason;
} agent_um_eais_task_ack_body_t;

agent_status_t agent_um_eais_task_ack_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_um_eais_task_event_body {
  agent_cbor_text_view_t task_id;
  agent_cbor_text_view_t step_id;
  agent_cbor_text_view_t idempotency_key;

  agent_cbor_text_view_t state;  // required (e.g. "running", "done", "failed")

  double progress;  // optional
  int has_progress;

  agent_cbor_text_view_t error;  // optional
  int has_error;
} agent_um_eais_task_event_body_t;

agent_status_t agent_um_eais_task_event_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_um_eais_task_failed_body {
  agent_cbor_text_view_t task_id;
  agent_cbor_text_view_t step_id;
  agent_cbor_text_view_t idempotency_key;
  agent_cbor_text_view_t error;
} agent_um_eais_task_failed_body_t;

agent_status_t agent_um_eais_task_failed_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

