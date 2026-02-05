#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// agent_cbor_det_* provides a tiny deterministic CBOR (RFC 8949) encoder intended for
// MCU/edge interop, specifically to generate the exact signing bytes required by:
// - UM‑BMP envelope auth `auth.alg="hmac-sha256-cbor"`
// - UM‑BMP envelope auth `auth.alg="ed25519-cbor"`
//
// Determinism profile (matches agentd `daemon/src/cbor_encode.*`):
// - Definite-length items only.
// - Map keys are text strings and are ordered by:
//   1) UTF‑8 byte length
//   2) lexicographic order of UTF‑8 bytes
// - Integers are encoded in the minimal CBOR integer form.
// - Doubles, when used, are encoded as float64 (major 7, ai 27).
//
// Notes:
// - This is an encoder only; decoding and full JSON<->CBOR mapping are intentionally out of scope.
// - This module does not allocate; all output is written into a caller-provided buffer.

typedef struct agent_cbor_writer {
  uint8_t* buf;
  size_t cap;
  size_t len;
} agent_cbor_writer_t;

void agent_cbor_writer_init(agent_cbor_writer_t* w, uint8_t* buf, size_t cap);

// Returns a pointer to the written bytes and their length.
// The pointer is the original `buf` passed to init.
const uint8_t* agent_cbor_writer_bytes(const agent_cbor_writer_t* w);
size_t agent_cbor_writer_len(const agent_cbor_writer_t* w);

agent_status_t agent_cbor_write_uint(agent_cbor_writer_t* w, uint64_t v);
agent_status_t agent_cbor_write_int(agent_cbor_writer_t* w, int64_t v);
agent_status_t agent_cbor_write_bool(agent_cbor_writer_t* w, int v_true);
agent_status_t agent_cbor_write_null(agent_cbor_writer_t* w);
agent_status_t agent_cbor_write_text(agent_cbor_writer_t* w, const char* s, size_t n);
agent_status_t agent_cbor_write_bytes(agent_cbor_writer_t* w, const uint8_t* p, size_t n);

// Always encodes float64 (0xfb + big-endian 8 bytes).
agent_status_t agent_cbor_write_f64(agent_cbor_writer_t* w, double d);

// Writes a definite-length array/map header. Caller must then write exactly `n` items (array) or `2*n`
// items (map: key,value pairs).
agent_status_t agent_cbor_write_array_start(agent_cbor_writer_t* w, size_t n);
agent_status_t agent_cbor_write_map_start(agent_cbor_writer_t* w, size_t n_pairs);

typedef agent_status_t (*agent_cbor_encode_fn)(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_cbor_kv {
  const char* key;
  size_t key_len;
  agent_cbor_encode_fn encode_value;
  void* value_ctx;
} agent_cbor_kv_t;

// Encodes a CBOR map from key/value callbacks, sorting keys deterministically by the profile above.
// Returns AGENT_ERR_LIMIT if `n_pairs` is too large for the internal stack sorter.
agent_status_t agent_cbor_write_map_sorted(agent_cbor_writer_t* w, const agent_cbor_kv_t* pairs, size_t n_pairs);

#ifdef __cplusplus
}  // extern "C"
#endif

