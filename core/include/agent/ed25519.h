#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Portable Ed25519 helpers (sign + verify).
//
// Notes:
// - The secret key is the 32-byte Ed25519 seed (RFC 8032 style).
// - The public key is 32 bytes.
// - The signature is 64 bytes.
//
// This API is intentionally small so it can be used by:
// - embedded agent_core nodes (sign UM-BMP envelopes)
// - agentd (verify envelope auth when using public-key identities)

void agent_ed25519_publickey(
  const uint8_t sk32[32],
  uint8_t out_pk32[32]
);

void agent_ed25519_sign(
  const void* msg,
  size_t msg_len,
  const uint8_t sk32[32],
  const uint8_t pk32[32],
  uint8_t out_sig64[64]
);

// Returns 1 if signature verifies, 0 otherwise.
int agent_ed25519_verify(
  const void* msg,
  size_t msg_len,
  const uint8_t pk32[32],
  const uint8_t sig64[64]
);

#ifdef __cplusplus
}  // extern "C"
#endif

