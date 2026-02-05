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
    "sig": "<base64-of-32-bytes>"
  }
}
```

Semantics:
- `auth.alg` MUST be `"hmac-sha256"`.
- `auth.kid` selects a shared secret provisioned on both node/gateway and platform.
- `auth.sig` is base64 (RFC 4648 standard alphabet) of the 32-byte HMAC digest.

## Signing input

Compute the signature over the canonical JSON bytes of the envelope **with the `auth` field removed**.

Canonicalization algorithm: `agent_json_c14n_v1`:
- object keys sorted lexicographically (UTF‑8 bytes)
- no whitespace
- number normalization (no exponent, trailing zeros removed)

Pseudocode:

1) `env_no_auth = env` with `auth` removed
2) `canon = json_c14n(env_no_auth)`
3) `sig = base64(HMAC_SHA256(secret_for(kid), canon))`

## Platform enforcement behavior (agentd)

Operator config:
- `edge_auth_required: bool` (default false)
- `edge_auth_hmac_keys: { kid -> secret }` (secrets; not exposed in config snapshots)

When `edge_auth_required=true`:
- Missing `auth` => reject with HTTP 401
- Invalid signature / unknown `kid` => reject with HTTP 401
- Additionally, `from` MUST be `node:<node_id>`, and if `body.node_id` is present it MUST match `from`.

When `edge_auth_required=false`:
- Missing `auth` => accept (legacy bring-up)
- If `auth` is present, it MUST verify (reject invalid with HTTP 401)

