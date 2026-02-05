#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// agent_cbor_read_* provides a tiny CBOR (RFC 8949) reader intended for MCU/edge interop.
//
// Scope / profile:
// - Definite-length items only (indefinite-length arrays/maps/bytes/text are rejected).
// - No allocations; returned text/bytes are views into the caller-provided input buffer.
// - Intended to decode the same wire profile used by UM‑EAIS/UM‑BMP CBOR envelopes.
//
// Supported CBOR major types:
// - 0,1: unsigned / negative integers
// - 2,3: bytes / text
// - 4,5: arrays / maps (definite length)
// - 6: tags (skip only; tag values are ignored)
// - 7: booleans, null, and float64 (float16/float32 are rejected by agent_cbor_read_f64)
//
// Safety limits:
// - max nesting depth (default: 16)
// - max container items (default: 4096) for arrays and maps

typedef struct agent_cbor_view {
  const uint8_t* ptr;
  size_t len;
} agent_cbor_view_t;

typedef struct agent_cbor_text_view {
  const char* ptr;  // UTF‑8 bytes; not NUL-terminated
  size_t len;       // bytes
} agent_cbor_text_view_t;

typedef struct agent_cbor_reader {
  const uint8_t* buf;
  size_t len;
  size_t off;
  uint8_t depth;
  uint8_t max_depth;
  size_t max_container_items;
} agent_cbor_reader_t;

void agent_cbor_reader_init(agent_cbor_reader_t* r, const uint8_t* buf, size_t len);

// Optional: override limits after init.
void agent_cbor_reader_set_max_depth(agent_cbor_reader_t* r, uint8_t max_depth);
void agent_cbor_reader_set_max_container_items(agent_cbor_reader_t* r, size_t max_items);

// Current read offset (bytes consumed since init).
size_t agent_cbor_reader_offset(const agent_cbor_reader_t* r);

agent_status_t agent_cbor_read_uint(agent_cbor_reader_t* r, uint64_t* out_v);
agent_status_t agent_cbor_read_int(agent_cbor_reader_t* r, int64_t* out_v);
agent_status_t agent_cbor_read_bool(agent_cbor_reader_t* r, int* out_true);
agent_status_t agent_cbor_read_null(agent_cbor_reader_t* r);

agent_status_t agent_cbor_read_bytes(agent_cbor_reader_t* r, agent_cbor_view_t* out_view);
agent_status_t agent_cbor_read_text(agent_cbor_reader_t* r, agent_cbor_text_view_t* out_view);

// Reads a float64 (0xfb + 8 bytes). Other float widths are rejected.
agent_status_t agent_cbor_read_f64(agent_cbor_reader_t* r, double* out_v);

agent_status_t agent_cbor_read_array_start(agent_cbor_reader_t* r, size_t* out_n_items);
agent_status_t agent_cbor_read_map_start(agent_cbor_reader_t* r, size_t* out_n_pairs);

// Skips the next CBOR item (including nested arrays/maps/tags).
agent_status_t agent_cbor_skip_item(agent_cbor_reader_t* r);

#ifdef __cplusplus
}  // extern "C"
#endif

