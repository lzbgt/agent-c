# UM‑BMP Envelope Auth — Profile v0.4

Date: 2026-02-05
Status: implemented rolling core; PKI/confidentiality and node-native manifest distribution still open

This document defines an optional envelope authenticity mechanism for UM‑BMP messages
in constrained IoT/edge systems, intended for:
- MQTT/LoRa gateways (transport bridges where payload authenticity matters)
- MCU nodes (`agent_core`) where TLS mutual auth may not be available end-to-end

This profile is transport-agnostic and applies to both JSON and CBOR wire mappings.

## Goals

- Provide **enforceable authenticity** of the full UM‑BMP envelope at the platform ingress.
- Keep the signing algorithm implementable on MCUs (small code, no big deps).
- Bind node identity (`from: "node:<node_id>"`) to the authenticated envelope when enforcement is enabled.

## Additional goals

- Confidentiality (payload encryption in addition to transport security when available).
- Certificate chains / PKI with rotation and revocation support.
- Durable PKI / trust-root rotation and node-native signed manifest / identity distribution so trust roots do not rely
  only on operator-provisioned key maps.

## Envelope fields

When present, the envelope includes:

```json
{
  "auth": {
    "alg": "hmac-sha256",
    "kid": "k0",
    "seq": 123,
    "sig": "<base64-of-32-bytes>"
  }
}
```

Semantics:
- `auth.alg` MUST be one of:
  - `"hmac-sha256"` (signing input is canonical JSON bytes),
  - `"hmac-sha256-cbor"` (signing input is deterministic CBOR bytes),
  - `"ed25519"` (signing input is canonical JSON bytes),
  - `"ed25519-cbor"` (signing input is deterministic CBOR bytes).
- `auth.kid` selects key material provisioned on both node/gateway and platform:
  - for HMAC: shared secret
  - for Ed25519: public key (platform) / private key seed (node)
- `auth.seq` is an optional monotonic sequence number (recommended when clocks are unreliable).
- `auth.sig` is base64 (RFC 4648 standard alphabet) of:
  - 32 bytes for HMAC-SHA256
  - 64 bytes for Ed25519 signatures

## Signing input

Compute the signature over the envelope with the `auth.sig` field removed (auth metadata remains signed).

For `auth.alg="hmac-sha256"` (canonical JSON):

Canonicalization algorithm: `agent_json_c14n_v1`:
- object keys sorted lexicographically (UTF‑8 bytes)
- no whitespace
- number normalization (no exponent, trailing zeros removed)

Pseudocode:

1) `env_no_sig = env` with `auth.sig` removed
2) `canon = json_c14n(env_no_sig)`
3) `sig = base64(HMAC_SHA256(secret_for(kid), canon))`

For `auth.alg="hmac-sha256-cbor"` (deterministic CBOR):

Signing input is the deterministic CBOR encoding of `env_no_sig`:
- CBOR (RFC 8949) definite lengths only (no indefinite streaming items)
- JSON objects encoded as CBOR maps with deterministically ordered UTF‑8 text-string keys:
  - sort by UTF‑8 byte length
  - then lexicographically by UTF‑8 bytes
- integers encoded in the minimal CBOR integer form
- floats MAY be used, but are discouraged for MCU simplicity:
  - when present, the platform deterministic CBOR encoder uses **float64**
  - numeric types MUST be preserved for signing:
    - a CBOR float decoded to JSON must remain a CBOR float in the signing input,
      even if it is numerically integral (e.g. `87.0` must not be re-encoded as integer `87`)

Pseudocode:

1) `env_no_sig = env` with `auth.sig` removed
2) `canon = cbor_canonical(env_no_sig)` (see above)
3) `sig = base64(HMAC_SHA256(secret_for(kid), canon))`

For `auth.alg="ed25519"` and `auth.alg="ed25519-cbor"`:

- Signing input is computed exactly as above (canonical JSON or deterministic CBOR of `env_no_sig`).
- `sig = base64(Ed25519_SIGN(sk_for(kid), signing_input))`
- Verification is `Ed25519_VERIFY(pk_for(kid), signing_input, sig)`

## Test vectors (recommended for MCU bring-up)

This repo includes an executable set of signing/canonicalization vectors:

