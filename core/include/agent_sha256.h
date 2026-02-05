#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal SHA-256 implementation used for deterministic hashing surfaces.
//
// This is intentionally small and dependency-free so it can be used by:
// - agent_core (embedded / portable)
// - agent_host (CLI + host tools)
// - agentd (daemon)
//
// Output hex is lower-case.

typedef struct agent_sha256_ctx {
  uint32_t h[8];
  uint64_t total_len;
  uint8_t buf[64];
  size_t buf_len;
} agent_sha256_ctx_t;

void agent_sha256_init(agent_sha256_ctx_t* ctx);
void agent_sha256_update(agent_sha256_ctx_t* ctx, const void* data, size_t len);
void agent_sha256_final(agent_sha256_ctx_t* ctx, uint8_t out32[32]);

// Writes 64 hex chars + NUL terminator.
void agent_sha256_hex(const uint8_t hash32[32], char out65[65]);

// Convenience helper for one-shot hashing.
void agent_sha256_hex_of_bytes(const void* data, size_t len, char out65[65]);

#ifdef __cplusplus
}  // extern "C"
#endif

