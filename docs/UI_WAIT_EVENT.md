# UI Wait Event (Host Tool) (Draft)

Date: 2026-01-30

This document defines cooperative host tools (`ui_wait_*`) which allow an agent/tool-loop run to **wait**
for a client acknowledgement (posted via `POST /api/v1/session/client_event`) within the **same run**.

This reduces runaway loops where the model keeps repeating the same action because it has no way to observe
that the UI/user completed it.

## Goals

- Provide a deterministic stop condition for workflows like:
  - “play audio then continue when finished”
  - “show notification then continue when acknowledged”
- Work without the SQLite troubleshooting DB (DB is optional); rely on a session-scoped JSONL file.
- Be cooperative with daemon job cancellation (`/api/v1/job/cancel`).
- Keep tool output bounded and machine-readable.

## Current state (facts)

- The `ui_wait_*` tools are implemented as polling against the session-scoped client event log. This is reliable and portable,
  but not a low-latency push channel.
- Matching is intentionally bounded (partial object match, exact scalars/arrays) to keep evaluation safe and deterministic.
  If you need richer logic, use `client_rpc` and/or a scriptable probe in the client runtime.

## Persistence model

When `POST /api/v1/session/client_event` is called, agentd appends a line to:
- Legacy (file-backed): `<sessions_root>/<session_id>.client_events.jsonl` (current canonical store is the DB)

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
- `data_match` (object, optional): partial-match filters applied to `payload.data`
  - objects are matched by requiring the specified keys/values (extra keys in the payload are allowed)
  - arrays are matched exactly (same length + element equality)
  - scalar values match by exact equality (string/bool/number/null)
- `max_bytes` (int, optional, default `262144`): max bytes read from the end of the JSONL file each poll

Example: wait for a notification acknowledgement for a specific tool call:

```json
{
  "type": "notification_ack",
  "timeout_ms": 60000,
  "data_match": { "tool_call_id": "call_ua_1" }
}
```

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

## Join waits (multiple predicates)

For workflows where “done” requires **multiple** UI acknowledgements (or where the user may do one of several actions),
the host toolset also exposes:

- `ui_wait_any`: OR-join. Returns when the first predicate matches.
- `ui_wait_all`: AND-join. Returns when all predicates match.

Arguments (both tools):
- `predicates` (array, required): list of `{ type, after_unix_ms?, path?, data_match? }`
- `timeout_ms` (int, optional, default `30000`)
- `max_bytes` (int, optional, default `262144`)
- `max_files` (int, optional): consider rotated client event logs up to this many files

Example: wait until both “artifact was rendered” and “client RPC completed”:

```json
{
  "predicates": [
    { "type": "artifact_rendered", "data_match": { "tool_call_id": "call_af_1" } },
    { "type": "client_rpc_result", "data_match": { "rpc_id": "rpc_123", "ok": true } }
  ],
  "timeout_ms": 60000
}
```
