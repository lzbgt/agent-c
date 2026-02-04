# Edge Agent Interop (UM‑EAIS v0.1) — Platform/Broker Support in `agentd`

Date: 2026-02-04

This repo implements the **platform/broker** side of the UM‑EAIS v0.1 draft spec (transport-agnostic payload semantics).

Canonical spec (verbatim copy):
- `docs/spec/um-eais/um-eais-v0.1.md` (copied from `../urine_monitor` commit `278ad9e5`)

This document describes the **HTTP transport mapping** implemented by `agentd` for that payload-level spec.

## Design

UM‑BMP defines:
- the **message envelope** (`msg_id`, `ts_utc_ms`, `type`, `from`, `to`, `body`)
- the **semantics** for discovery + tasking + events

UM‑BMP does *not* mandate a transport. `agentd` provides a simple, robust mapping:

- **Ingress**: `POST /api/v1/edge/message` (any UM‑BMP envelope; idempotent by `msg_id`)
- **Egress**: `GET /api/v1/edge/outbox?node_id=...&cursor=0&limit=256` (per-node outbox cursor)

This makes it easy to layer:
- MQTT gateway (topic → HTTP bridge)
- WebSocket gateway
- LoRa hub backhaul

…without changing payload semantics.

## Node-initiated collaboration (platform extensions)

In addition to the canonical UM‑EAIS v0.1 message types, `agentd` implements a small set of platform-side extensions that
enable **node-initiated orchestration** (handoff to the coordinator) over the same ingress pipe:

- `WORKFLOW_SUBMIT` (node → platform): persist a new edge workflow (`edge_workflows`, `edge_workflow_steps`)
- `WORKFLOW_CANCEL` (node → platform): cancel an edge workflow (`CANCELED`)

Details: `docs/spec/um-eais/um-eais-platform-extensions-v0.1.md`

## Endpoints

All endpoints require daemon auth when `agentd` is started with `--auth-token`.

### Ingest UM‑BMP envelopes

`POST /api/v1/edge/message`

This stores the envelope durably (`edge_inbox_messages`) and updates platform state:
- `NODE_HELLO`, `NODE_HEARTBEAT` update `edge_nodes`
- `NODE_CAPS_RSP` stores manifest + extracts tags/tools/presence
- `TASK_*` messages update `edge_tasks` + append `edge_task_events`
- `SENSOR_EVENT` appends `edge_sensor_events`

If the platform sees a new/unknown `caps_sha256`, it queues a `PLATFORM_CAPS_REQ` to the node outbox.

### Poll node outbox

`GET /api/v1/edge/outbox?node_id=...&cursor=0&limit=256`

Returns messages in ascending `outbox_id` order. The node should:
- persist its last `cursor_next`
- re-poll with `cursor=<cursor_next>`

### Debug registry helpers

- `GET /api/v1/edge/nodes`
- `GET /api/v1/edge/node?node_id=...`
- `GET /api/v1/edge/node/caps?node_id=...`

### Platform helper: enqueue TASK_ASSIGN

`POST /api/v1/edge/task/assign`

Creates (or dedupes) an `edge_tasks` row and enqueues a UM‑BMP `TASK_ASSIGN` to the target node outbox.

Targeting:
- explicit: `node_id`
- capability routing: `match_any { requires_tools, tags_all, tags_any, tags_none }`

Safety/rate gates (best-effort, platform-side):
- For `mode:"invoke"`, the platform requires a stored node manifest (`NODE_CAPS_RSP`) so it can inspect tool metadata.
- Denies tools tagged with hazard `privacy_camera` by default (unless explicitly allowed via request).
- Denies `side_effect_level:"high"` by default (unless explicitly allowed via request).
- Enforces per-tool `rate_limit` from the manifest (`max_per_minute`, `cooldown_ms`) using platform-side state.

### Task status

`GET /api/v1/edge/task?task_id=...&step_id=...`

### Automation rules (SENSOR_EVENT → TASK_ASSIGN)

- `POST /api/v1/edge/rule/upsert`
- `GET /api/v1/edge/rules`
- `DELETE /api/v1/edge/rule?rule_id=...`

Rules are evaluated during `SENSOR_EVENT` ingestion. The platform enqueues a `TASK_ASSIGN` when:
- `event_type` matches
- `confidence >= min_confidence`
- `cooldown_ms` has elapsed since `last_fired_utc_ms`

### Durable edge workflows (UM‑WF)

- `POST /api/v1/edge/workflow/submit`
- `POST /api/v1/edge/workflow/cancel`
- `GET /api/v1/edge/workflow?workflow_id=...&include_steps=1`
- `GET /api/v1/edge/workflows?status=...&limit=...`
- `GET /api/v1/edge/workflow/events?workflow_id=...&cursor=0&limit=256`
- `GET /api/v1/edge/workflow/stream?workflow_id=...&cursor=0` (SSE)

Workflows are executed by a background runner in `agentd`:
- dispatches `invoke_tool`/`run_agent` steps via `TASK_ASSIGN`
- supports `depends_on` sequencing, parallel fan-out, and `join` (`all|any`)

## Quick smoke flow (single node)

1) Node sends `NODE_HELLO` → platform queues `PLATFORM_CAPS_REQ`
2) Node polls outbox, receives caps req, responds with `NODE_CAPS_RSP`
3) Platform enqueues `TASK_ASSIGN` (`mode:"invoke"`) to call a device tool
4) Node replies with `TASK_ACK` + `TASK_EVENT` + `TASK_DONE`

Proof:
- `ctest` includes `agentd_edge_interop_smoke` and `agentd_edge_workflow_submit_message_smoke`.

## Storage

DB tables are documented in `docs/DB.md`:
- `edge_nodes`
- `edge_inbox_messages`
- `edge_outbox_messages`
- `edge_tasks`
- `edge_task_events`
- `edge_sensor_events`
- `edge_tool_rate_state`
- `edge_rules`
- `edge_workflows`
- `edge_workflow_steps`
- `edge_workflow_events`
