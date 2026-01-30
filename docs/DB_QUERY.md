# Agentd DB Query API (Troubleshooting) — Draft

Date: 2026-01-30

When `agentd` is started with `--db-path` (or `AGENTD_DB_PATH`), it mirrors runs/events/tool records/artifacts into an SQLite DB
(`docs/DB.md`). This document defines a small, read-only HTTP surface so operators and the Web UI can query that DB directly for
troubleshooting.

These endpoints are intentionally **not** a stable public API; they are a debugging convenience.

## Goals

- Provide a reliable way to inspect daemon history when:
  - the JSONL audit file is too large to manually inspect
  - the UI wants to show “last runs” / “recent errors”
  - you need to correlate runaway tool loops with artifacts / tool calls
- Keep endpoints read-only and safe:
  - require daemon auth when configured (same as other endpoints)
  - return bounded, paged results

## Non-goals

- Replacing the canonical `.sess` / `.events.jsonl` stores.
- Advanced analytics (use `sqlite3` directly for that).

## Endpoints

All endpoints return JSON with:
- `ok` (bool)
- `error` (string, optional)

### List runs

`GET /api/v1/db/runs?session_id=...&limit=...&offset=...`

Response fields:
- `runs` (array)
  - `run_id` (number)
  - `session_id` (string)
  - `job_id` (string|null)
  - `ts_unix_ms` (number)
  - `tools` (string)
  - `model` (string|null)
  - `ok` (bool)
  - `stop_reason` (string|null): best-effort stop reason summary (`done` or last error `reason`)
  - `steps_executed` (number|null): tool-loop steps executed (0 for tools=none)
  - `tool_calls_total` (number|null): total tool calls executed
  - `tool_calls_by_tool_json` (string|null): JSON object string mapping `tool -> count`
  - `tool_calls_by_tool` (object, optional): parsed form of `tool_calls_by_tool_json` when parsing succeeds
  - `last_error_reason` (string|null): last `error` event's `reason` (when known)
  - `error` (string|null)
  - `last_error_json` (string|null): raw JSON object string for the most recent `events.type="error"` row for this run
  - `last_error` (object, optional): parsed form of `last_error_json` when parsing succeeds

### Fetch run details

`GET /api/v1/db/run?run_id=...&include_events=1&include_tools=1&include_artifacts=1`

Response fields:
- `run` (object) basic run fields
- `events` (array, optional) ordered by `event_id`
- Each event row includes:
  - `type` (string)
  - `data_json` (string|null): raw JSON object string
  - `data` (object, optional): parsed form of `data_json` when parsing succeeds
- `tool_records` (array, optional) ordered by `id`
- `artifacts` (array, optional) ordered by `id`
- Each artifact row includes:
  - `artifact_json` (string): raw JSON object string
  - `artifact` (object, optional): parsed form of `artifact_json` when parsing succeeds

### List artifacts

`GET /api/v1/db/artifacts?session_id=...&limit=...&offset=...`

Response fields:
- `artifacts` (array)
  - `id` (number)
  - `run_id` (number)
  - `ts_unix_ms` (number)
  - `path` (string)
  - `kind` (string|null)
  - `mime` (string|null)
  - `title` (string|null)
  - `repeat` (number)
  - `autoplay` (bool)

## Notes

- If the DB is disabled, endpoints return `404` (not found) or `{ok:false,error:"db disabled"}` depending on call site.
- For richer queries, use `sqlite3` directly against `db_path` shown in `/api/v1/config`.
