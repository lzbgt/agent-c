# UM‑EAIS / UM‑BMP CBOR Wire Profile v0.1 (Optional)

Date: 2026-02-06

Status: draft (platform support shipped in `agentd`; node support is optional)

## Purpose

MCU/edge systems often operate under constraints where JSON is a poor wire encoding:

- tight link budgets (LoRa, BLE advertisements, constrained MQTT uplinks)
- limited RAM/CPU for parsing
- desire to reduce envelope overhead for high-frequency heartbeats/events

This profile defines a **CBOR** (RFC 8949) wire encoding for UM‑BMP envelopes, without changing payload semantics.

**Canonical semantics remain JSON-shaped** (and are specified by the UM‑EAIS schemas + fixtures in this repo).
CBOR is only a transport encoding option.

## Transport mapping (HTTP)

Endpoint:
- `POST /api/v1/edge/message`
- `GET /api/v1/edge/outbox`

Encoding:
- JSON: `Content-Type: application/json`
- CBOR: `Content-Type: application/cbor`
  - For `GET /api/v1/edge/outbox`, request CBOR with `Accept: application/cbor` and the platform responds with `Content-Type: application/cbor`.

For CBOR requests, the body MUST decode to a map with the same key names as the JSON envelope:

```json
{
  "msg_id": "...",
  "ts_utc_ms": 1700000000000,
  "type": "NODE_HELLO",
  "from": "node:<node_id>",
  "to": "platform",
  "trace": {"trace_id":"..."},
  "body": { ... }
}
```

The HTTP response remains JSON (`application/json`) for operator/debug ergonomics.

## CBOR constraints (v0.1)

To keep decoding bounded, deterministic, and MCU-friendly, the v0.1 CBOR profile is intentionally strict:

- Definite-length items only (no indefinite-length strings/arrays/maps).
- Map keys MUST be text strings (CBOR major type 3).
- CBOR tags MAY be present and are ignored by the platform (the tagged item is decoded).
- CBOR byte strings are **not** used in v0.1 envelopes (prefer text strings or nested objects).

## Determinism and hashing

If/when a future interop version defines message-level hashing or signatures on the wire,
the recommended approach is:

- Use a deterministic encoding:
  - CBOR canonical encoding (RFC 8949, Section “Deterministically Encoded CBOR”)
- Hash the canonical CBOR bytes (not the decoded JSON), to avoid ambiguities.

This repo’s current “correctness surfaces” (e.g. `agent_json_c14n_v1` for JSON) remain valid
for platform-side storage and quorum logic, but wire-level attestations should prefer wire bytes.

## Implementation status (this repo)

Platform (`agentd`):
- Shipped: CBOR decoding on `POST /api/v1/edge/message` when `Content-Type: application/cbor`.
- Shipped: CBOR encoding on `GET /api/v1/edge/outbox` when `Accept: application/cbor`.
- Proof: `ctest` includes `agentd_edge_message_cbor_smoke` and `agentd_edge_outbox_cbor_smoke`.

Node (`agent_core`):
- Not yet shipped: CBOR codec helpers for MCU firmware.
- Recommendation: add a tiny CBOR encoder/decoder module under `core/` later, once the message subset stabilizes.