- Fixture: `docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json`
  - includes `canon_json_hex` and `canon_cbor_hex` for a minimal `NODE_HELLO` envelope
  - includes expected signatures for:
    - HMAC (`hmac-sha256`, `hmac-sha256-cbor`)
    - Ed25519 (`ed25519`, `ed25519-cbor`)
  - intended use: ensure your MCU CBOR encoder (e.g. TinyCBOR-derived) emits exactly the same canonical bytes
    before you chase “why does signature verification fail”.
- Generator tool (host-only): `umbmp_auth_vectors_tool` (writes the JSON fixture to stdout).
- Proof: `ctest` includes `umbmp_auth_vectors_tests` (verifies fixture against the platform canonicalizers).

MCU CBOR implementation guidance (TinyCBOR-style):

- Encode the envelope as a CBOR map (major type 5) with a *definite* pair count.
- Use only text-string keys (major type 3).
- Sort map keys deterministically:
  - by UTF‑8 byte length
  - then lexicographically by UTF‑8 bytes
  - For UM‑BMP envelopes this means keys are emitted in this order:
    `to`, `auth`, `body`, `from`, `type`, `trace`, `msg_id`, `ts_utc_ms`
    (omit absent optional keys, but preserve ordering of those present).
- Encode integers in minimal CBOR form (major 0/1).
- Avoid floats in signed envelopes. If you must include one, the platform deterministic CBOR encoder uses float64.

Embedded helper (this repo):
- `agent_core` includes a tiny deterministic CBOR writer under `agent/cbor_det.h` that matches the platform’s encoding rules.
- `agent_core` includes tiny base64 helpers under `agent/base64.h` (RFC 4648, standard alphabet) to format `auth.sig`.
- `agent_core` includes `agent/umbmp_auth.h` helpers to build the exact CBOR signing input (`env_no_sig`) and compute base64 signatures.

## Platform enforcement behavior (agentd)

Operator config:
- `edge_auth_required: bool` (default false)
- `edge_auth_require_ts: bool` (default false)
- `edge_auth_max_skew_ms: int64` (default 0 = disabled)
- `edge_auth_require_seq: bool` (default false)
- `edge_auth_kid_policy: "any"|"match_node"|"node_prefix"` (default "any")
- `edge_auth_hmac_keys: { kid -> secret }` (secrets; not exposed in config snapshots)
- `edge_auth_ed25519_pubkeys: { kid -> base64(pubkey32) }` (stored in runtime secrets for uniformity; not exposed in config snapshots)

When `edge_auth_required=true`:
- Missing `auth` => reject with HTTP 401
- Invalid signature / unknown `kid` => reject with HTTP 401
- Additionally, `from` MUST be `node:<node_id>`, and if `body.node_id` is present it MUST match `from`.
 - If `edge_auth_require_ts=true`: missing/invalid `ts_utc_ms` => reject with HTTP 401
 - If `edge_auth_max_skew_ms > 0` and `ts_utc_ms` is present: reject with HTTP 401 when outside the allowed skew window.
 - If `edge_auth_require_seq=true`: missing/invalid `auth.seq` => reject with HTTP 401; platform enforces strict monotonic increase per node (best-effort).
 - If `edge_auth_kid_policy != "any"` and `from:"node:<node_id>"`:
   - `match_node`: require `auth.kid == <node_id>`
   - `node_prefix`: require `auth.kid == <node_id>` OR `auth.kid` starts with `<node_id>:`

When `edge_auth_required=false`:
- Missing `auth` => accept (legacy bring-up)
- If `auth` is present, it MUST verify (reject invalid with HTTP 401)
 - Optional timestamp skew checks apply only when `auth` is present (and `ts_utc_ms` is present when required).

## Current proof points

- `ctest` includes `umbmp_auth_vectors_tests` for canonical JSON/CBOR fixture verification.
- `ctest` includes `agentd_edge_auth_hmac_cbor_wire_smoke` and
  `agentd_edge_auth_ed25519_cbor_wire_smoke` for authenticated CBOR ingress.
- `ctest` includes `agentd_edge_auth_hmac_smoke` and `agentd_edge_auth_ed25519_smoke`
  for authenticated JSON ingress and operator enforcement.
- `ctest` includes `agentd_edge_manifest_bundle_smoke` for the platform-signed manifest bundle
  export surface (`GET /api/v1/edge/node/manifest_bundle`).
