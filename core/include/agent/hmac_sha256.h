#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Portable HMAC-SHA256 helper built on agent_sha256.
//
// This is intentionally small and dependency-free so it can be used by:
// - agent_core (embedded / portable)
// - agentd (daemon)
//
// Output is 32 raw bytes.
void agent_hmac_sha256(
  const void* key,
  size_t key_len,
  const void* msg,
  size_t msg_len,
  uint8_t out32[32]
);

#ifdef __cplusplus
}  // extern "C"
#endif

