#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_read.h"
#include "agent/umbmp_envelope_read.h"

#ifdef __cplusplus
extern "C" {
#endif

// Decoder for agentd `/api/v1/edge/outbox` response when `Accept: application/cbor`.
//
// This is NOT a UM‑BMP message; it is a platform API response that contains a list of UM‑BMP
// envelopes under `messages[].msg`.
//
// Goals for MCU/gateway bring-up:
// - decode without JSONCPP / dynamic allocations
// - surface envelopes as `agent_umbmp_envelope_view_t` (views into the input buffer)
//
// Limitations:
// - Only handles the "happy path" where each row contains `msg` (decoded envelope object).
//   Rows with only `msg_raw` are skipped (forward-compatible / robust).

typedef struct agent_um_eais_outbox_row_view {
  uint64_t outbox_id;
  uint64_t ts_utc_ms;
  int has_ts_utc_ms;

  // Raw CBOR bytes for the envelope item stored under row["msg"].
  agent_cbor_view_t msg_item;

  // Parsed view into msg_item.
  agent_umbmp_envelope_view_t msg;
  int has_msg;
} agent_um_eais_outbox_row_view_t;

typedef struct agent_um_eais_outbox_view {
  int ok;
  agent_cbor_text_view_t node_id;
  uint64_t cursor_base;
  uint64_t cursor_next;

  size_t messages_total;   // number of rows in the CBOR array
  size_t messages_parsed;  // number of rows decoded into `out_rows`
  int truncated;           // 1 if more rows existed than the caller buffer
} agent_um_eais_outbox_view_t;

// Decodes the outbox response and fills up to `rows_cap` items into `out_rows`.
//
// On success:
// - returns AGENT_OK
// - `out->messages_parsed <= rows_cap`
// - `out->truncated` indicates whether more rows were present
agent_status_t agent_um_eais_outbox_read_cbor_v0_1(
  const uint8_t* buf,
  size_t len,
  agent_um_eais_outbox_view_t* out,
  agent_um_eais_outbox_row_view_t* out_rows,
  size_t rows_cap
);

#ifdef __cplusplus
}  // extern "C"
#endif

