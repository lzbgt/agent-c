# MCU CBOR Encoder Notes (TinyCBOR / `cobr` style) — UM‑BMP/UM‑EAIS Deterministic Profile

Date: 2026-02-07
Status: draft (notes)

This note is for **resource-constrained MCU firmware** that wants to speak UM‑BMP/UM‑EAIS to `agentd` using CBOR
(`Content-Type: application/cbor`) and/or envelope auth (`auth.alg="hmac-sha256-cbor"` / `auth.alg="ed25519-cbor"`).

The platform (`agentd`) treats the UM‑BMP envelope as JSON-shaped semantics; CBOR is only a wire encoding.

Primary references in this repo:
- Wire profile: `docs/spec/um-eais/um-eais-cbor-wire-profile-v0.1.md`
- Auth profile: `docs/spec/um-eais/um-bmp-envelope-auth-hmac-v0.4.md`
- Embedded helpers (C): `core/include/agent/cbor_det.h`, `core/include/agent/umbmp_auth.h`
- Platform canonicalizer (C++): `daemon/src/cbor_encode.*`
- Canonical test vectors: `docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json`

## Why “deterministic” matters

For `auth.alg="*-cbor"` the signature covers **deterministic CBOR bytes** of the envelope with `auth.sig` removed.
If two stacks produce different bytes for the “same” logical object, signatures will not verify.

The most common foot-guns are:
- indefinite-length arrays/maps/strings
- map keys not in deterministic order
- encoding floats as float16/float32 sometimes and float64 other times
- coercing numerically integral floats (e.g. `87.0`) into integers (`87`) during CBOR→JSON→CBOR flows

## Deterministic rules (summary)

For signed envelopes, follow these rules:

1) **Definite-length only**
- maps, arrays, text strings: always definite length (no streaming / indefinite items).

2) **Text keys only**
- maps MUST use text-string keys (CBOR major type 3).
- do not use byte-string keys.

3) **Deterministic map key ordering**
- sort map keys by:
  1) UTF‑8 byte length ascending
  2) UTF‑8 bytes lexicographically

This is compatible with RFC 8949 deterministic encoding for **text-string keys**.

For UM‑BMP envelopes (when all optional keys are present), this implies:
`to`, `auth`, `body`, `from`, `type`, `trace`, `msg_id`, `ts_utc_ms`

4) **Minimal integers**
- encode integers in the minimal CBOR integer form (major 0/1 with shortest additional-info width).

5) **Float policy**
- if you must encode floats in signed envelopes, encode as **float64** (`0xfb + 8 bytes`).
- preserve numeric types:
  - a value intended as a float must stay a float in the signing input bytes, even if it is numerically integral.

Recommendation for MCUs:
- prefer **integers / fixed-point** for telemetry (battery percent, RSSI, temperatures, etc).

## Signing procedure (CBOR auth)

High-level algorithm to produce an authenticated envelope:

1) Build `env_no_sig`:
   - include `auth.alg`, `auth.kid`, and optional `auth.seq`
   - omit `auth.sig`
2) Encode `env_no_sig` using the deterministic CBOR rules above ⇒ `signing_input_bytes`
3) Compute signature:
   - HMAC: `sig = HMAC_SHA256(secret[kid], signing_input_bytes)` (32 bytes)
   - Ed25519: `sig = Ed25519_SIGN(sk[kid], signing_input_bytes)` (64 bytes)
4) Base64-encode signature bytes (RFC 4648 standard alphabet) ⇒ `auth.sig`
5) Build the final envelope:
   - same fields as `env_no_sig`
   - plus `auth.sig`
6) Encode final envelope as CBOR and send.

Note: the signature covers the **auth metadata** too (`alg`/`kid`/`seq`), not only the payload.

## Bring-up checks (recommended)

Before debugging “platform rejected my message”, prove your canonicalization:

- Run the platform’s canonicalization tests:
  - `ctest -R umbmp_auth_vectors_tests`
  - `ctest -R cbor_det_roundtrip_tests`
- Validate your MCU encoder matches the canonical bytes in:
  - `docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json`
- For host-side generation of known-good CBOR:
  - `build/agent_core_umbmp_cbor_encode` (emits signed/unsigned CBOR envelopes)

## TinyCBOR / `cobr` implementation sketch

If your firmware has a TinyCBOR-derived API (often “encode map start”, then “encode key”, then “encode value”),
you still need deterministic ordering:

- Collect all keys (and pointers to their associated values) into a small array.
- Sort the array by (len, bytes).
- Emit map entries in sorted order.

For nested objects (e.g. `body.health`), apply the same rule recursively.

## Field encoding suggestions for MCU telemetry

Even though floats are supported, fixed-point is often better:

- `battery_pct`: integer `0..100` (or `battery_bp` basis-points `0..10000`)
- `rssi_dbm`: integer (e.g. `-55`)
- `temp_c_x100`: integer (temperature in centi-degrees)

This reduces ambiguity and makes range validation cheap.

