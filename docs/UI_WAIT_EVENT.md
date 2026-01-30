# UI Wait Event (Host Tool) (Draft)

Date: 2026-01-30

This document defines a cooperative host tool, `ui_wait_event`, which allows an agent/tool-loop run to **wait**
for a UI acknowledgement (posted via `POST /api/v1/session/ui_event`) within the **same run**.

This reduces runaway loops where the model keeps repeating the same action because it has no way to observe
that the UI/user completed it.

## Goals

- Provide a deterministic stop condition for workflows like:
  - “play audio then continue when finished”
  - “show notification then continue when acknowledged”
- Work without the SQLite troubleshooting DB (DB is optional); rely on a session-scoped JSONL file.
- Be cooperative with daemon job cancellation (`/api/v1/job/cancel`).
- Keep tool output bounded and machine-readable.

## Non-goals (for now)

- Real-time push notifications into the tool-loop (this tool polls the session event log).
- Arbitrary complex matching expressions.

## Persistence model

When `POST /api/v1/session/ui_event` is called, agentd appends a line to:
- `<sessions_root>/<session_id>.client_events.jsonl`

Each line is a JSON object (the “payload”) with:
- `type` (string)
- `ts_unix_ms` (number)
- `data` (object)

When `--db-path` is enabled, the daemon also mirrors these events into the SQLite troubleshooting DB (`client_events` table).

## Host tool: `ui_wait_event`

Tool name: `ui_wait_event`

Purpose:
- Block until a matching client event arrives (or until timeout/cancel).

Arguments (JSON):
- `type` (string, required): match `payload.type`
- `timeout_ms` (int, optional, default `30000`, max `300000`)
- `after_unix_ms` (number, optional): ignore events older than this timestamp
- `path` (string, optional): convenience filter for `payload.data.path`
- `max_bytes` (int, optional, default `262144`): max bytes read from the end of the JSONL file each poll

Return:

Success:
```json
{
  "ok": true,
  "data": {
    "tool": "ui_wait_event",
    "timed_out": false,
    "event": { "type": "...", "ts_unix_ms": 0, "data": { } }
  }
}
```

Timeout:
```json
{
  "ok": false,
  "error": "timeout",
  "data": { "tool": "ui_wait_event", "timed_out": true }
}
```

Cancelled:
```json
{
  "ok": false,
  "error": "cancelled",
  "data": { "tool": "ui_wait_event", "cancelled": true }
}
```

## Notes

- This tool is only exposed when the run is session-backed (i.e., `no_session=false`) and the daemon has a sessions root.
- For best results, the UI should post `append_to_session=false` for high-frequency events (like audio playback),
  and allow the agent to use `ui_wait_event` instead of polluting the session transcript with dozens of UI markers.

