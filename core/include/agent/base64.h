#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// Tiny base64 helpers (RFC 4648, standard alphabet, '=' padding).
//
// Intended for MCU/edge interop:
// - UM‑BMP envelope auth (`auth.sig` is base64 of 32 bytes (HMAC) or 64 bytes (Ed25519))
// - provisioning public keys via config endpoints (base64(pubkey32))
//
// Notes:
// - No allocations. Caller provides output buffers.
// - Encoding always includes '=' padding as required by RFC 4648.
// - Decoding accepts both padded and unpadded input (standard alphabet only).

// Returns the exact number of base64 characters (excluding NUL) produced by encoding `n` bytes.
size_t agent_base64_encode_len(size_t n);

// Encodes `in[0..in_len)` to base64.
// - `out_cap` must be at least `agent_base64_encode_len(in_len) + 1` (for NUL).
// - On success, `out` is NUL-terminated and `*out_len` excludes the NUL.
agent_status_t agent_base64_encode(
  const uint8_t* in,
  size_t in_len,
  char* out,
  size_t out_cap,
  size_t* out_len
);

// Decodes base64 `in[0..in_len)` to raw bytes.
// - Accepts standard base64 alphabet [A-Za-z0-9+/] and optional '=' padding.
// - Rejects whitespace and URL-safe alphabet ('-' '_').
// - On success, writes bytes into `out` and sets `*out_len`.
agent_status_t agent_base64_decode(
  const char* in,
  size_t in_len,
  uint8_t* out,
  size_t out_cap,
  size_t* out_len
);

#ifdef __cplusplus
}  // extern "C"
#endif

