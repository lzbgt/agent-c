# UM‑EAIS Platform Extensions v0.1 (Agent Repo Additions)

Date: 2026-02-04

This document defines **platform-side extensions** implemented in this repo (`agentd`) on top of the canonical
UM‑EAIS v0.1 draft (`docs/spec/um-eais/um-eais-v0.1.md`).

Rationale:
- The canonical draft focuses on **platform → node** task dispatch and **node → platform** task/event reporting.
- For real IoT deployments with embedded `agent_core` (C), we also need **node-initiated collaboration**:
  - a node can “handoff” an intent to the platform coordinator
  - the platform can orchestrate multi-node workflows without requiring pre-installed automation rules

These extensions are transport-agnostic at the payload level, and have a concrete HTTP mapping via:
- `POST /api/v1/edge/message`

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

Notes:
- This is a platform-level cancellation (prevents future dispatch). It does not guarantee remote side-effects are avoided
  if a node already received a `TASK_ASSIGN`.

