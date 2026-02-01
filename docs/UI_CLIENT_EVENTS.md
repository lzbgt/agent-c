# Client → agentd Client Events (Draft)

Date: 2026-01-30

This document specifies a minimal **client → agentd** event channel so a client can send structured “user actions”
back to the daemon, not just text prompts.

This makes the system truly **bidirectional**:
- agent/tools → client: via typed events like `artifact` and `ui_action` (as rendered by a client)
- client/user → agentd: via `client_event` posts (persisted for troubleshooting and optionally appended to the session)

## Goals

- Let the UI send explicit acknowledgements / facts to the daemon, for example:
  - “audio finished playing”
  - “user clicked cancel playback”
  - “user acknowledged notification”
- Persist these events in the troubleshooting DB so operators can debug “why did the agent repeat X?”
- Optionally append a compact representation into the session message history so the next run can take it into account.

## Current state (facts)

- **Mid-run coordination**: this endpoint alone is not sufficient to change an in-flight tool-loop decision. The intended
  workflow is: agent requests a UI-side action/RPC → client posts a correlated event → agent waits deterministically using
  a cooperative wait tool (`ui_wait_event` / `ui_wait_any` / `ui_wait_all`) within the same run.
- **Rolling API**: the schema is expected to evolve quickly; avoid hard dependencies on exact payload shapes.
- **Browser autoplay**: user-gesture policies are enforced by standards-compliant browsers. Clients should attempt autoplay,
  report whether it succeeded, and fall back to a deterministic “gesture handshake” when blocked.

## Endpoint: `POST /api/v1/session/client_event` (preferred)

Request JSON:
- `session_id` (string, required)
- `type` (string, required): event type (e.g. `audio_played`, `notification_ack`)
- `ts_unix_ms` (number, optional): if omitted, daemon uses current time
- `client` (object, optional): client identity
  - `id` (string, optional)
  - `kind` (string, optional)
  - `instance_id` (string, optional)
- `data` (object, optional): arbitrary payload (JSON object)
- `append_to_session` (bool, optional, default `true`):
  - when true: append a minimal textual marker to the session as a normal message (so future runs can see it)
  - when false: store only in DB (troubleshooting)

Response JSON:
- `ok` (bool)
- `error` (string, optional)

Notes:
- This endpoint requires daemon auth when enabled (same as other `/api/v1/*` endpoints).
- This endpoint does not require the DB to be enabled; when the DB is disabled, it still returns `ok:true`
  (but DB mirroring is skipped).
- The UI/client event log can be read back (tail) via `GET /api/v1/session/client_events`.
- Legacy alias: `POST /api/v1/session/ui_event` (same semantics).

## DB mirror

When `--db-path` is enabled, the daemon mirrors client events into:
- `client_events` table (session-scoped; newest-first index)

This keeps UI events queryable even when `.sess` compaction collapses older text messages.

## File-backed log (always enabled)

Regardless of DB settings, agentd appends every UI client event as a JSONL line to:
- Legacy (file-backed): `<sessions_root>/<session_id>.client_events.jsonl` (current canonical store is the DB)

This file is the canonical source for the host tool `ui_wait_event` (see `docs/UI_WAIT_EVENT.md`), so tool-loop runs can
wait for UI acknowledgements even when the SQLite DB is disabled.

Retention:
- The daemon keeps this log **bounded** by rotating it when it grows too large:
  - current: `<session_id>.client_events.jsonl`
  - backups: `<session_id>.client_events.jsonl.1`, `.2`, ...
- For long-term troubleshooting history, prefer the SQLite DB mirror when enabled (`--db-path`).

Query endpoint:
- `GET /api/v1/db/client_events?session_id=...&limit=...&offset=...`

Troubleshooting helper:
- `GET /api/v1/session/clients?session_id=...` lists distinct clients observed in the client event log (based on `payload.client`).

## Suggested event types

This is intentionally open-ended, but common initial types:
- `artifact_rendered`
- `ui_action_shown`
- `client_state`
- `client_capabilities`
- `client_rpc_result`
- `client_rpc_progress`
- `scene_error` (e.g. Canvas2D script exceptions in the Scene)
- `client_probe_result` (legacy)
- `notification_shown`
- `notification_ack`
- `artifact_viewed`

Recommended payloads:
- For `notification_ack`, include enough fields for deterministic matching (so the agent can use `ui_wait_event` with `data_match`):
  - `tool_call_id` (when known; preferred)
  - `title`, `message`

- For `artifact_rendered`, include:
  - `tool_call_id` (preferred; enables deterministic DoD waits)
  - `path`, `kind`, `title` (optional)

- For `ui_action_shown`, include:
  - `tool_call_id` (preferred)
  - `action_type`, `title` (optional)

- For `client_state`, include:
  - `query_id` (recommended when responding to `request_client_state`)
  - `media` (bounded list): objects containing `{ kind, paused, ended, current_time?, duration?, tool_call_id?, path? }`

- For `client_capabilities`, include:
  - `rpcs` (bounded list): objects containing `{ kind, side_effects, description? }`
  - optional legacy alias: `probes` (bounded list) for older probe-only clients

- For `client_rpc_result`, include:
  - `rpc_id` (string, required): correlation id (usually the originating tool call id)
  - `request_tool_call_id` (string, optional): tool call id for troubleshooting
  - `rpc_kind` (string, optional): allowlisted rpc kind
  - `ok` (bool)
  - `result` (object, optional; bounded) when `ok=true`, else `error` (string)

- For `client_rpc_progress`, include:
  - `rpc_id` (string, required)
  - `rpc_kind` (string, optional)
  - `name` (string, required): a phase/event name (client-defined but small/consistent)
  - `payload` (object, optional; bounded)

The daemon does not enforce an allowlist for types; UIs should keep payloads small.
