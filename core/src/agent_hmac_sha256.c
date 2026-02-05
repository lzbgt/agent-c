#include "agent/hmac_sha256.h"

#include "agent_sha256.h"

#include <string.h>

void agent_hmac_sha256(
  const void* key,
  size_t key_len,
  const void* msg,
  size_t msg_len,
  uint8_t out32[32]
) {
  // HMAC-SHA256 (RFC 2104): SHA256((K ^ opad) || SHA256((K ^ ipad) || msg))
  // Block size: 64 bytes.
  uint8_t k0[64];
  memset(k0, 0, sizeof(k0));

  if (key && key_len > 0) {
    if (key_len > sizeof(k0)) {
      agent_sha256_ctx_t ctx;
      agent_sha256_init(&ctx);
      agent_sha256_update(&ctx, key, key_len);
      uint8_t kh[32];
      agent_sha256_final(&ctx, kh);
      memcpy(k0, kh, sizeof(kh));
    } else {
      memcpy(k0, key, key_len);
    }
  }

  uint8_t ipad[64];
  uint8_t opad[64];
  for (size_t i = 0; i < sizeof(k0); i++) {
    ipad[i] = (uint8_t)(k0[i] ^ 0x36);
    opad[i] = (uint8_t)(k0[i] ^ 0x5c);
  }

  uint8_t inner[32];
  {
    agent_sha256_ctx_t ctx;
    agent_sha256_init(&ctx);
    agent_sha256_update(&ctx, ipad, sizeof(ipad));
    if (msg && msg_len > 0) agent_sha256_update(&ctx, msg, msg_len);
    agent_sha256_final(&ctx, inner);
  }
  {
    agent_sha256_ctx_t ctx;
    agent_sha256_init(&ctx);
    agent_sha256_update(&ctx, opad, sizeof(opad));
    agent_sha256_update(&ctx, inner, sizeof(inner));
    agent_sha256_final(&ctx, out32);
  }
}

