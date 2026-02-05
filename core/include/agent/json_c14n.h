#pragma once

#include <stddef.h>

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// agent_json_c14n_* provides a deterministic canonical JSON form intended for
// portable hashing / attestation across heterogeneous nodes (MCU, desktop, etc).
//
// Canonicalization (agent_json_c14n_v1):
// - Objects: keys sorted lexicographically by UTF-8 bytes; no whitespace.
// - Strings: parsed/unescaped then re-escaped deterministically.
// - Numbers: normalized to a plain decimal form (no exponent) with trailing
//   fraction zeros removed (exact base-10 shift; no float rounding).
// - Arrays: order preserved.
//
// Notes:
// - Input must be valid JSON (a single value).
// - Output is UTF-8 JSON without a trailing newline.

// Canonicalizes `json[0..json_len)` into a newly allocated UTF-8 JSON string.
// The returned buffer is NUL-terminated for convenience, and `*out_len` excludes
// the terminator.
// Caller owns the buffer and must free it via `agent_free`.
agent_status_t agent_json_c14n_canonicalize(
  const char* json,
  size_t json_len,
  char** out_json,
  size_t* out_len,
  char* err_buf,
  size_t err_cap
);

// Computes a sha256 token over the canonicalized bytes (agent_json_c14n_v1).
// `out_token` must have room for at least 80 bytes.
// On success: writes "sha256:" + 64 lowercase hex chars + NUL.
agent_status_t agent_json_c14n_sha256_token(
  const char* json,
  size_t json_len,
  char out_token[80],
  char* err_buf,
  size_t err_cap
);

#ifdef __cplusplus
}  // extern "C"
#endif

