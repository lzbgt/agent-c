#include "agent/ed25519.h"

#include <string.h>

// Public-domain Ed25519 implementation (vendored).
#include "ed25519.h"

void agent_ed25519_publickey(
  const uint8_t sk32[32],
  uint8_t out_pk32[32]
) {
  if (!sk32 || !out_pk32) return;
  ed25519_publickey(sk32, out_pk32);
}

void agent_ed25519_sign(
  const void* msg,
  size_t msg_len,
  const uint8_t sk32[32],
  const uint8_t pk32[32],
  uint8_t out_sig64[64]
) {
  if (!sk32 || !pk32 || !out_sig64) return;
  const unsigned char* m = (const unsigned char*)msg;
  const size_t n = msg ? msg_len : 0;
  ed25519_sign(m, n, sk32, pk32, out_sig64);
}

int agent_ed25519_verify(
  const void* msg,
  size_t msg_len,
  const uint8_t pk32[32],
  const uint8_t sig64[64]
) {
  if (!pk32 || !sig64) return 0;
  const unsigned char* m = (const unsigned char*)msg;
  const size_t n = msg ? msg_len : 0;
  const int ok = ed25519_sign_open(m, n, pk32, sig64);
  return ok == 0 ? 1 : 0;
}

