#pragma once

#include <stddef.h>
#include <stdint.h>

#include "agent/agent.h"
#include "agent/cbor_det.h"

#ifdef __cplusplus
extern "C" {
#endif

// UM‑BMP envelope auth helpers intended for embedded nodes.
//
// This module exists to make MCU/gateway bring-up predictable:
// - it provides a canonical CBOR signing input builder that matches `agentd`
// - it provides tiny helpers to compute `auth.sig` in base64 for HMAC or Ed25519
//
// It intentionally does NOT define the full envelope schema; it only helps encode the
// `env_no_sig` bytes that are signed for `auth.alg="*-cbor"`.

typedef agent_status_t (*agent_umbmp_encode_fn)(agent_cbor_writer_t* w, void* ctx);

typedef struct agent_umbmp_envelope_cbor_params {
  const char* msg_id;
  size_t msg_id_len;

  int64_t ts_utc_ms;

  const char* type;
  size_t type_len;

  const char* from;
  size_t from_len;

  const char* to;
  size_t to_len;

  // Required: encodes `body` as a CBOR map/value (deterministic rules are the caller's job).
  agent_umbmp_encode_fn encode_body;
  void* body_ctx;

  // Optional: encodes `trace` object when present.
  agent_umbmp_encode_fn encode_trace;
  void* trace_ctx;

  // Optional: include `auth` metadata (alg/kid/seq), but NOT `auth.sig`.
  // This is the typical signing input shape for envelope auth.
  const char* auth_alg;
  size_t auth_alg_len;
  const char* auth_kid;
  size_t auth_kid_len;
  uint64_t auth_seq;
  int auth_has_seq;

  // Optional: include `auth.sig` as a CBOR text string (base64, RFC 4648).
  // When provided, `auth` is encoded as {alg,kid,seq?,sig}.
  const char* auth_sig_b64;
  size_t auth_sig_b64_len;
} agent_umbmp_envelope_cbor_params_t;

// Encodes the deterministic CBOR signing input (env with `auth.sig` omitted).
//
// The resulting bytes can be fed into:
// - agent_hmac_sha256 + base64 => `auth.sig` for `hmac-sha256-cbor`
// - agent_ed25519_sign + base64 => `auth.sig` for `ed25519-cbor`
agent_status_t agent_umbmp_envelope_no_sig_cbor_v0_4(
  const agent_umbmp_envelope_cbor_params_t* p,
  agent_cbor_writer_t* w
);

// Encodes a full UM‑BMP envelope as deterministic CBOR, including `auth.sig` when provided.
//
// This helper is intended for MCU/edge systems that want to use the CBOR wire profile (v0.1)
// while also attaching envelope auth metadata/signatures (v0.4 partial).
//
// Notes:
// - The signature verification signing input remains `agent_umbmp_envelope_no_sig_cbor_v0_4(...)`.
// - This function is about wire encoding for transport; it does not perform any signing itself.
agent_status_t agent_umbmp_envelope_cbor_v0_4(
  const agent_umbmp_envelope_cbor_params_t* p,
  agent_cbor_writer_t* w
);

// Computes base64 signature strings for `auth.sig`.
agent_status_t agent_umbmp_auth_hmac_sha256_cbor_sig_b64(
  const void* secret,
  size_t secret_len,
  const uint8_t* signing_input,
  size_t signing_input_len,
  char* out_b64,
  size_t out_cap,
  size_t* out_len
);

agent_status_t agent_umbmp_auth_ed25519_cbor_sig_b64(
  const uint8_t sk_seed32[32],
  const uint8_t pk32[32],
  const uint8_t* signing_input,
  size_t signing_input_len,
  char* out_b64,
  size_t out_cap,
  size_t* out_len
);

#ifdef __cplusplus
}  // extern "C"
#endif
