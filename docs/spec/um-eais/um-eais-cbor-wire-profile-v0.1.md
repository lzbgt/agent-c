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

UM‑BMP auth v0.4 already defines message-level signatures where the signing input may be CBOR bytes
(`auth.alg="hmac-sha256-cbor"` / `auth.alg="ed25519-cbor"`).

The recommended approach is:

- Use a deterministic encoding:
  - CBOR deterministic encoding (RFC 8949 definite-length items; deterministic map key ordering; minimal integers)
- Hash/sign the deterministic CBOR bytes (not the decoded JSON), to avoid ambiguities.

This repo’s current “correctness surfaces” (e.g. `agent_json_c14n_v1` for JSON) remain valid
for platform-side storage and quorum logic, but wire-level attestations should prefer wire bytes.

## Implementation status (this repo)

Platform (`agentd`):
- Shipped: CBOR decoding on `POST /api/v1/edge/message` when `Content-Type: application/cbor`.
- Shipped: CBOR encoding on `GET /api/v1/edge/outbox` when `Accept: application/cbor`.
- Proof: `ctest` includes `agentd_edge_message_cbor_smoke` and `agentd_edge_outbox_cbor_smoke`.

Node (`agent_core`):
- Shipped (partial): deterministic CBOR **writer** helpers under `agent/cbor_det.h` (encoder only).
- Shipped (partial): tiny CBOR **reader** under `agent/cbor_read.h` (definite-length; no allocations).
- Shipped (partial): UM‑BMP envelope CBOR decode helper under `agent/umbmp_envelope_read.h` (extracts envelope metadata; returns `body` as an opaque CBOR slice).
- Shipped (partial): UM‑EAIS `TASK_ASSIGN` body CBOR decode helper under `agent/um_eais_task_assign_read.h` (extracts ids/mode/deadline; returns `payload` as an opaque CBOR slice).
- Shipped (partial): UM‑EAIS task lifecycle CBOR **body encoders** under `agent/um_eais_task_lifecycle_write.h` for MCU-friendly replies:
  - `TASK_ACK` body encoder
  - `TASK_EVENT` body encoder
  - `TASK_FAILED` body encoder
  - `TASK_DONE` body encoder (result is caller-encoded CBOR map/value)
- Shipped (partial): UM‑EAIS node lifecycle CBOR **body encoders** under `agent/um_eais_node_write.h` for MCU-friendly node bring-up:
  - `NODE_HELLO` body encoder
  - `NODE_HEARTBEAT` body encoder (optionally includes `health`)
  - `SENSOR_EVENT` body encoder (requires caller-supplied deterministic `data` encoder)
- Shipped (partial): UM‑BMP envelope CBOR encode helper under `agent/umbmp_auth.h` (`agent_umbmp_envelope_cbor_v0_4`) for sending signed (or unsigned) envelopes over CBOR wire.
- Not shipped (yet): full JSON<->CBOR mapping helpers for MCU firmware.
- Recommendation: keep using a full CBOR library (e.g. TinyCBOR-derived) if you already have one, but use the deterministic profile
  defined in this repo for signed envelopes; `agent/cbor_det.h` can be used as a minimal fallback for signing inputs.
