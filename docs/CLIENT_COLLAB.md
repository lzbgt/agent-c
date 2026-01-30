# Client Collaboration Protocol (Draft)

Date: 2026-01-30

This project’s agent is not “talking to a UI”; it is collaborating with a **client** over a bidirectional protocol.

The Web UI is one client. Others can be:
- mobile apps
- Slack / Discord / Telegram integrations
- CLI front-ends
- future broker-connected devices

The key requirement is that the agent can make **stateful decisions** based on what the client actually did:
- “the image was rendered”
- “the video finished playing”
- “the user acknowledged the notification”

This is the root-cause fix for “agent repeats the same action forever”: make “done” observable.

## Concepts

### Session

A session is the shared conversation/task context. It is the namespace for:
- messages (prompt/assistant)
- runs
- client events (observable client state transitions)

### Client identity

Client identity is carried in every client event payload:

```json
{
  "type": "artifact_rendered",
  "ts_unix_ms": 0,
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": { "tool_call_id": "call_1", "path": "out.png" }
}
```

Fields:
- `client.id` (string): stable logical id (e.g. `webui`, `slack`, `mobile`), or a per-install identifier.
- `client.kind` (string): optional human-friendly kind (`webui`, `slack`, `mobile`, etc.)
- `client.instance_id` (string): optional per-instance identifier (browser tab, device instance) for debugging.

### Observable state via events

The protocol uses an append-only event log (`*.client_events.jsonl`) rather than mutable shared state.
Clients can send:
- “facts” (`artifact_rendered`, `audio_play_finished`, `notification_ack`, …)
- optional periodic “snapshots” (`client_state`) if polling is needed

Agents can:
- wait (`ui_wait_event` / `ui_wait_any` / `ui_wait_all`) for deterministic DoD handshakes
- wait (preferred names): `client_wait_event` / `client_wait_any` / `client_wait_all`
- probe: `client_peek` to inspect recent client event state without blocking
- request snapshots + wait deterministically (see `docs/CLIENT_STATE.md`)

## Endpoints

Client → agentd:
- `POST /api/v1/session/client_event` (preferred)
- `POST /api/v1/session/ui_event` (legacy alias; same semantics)

Read-back for troubleshooting (file-backed, DB optional):
- `GET /api/v1/session/client_events?session_id=...`

## Definition of Done (DoD)

See `docs/DOD_ACK.md` for concrete patterns.
