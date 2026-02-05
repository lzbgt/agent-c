# UM‑EAIS v0.2 — Attestation + Correlation (Draft)

Date: 2026-02-05

This document is a **delta** on top of UM‑EAIS v0.1 (`docs/spec/um-eais/um-eais-v0.1.md`).

Goal: make edge/MCU interoperability “power-unleashed” by improving:

- **Correlation**: stable, end-to-end `trace_id` propagation across task lifecycle messages
- **Correctness surface**: a durable, replayable **hash surface** + optional node-provided attestation blob

Non-goals (v0.2):

- Standardizing a full PKI/identity story (hardware roots, cert chains, key rotation).
- Requiring signed attestations (platform treats attest as best-effort data).
- Defining a strict canonical JSON hash algorithm that all nodes must implement.

## 1) Envelope `trace` (correlation)

UM‑BMP envelopes already allow an optional top-level `trace` object. In v0.2, the platform and nodes
should treat `trace.trace_id` as the primary correlation key.

### 1.1 `trace.trace_id`

- Type: string token
- Length: 1–128
- Allowed chars: `[A-Za-z0-9-_.:@]`

### 1.2 Propagation rule

- When the platform assigns work (e.g. `TASK_ASSIGN`, `WORKFLOW_*`, `DURABLE_WORKFLOW_*`) and it has a `trace_id`,
  it should include it in the envelope `trace.trace_id`.
- Nodes should echo the **same** `trace.trace_id` on all subsequent lifecycle messages for that unit of work:
  `TASK_ACK`, `TASK_EVENT`, `TASK_DONE`, `TASK_FAILED`.

### 1.3 Backfill rule (platform robustness)

To compensate for lossy transports and legacy nodes:

- If an inbound edge message contains a valid `trace.trace_id` and the corresponding `edge_tasks.trace_id` is empty,
  the platform may **backfill** `edge_tasks.trace_id` with the inbound value.
- If a node sends a `trace_id` that differs from the platform-stored one, the platform should not override the stored id,
  but it should record evidence (e.g. `_trace_id_mismatch`) for debugging/auditing.
- If the inbound `trace_id` is invalid, the platform should ignore it and record `_trace_id_invalid` evidence.

## 2) Task completion attestation (`result.attest`)

For `TASK_DONE`, v0.2 allows nodes to attach an optional attestation blob under:

`body.result.attest` (object)

This is treated as **best-effort** by the platform: it is persisted and surfaced, but does not (yet) gate success.

### 2.1 Recommended keys

Nodes may include any keys, but v0.2 recommends:

- `result_sha256`: a sha256 token (`sha256:<hex64>` or `<hex64>`) representing a node-computed hash surface
  for the result. (Algorithm is intentionally not standardized in v0.2.)
- `kid`: key identifier (string) for the signing key used by the node (if any).
- `alg`: algorithm identifier (string) (e.g. `ed25519`, `hmac-sha256`, etc; not standardized).
- `sig`: signature bytes as a string (encoding not standardized in v0.2; base64 is recommended).
- `ts_utc_ms`: attestation timestamp (integer).

### 2.2 Platform-computed deterministic hash surface

Independently of node-provided attestations, the platform computes:

- `edge_tasks.result_sha256`: sha256 of the **platform-persisted** `result_json` bytes (best-effort deterministic within the platform build).

This enables:

- replay checks (did the stored result change?)
- deterministic joins/quorum in workflows (`aggregate.quorum_hashes`)

If the node also provides `result.attest.result_sha256`, the platform may record mismatch evidence to make it visible
when the hash surfaces diverge.

## 3) Compatibility and evolution

UM‑EAIS v0.2 is designed so:

- v0.1 nodes can interoperate (they can omit `trace` and `attest`)
- v0.2 nodes get improved correlation + correctness surfaces

Future (v0.3+):

- Standardize the canonical hash algorithm for `result_sha256` across nodes and platform (portable across languages).
- Standardize signature formats and trust roots for enforceable attestations.

Update:

- v0.3 draft now exists as `docs/spec/um-eais/um-eais-v0.3.md` and the platform has an implementation of
  `agent_json_c14n_v1` (portable canonical JSON hashing) for `result_sha256`.
