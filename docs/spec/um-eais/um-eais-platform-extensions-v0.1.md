# UM‑EAIS Platform Extensions v0.1 (Agent Repo Additions)

Date: 2026-02-04

This document defines **platform-side extensions** implemented in this repo (`agentd`) on top of the canonical
UM‑EAIS v0.1 draft (`docs/spec/um-eais/um-eais-v0.1.md`).

Rationale:
- The canonical draft focuses on **platform → node** task dispatch and **node → platform** task/event reporting.
- For real IoT deployments with embedded `agent_core` (C), we also need **node-initiated collaboration**:
  - a node can “handoff” an intent to the platform coordinator
  - the platform can orchestrate multi-node workflows without requiring pre-installed automation rules

Platform correctness requirement (capability caching):
- When a node reports a different `caps_sha256` (via `NODE_HELLO` / `NODE_HEARTBEAT`) than the platform has cached,
  the platform MUST treat the stored manifest as stale:
  - invalidate cached `manifest_json` / `tools_json` / `tags_json` / hardware presence
  - enqueue `PLATFORM_CAPS_REQ` (`want:"full"`) for that node
  - avoid routing tool invocations to a node until an updated `NODE_CAPS_RSP` is received (prevents stale routing)

These extensions are transport-agnostic at the payload level, and have a concrete HTTP mapping via:
- `POST /api/v1/edge/message`

Machine-readable contract (this repo):
- JSON Schema (best-effort, v0.1): `docs/spec/um-eais/schema/um-eais-platform-extensions-v0.1.schema.json`
- Golden transcript fixture(s): `docs/spec/um-eais/fixtures/`

## 1) WORKFLOW_SUBMIT (node → platform)

Envelope:
- `type: "WORKFLOW_SUBMIT"`
- `from: "node:<node_id>"` (recommended)
- `to: "platform"`

Body:
- Either the submitted workflow object directly, or wrapped under `workflow`:

Option A (direct):
```json
{
  "workflow_id": "wf:...",
  "goal": "turn on light + tv + ac",
  "priority": 1,
  "steps": [ ... ]
}
```

Option B (wrapped):
```json
{
  "workflow": { "workflow_id": "wf:...", "steps": [ ... ] }
}
```

Platform behavior:
- Validates the workflow shape (basic safety: id-safe ids; supported step kinds).
- Persists `edge_workflows` + `edge_workflow_steps`.
- Emits `edge_workflow_events` `workflow_created`.
- Returns HTTP `{ok:true, workflow_id}` on the `/api/v1/edge/message` transport mapping.

Best-effort ACK:
- Platform also enqueues an outbox message to the submitting node:
  - `type: "WORKFLOW_ACK"`
  - `body: { workflow_id, ok:true }`

This makes the extension usable over non-HTTP transports (e.g. MQTT bridge) without relying on an immediate HTTP response.

## 2) WORKFLOW_CANCEL (node → platform)

Envelope:
- `type: "WORKFLOW_CANCEL"`
- `from: "node:<node_id>"` (recommended)
- `to: "platform"`

Body:
```json
{ "workflow_id": "wf:..." }
```

Platform behavior:
- Sets `edge_workflows.status = "CANCELED"` and best-effort cancels non-terminal `edge_workflow_steps`.
- Emits `edge_workflow_events` `workflow_canceled`.
- Returns HTTP `{ok:true, workflow_id, status}` on the `/api/v1/edge/message` transport mapping.

Best-effort ACK:
- Platform also enqueues an outbox message to the requesting node:
  - `type: "WORKFLOW_ACK"`
  - `body: { workflow_id, ok:true, status:"CANCELED" }`

Notes:
- This is a platform-level cancellation (prevents future dispatch). It does not guarantee remote side-effects are avoided
  if a node already received a `TASK_ASSIGN`.

## 3) DURABLE_WORKFLOW_SUBMIT (node → platform)

This extension allows a resource-constrained node (MCU running embedded `agent_core`) to hand off a *durable*
multi-step plan to the platform coordinator, without requiring the node to maintain long-lived orchestration state.

Transport mapping:
- Ingress: `POST /api/v1/edge/message` with `type:"DURABLE_WORKFLOW_SUBMIT"`
- Platform behavior: forwards the body to the durable workflow submit endpoint (`POST /api/v1/workflow/submit`)
  after applying a few safety/correlation defaults.

Envelope:
- `type: "DURABLE_WORKFLOW_SUBMIT"`
- `from: "node:<node_id>"` (recommended)
- `to: "platform"`

Body:
- Either the durable workflow submit request object directly, or wrapped under `workflow`:

Option A (direct):
```json
{
  "workflow_id": "wf:...",
  "trace_id": "wf:...",
  "idempotency_key": "edge_msg:...",
  "tasks": [ ... ]
}
```

Option B (wrapped):
```json
{
  "workflow": { "tasks": [ ... ] }
}
```

Platform defaults (applied when fields are missing/empty):
- `workflow_id`: defaults to `wf:<sanitized msg_id>` (id-safe tokenization)
- `trace_id`: defaults to the final `workflow_id`
- `idempotency_key`: defaults to `edge_msg:<sanitized msg_id>` (makes submit retry-safe if the node changes `msg_id`,
  e.g. in a non-idempotent transport bridge; durable workflows also have native idempotency via `idempotency_key`)
- `allow_inline_api_keys`: **forced to `false`** (nodes should not ship provider keys in submitted specs)

Response semantics (HTTP mapping):
- The HTTP response body for `POST /api/v1/edge/message` is the same as `POST /api/v1/workflow/submit`
  (`{ok:true, workflow_id, trace_id, ...}`), unless the envelope is deduped by `msg_id`.

Best-effort ACK:
- Platform also enqueues an outbox message to the submitting node:
  - `type: "DURABLE_WORKFLOW_ACK"`
  - `body: { op:"submit", workflow_id, ok:true|false }`

This makes the extension usable over non-HTTP transports (MQTT/LoRa bridges) that do not preserve synchronous
HTTP responses.

## 4) DURABLE_WORKFLOW_CANCEL (node → platform)

Envelope:
- `type: "DURABLE_WORKFLOW_CANCEL"`
- `from: "node:<node_id>"` (recommended)
- `to: "platform"`

Body:
```json
{ "workflow_id": "wf:..." }
```

Platform behavior:
- Forwards to `POST /api/v1/workflow/cancel`.
- Returns the same HTTP response as the cancel endpoint (unless deduped by `msg_id`).

Best-effort ACK:
- Platform enqueues an outbox message to the requesting node:
  - `type: "DURABLE_WORKFLOW_ACK"`
  - `body: { op:"cancel", workflow_id, ok:true|false }`
