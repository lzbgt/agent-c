# UI → agentd Client Events (Draft)

Date: 2026-01-30

This document specifies a minimal **UI → agentd** event channel so the Web UI can send structured “user actions”
back to the daemon, not just text prompts.

This makes the system truly **bidirectional**:
- agent/tools → UI: via typed events like `artifact` and `ui_action`
- UI/user → agentd: via `client_event` posts (persisted for troubleshooting and optionally appended to the session)

## Goals

- Let the UI send explicit acknowledgements / facts to the daemon, for example:
  - “audio finished playing”
  - “user clicked cancel playback”
  - “user acknowledged notification”
- Persist these events in the troubleshooting DB so operators can debug “why did the agent repeat X?”
- Optionally append a compact representation into the session message history so the next run can take it into account.

## Non-goals (for now)

- Mid-run interrupts that change an in-flight tool-loop decision (that needs a cooperative “wait/ack” tool).
- Stable public API guarantees (rolling project).
- Browser autoplay bypass (still requires user gesture).

## Endpoint: `POST /api/v1/session/ui_event`

Request JSON:
- `session_id` (string, required)
- `type` (string, required): event type (e.g. `audio_played`, `notification_ack`)
- `ts_unix_ms` (number, optional): if omitted, daemon uses current time
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

## DB mirror

When `--db-path` is enabled, the daemon mirrors client events into:
- `client_events` table (session-scoped; newest-first index)

This keeps UI events queryable even when `.sess` compaction collapses older text messages.

## File-backed log (always enabled)

Regardless of DB settings, agentd appends every UI client event as a JSONL line to:
- `<sessions_root>/<session_id>.client_events.jsonl`

This file is the canonical source for the host tool `ui_wait_event` (see `docs/UI_WAIT_EVENT.md`), so tool-loop runs can
wait for UI acknowledgements even when the SQLite DB is disabled.

Retention:
- The daemon keeps this log **bounded** by rotating it when it grows too large:
  - current: `<session_id>.client_events.jsonl`
  - backups: `<session_id>.client_events.jsonl.1`, `.2`, ...
- For long-term troubleshooting history, prefer the SQLite DB mirror when enabled (`--db-path`).

Query endpoint:
- `GET /api/v1/db/client_events?session_id=...&limit=...&offset=...`

## Suggested event types

This is intentionally open-ended, but common initial types:
- `audio_play_started`
- `audio_play_finished`
- `artifact_rendered`
- `ui_action_shown`
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

The daemon does not enforce an allowlist for types; UIs should keep payloads small.
