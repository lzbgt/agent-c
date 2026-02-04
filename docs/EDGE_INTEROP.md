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

### Task status

`GET /api/v1/edge/task?task_id=...&step_id=...`

## Quick smoke flow (single node)

1) Node sends `NODE_HELLO` → platform queues `PLATFORM_CAPS_REQ`
2) Node polls outbox, receives caps req, responds with `NODE_CAPS_RSP`
3) Platform enqueues `TASK_ASSIGN` (`mode:"invoke"`) to call a device tool
4) Node replies with `TASK_ACK` + `TASK_EVENT` + `TASK_DONE`

## Storage

DB tables are documented in `docs/DB.md`:
- `edge_nodes`
- `edge_inbox_messages`
- `edge_outbox_messages`
- `edge_tasks`
- `edge_task_events`
- `edge_sensor_events`

