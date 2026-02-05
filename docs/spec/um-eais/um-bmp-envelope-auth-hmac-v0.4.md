# UM‑BMP Envelope Auth (HMAC) — Profile v0.4 (Draft)

Date: 2026-02-05

This document defines an optional envelope authenticity mechanism for UM‑BMP messages
in constrained IoT/edge systems, intended for:
- MQTT/LoRa gateways (transport bridges where payload authenticity matters)
- MCU nodes (`agent_core`) where TLS mutual auth may not be available end-to-end

This profile is transport-agnostic and applies to both JSON and CBOR wire mappings.

## Goals

- Provide **enforceable authenticity** of the full UM‑BMP envelope at the platform ingress.
- Keep the signing algorithm implementable on MCUs (small code, no big deps).
- Bind node identity (`from: "node:<node_id>"`) to the authenticated envelope when enforcement is enabled.

## Non-goals

- Confidentiality (use TLS/transport security when available).
- Public-key identity / non-repudiation (future profile; e.g., Ed25519).

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
- `auth.alg` MUST be either:
  - `"hmac-sha256"` (signing input is canonical JSON bytes), or
  - `"hmac-sha256-cbor"` (signing input is canonical CBOR bytes).
- `auth.kid` selects a shared secret provisioned on both node/gateway and platform.
- `auth.seq` is an optional monotonic sequence number (recommended when clocks are unreliable).
- `auth.sig` is base64 (RFC 4648 standard alphabet) of the 32-byte HMAC digest.

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

For `auth.alg="hmac-sha256-cbor"` (canonical CBOR):

Signing input is the deterministic CBOR encoding of `env_no_sig`:
- CBOR (RFC 8949) definite lengths only (no indefinite streaming items)
- JSON objects encoded as CBOR maps with lexicographically sorted UTF‑8 string keys
- integers encoded in the minimal CBOR integer form
- floats SHOULD NOT be used in signed envelopes; the platform encodes floats as float64 when present

Pseudocode:

1) `env_no_sig = env` with `auth.sig` removed
2) `canon = cbor_canonical(env_no_sig)` (see above)
3) `sig = base64(HMAC_SHA256(secret_for(kid), canon))`

## Platform enforcement behavior (agentd)

Operator config:
- `edge_auth_required: bool` (default false)
- `edge_auth_require_ts: bool` (default false)
- `edge_auth_max_skew_ms: int64` (default 0 = disabled)
- `edge_auth_require_seq: bool` (default false)
- `edge_auth_kid_policy: "any"|"match_node"|"node_prefix"` (default "any")
- `edge_auth_hmac_keys: { kid -> secret }` (secrets; not exposed in config snapshots)

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
