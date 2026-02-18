# Agentd DB Query API (Troubleshooting) — Draft

Date: 2026-02-17

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

## Additional goals

- Make the DB query API a first-class canonical surface alongside `.sess` / `.events.jsonl`, with a clear migration path.
- Provide advanced analytics endpoints and precomputed aggregates for operators and UIs.

## Endpoints

All endpoints return JSON with:
- `ok` (bool)
- `error` (string, optional)

### List runs

`GET /api/v1/db/runs?session_id=...&limit=...&offset=...`

Optional filters:
- `only_errors=1` to return only failing runs (`ok=0`)
- `stop_reason=<reason>` (or `reason=<reason>`) to filter by an exact stop reason (e.g. `max_steps_exceeded`)

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

`GET /api/v1/db/run?run_id=...&include_events=1&include_tools=1&include_artifacts=1&include_ui_actions=1`

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
- `ui_actions` (array, optional) ordered by `id`
- Each ui_action row includes:
  - `action_json` (string|null): raw JSON object string
  - `action` (object, optional): parsed form of `action_json` when parsing succeeds

### List workflows

`GET /api/v1/db/workflows?status=...&session_id=...&trace_id=...&limit=...&offset=...`

Optional filters:
- `status=<status>` (e.g. `queued`, `running`, `done`, `error`, `cancelled`)
- `session_id=<session_id>`
- `trace_id=<trace_id>`
- `include_spec=1` to include `spec_json`/`spec`
- `include_result=1` to include `result_json`/`result`

Response fields:
- `workflows` (array)
  - `workflow_id` (string)
  - `session_id` (string|null)
  - `trace_id` (string|null)
  - `priority` (number|null)
  - `deadline_unix_ms` (number|null)
  - `idempotency_key` (string|null)
  - `created_unix_ms` (number)
  - `updated_unix_ms` (number)
  - `status` (string)
  - `cancel_requested` (bool)
  - `error` (string|null)
  - `spec_json` (string|null, optional)
  - `spec` (object, optional): parsed form of `spec_json` when parsing succeeds
  - `result_json` (string|null, optional)
  - `result` (object, optional): parsed form of `result_json` when parsing succeeds

### Fetch workflow details

`GET /api/v1/db/workflow?workflow_id=...&include_tasks=1&include_events=1`

Response fields:
- `workflow` (object) basic workflow fields + `spec_json`/`result_json`
- `tasks` (array, optional) ordered by `updated_unix_ms DESC`
- `events` (array, optional) ordered by `event_id`

### List workflow tasks

`GET /api/v1/db/workflow_tasks?workflow_id=...&limit=...&offset=...`

Response fields:
- `tasks` (array)
  - `workflow_id` (string)
  - `task_id` (string)
  - `priority` (number|null)
  - `created_unix_ms` (number)
  - `updated_unix_ms` (number)
  - `status` (string)
  - `allow_error` (bool)
  - `attempt` (number)
  - `max_attempts` (number)
  - `ready_unix_ms` (number)
  - `started_unix_ms` (number|null)
  - `finished_unix_ms` (number|null)
  - `depends_on_json` (string|null)
  - `depends_on` (array, optional): parsed form of `depends_on_json` when parsing succeeds
  - `request_json` (string)
  - `request` (object, optional): parsed form of `request_json` when parsing succeeds
  - `expect_json` (string|null)
  - `expect` (object, optional): parsed form of `expect_json` when parsing succeeds
  - `result_json` (string|null)
  - `result` (object, optional): parsed form of `result_json` when parsing succeeds
  - `error` (string|null)
  - `tool_calls_total_cum` (number)
  - `steps_executed_cum` (number)
  - `elapsed_ms_cum` (number)
  - `prompt_tokens_cum` (number)
  - `completion_tokens_cum` (number)
  - `total_tokens_cum` (number)

### List workflow events

`GET /api/v1/db/workflow_events?workflow_id=...&limit=...&offset=...`

Response fields:
- `workflow_events` (array)
  - `event_id` (number)
  - `workflow_id` (string)
  - `task_id` (string|null)
  - `ts_unix_ms` (number)
  - `type` (string)
  - `data_json` (string|null)
  - `data` (object, optional): parsed form of `data_json` when parsing succeeds

### List edge workflows

`GET /api/v1/db/edge_workflows?status=...&limit=...&offset=...`

Optional filters:
- `status=<status>` (e.g. `queued`, `running`, `done`, `error`, `cancelled`)
- `include_spec=1` to include `spec_json`/`spec`

Response fields:
- `edge_workflows` (array)
  - `workflow_id` (string)
  - `goal` (string|null)
  - `status` (string)
  - `priority` (number)
  - `spec_json` (string|null, optional)
  - `spec` (object, optional): parsed form of `spec_json` when parsing succeeds
  - `created_utc_ms` (number)
  - `updated_utc_ms` (number)
  - `error` (string|null)

### Fetch edge workflow details

`GET /api/v1/db/edge_workflow?workflow_id=...&include_steps=1&include_events=1`

Response fields:
- `edge_workflow` (object) basic workflow fields + `spec_json`
- `steps` (array, optional) ordered by `updated_utc_ms DESC`
- `events` (array, optional) ordered by `id`

### List edge workflow steps

`GET /api/v1/db/edge_workflow_steps?workflow_id=...&limit=...&offset=...`

Response fields:
- `edge_workflow_steps` (array)
  - `workflow_id` (string)
  - `step_id` (string)
  - `kind` (string)
  - `depends_on_json` (string|null)
  - `depends_on` (array, optional): parsed form of `depends_on_json` when parsing succeeds
  - `target_json` (string|null)
  - `target` (object, optional): parsed form of `target_json` when parsing succeeds
  - `payload_json` (string|null)
  - `payload` (object, optional): parsed form of `payload_json` when parsing succeeds
  - `join_mode` (string|null)
  - `deadline_utc_ms` (number|null)
  - `state` (string)
  - `created_utc_ms` (number)
  - `updated_utc_ms` (number)
  - `error` (string|null)
  - `attempt` (number)
  - `max_attempts` (number)
  - `next_ready_utc_ms` (number)
  - `backoff_ms` (number)

### List edge workflow events

`GET /api/v1/db/edge_workflow_events?workflow_id=...&limit=...&offset=...`

Response fields:
- `edge_workflow_events` (array)
  - `id` (number)
  - `workflow_id` (string)
  - `ts_utc_ms` (number)
  - `type` (string)
  - `data_json` (string|null)
  - `data` (object, optional): parsed form of `data_json` when parsing succeeds

### Workflow analytics aggregates

`GET /api/v1/db/analytics/workflows?scope=all&since_unix_ms=...&until_unix_ms=...`

Parameters:
- `scope=all|durable|edge` (default `all`)
- `since_unix_ms` / `until_unix_ms` (optional time window, applied to updated timestamps)

Response fields:
- `durable` (object, optional when `scope=durable|all`)
  - `counts` (object) status → count
  - `total` (number)
  - `terminal` (object): `{count, avg_ms, min_ms, max_ms}` for `done|error|cancelled`
  - `error_count` (number)
  - `error_rate` (number|null)
- `edge` (object, optional when `scope=edge|all`)
  - same fields as `durable`, using edge workflow timestamps

### Workflow analytics exports (CSV/JSON)

`GET /api/v1/db/analytics/workflows/export?format=json|csv&scope=all|durable|edge&since_unix_ms=...&until_unix_ms=...`

Parameters:
- `format` (default `json`): `json` or `csv`
- `scope` (default `all`): `all`, `durable`, or `edge`
- `since_unix_ms` / `until_unix_ms` (optional time window)

Response (JSON):
- `ok` (boolean)
- `generated_utc_ms` (number)
- `scope` (string)
- `since_unix_ms` / `until_unix_ms` (number|null)
- `durable` (object, optional per scope): same fields as workflow analytics aggregates
- `edge` (object, optional per scope): same fields as workflow analytics aggregates

Response (CSV):
- `Content-Type: text/csv`
- Header: `section,metric,key,value`
- Rows include `meta` entries (generated time + window), `durable` metrics, and `edge` metrics

### Edge analytics aggregates

`GET /api/v1/db/analytics/edge?since_unix_ms=...&until_unix_ms=...&active_within_ms=...`

Parameters:
- `since_unix_ms` / `until_unix_ms` (optional time window, applied to updated timestamps)
- `active_within_ms` (optional): if set, counts edge nodes with a heartbeat within the last N ms

Response fields:
- `edge_tasks` (object)
  - `counts` (object) state → count
  - `total` (number)
  - `terminal` (object): `{count, avg_ms, min_ms, max_ms}` for `done|error|cancelled`
  - `error_count` (number)
  - `error_rate` (number|null)
- `edge_nodes` (object)
  - `total` (number)
  - `window_count` (number, optional when time window provided)
  - `active_within_ms` (number, optional when provided)
  - `active_count` (number, optional when provided)
  - `last_heartbeat_min` / `last_heartbeat_max` (number|null)

### Edge analytics exports (CSV/JSON)

`GET /api/v1/db/analytics/edge/export?format=json|csv&scope=all|edge_tasks|edge_nodes&since_unix_ms=...&until_unix_ms=...&active_within_ms=...`

Parameters:
- `format` (default `json`): `json` or `csv`
- `scope` (default `all`): `all`, `edge_tasks`, or `edge_nodes`
- `since_unix_ms` / `until_unix_ms` (optional time window)
- `active_within_ms` (optional): counts edge nodes with a heartbeat within the last N ms

Response (JSON):
- `ok` (boolean)
- `generated_utc_ms` (number)
- `scope` (string)
- `since_unix_ms` / `until_unix_ms` (number|null)
- `active_within_ms` (number|null)
- `edge_tasks` (object, optional per scope): same fields as edge analytics aggregates
- `edge_nodes` (object, optional per scope): same fields as edge analytics aggregates

Response (CSV):
- `Content-Type: text/csv`
- Header: `section,metric,key,value`
- Rows include `meta` entries (generated time + window), `edge_tasks` metrics, and `edge_nodes` metrics

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

### List UI actions

`GET /api/v1/db/ui_actions?session_id=...&limit=...&offset=...`

Response fields:
- `ui_actions` (array)
  - `id` (number)
  - `run_id` (number)
  - `ts_unix_ms` (number)
  - `type` (string|null)
  - `title` (string|null)
  - `message` (string|null)
  - `path` (string|null)
  - `mime` (string|null)
  - `repeat` (number)
  - `autoplay` (bool)
  - `action_json` (string|null)
  - `action` (object, optional): parsed form of `action_json` when parsing succeeds

### List DB sessions (most recently updated)

`GET /api/v1/db/sessions?limit=...&offset=...`

Response fields:
- `sessions` (array)
  - `session_id` (string)
  - `created_unix_ms` (number)
  - `updated_unix_ms` (number)

### List session messages (DB mirror)

`GET /api/v1/db/messages?session_id=...&limit=...&offset=...&max_content_bytes=...&max_mm_bytes=...`

Notes:
- This reads from the DB mirror `messages` table (not from `.sess` directly).
- To keep UI responsive, `content` can be truncated based on `max_content_bytes` (default `8192`).
- Multimodal payloads (`mm_json`) can be truncated based on `max_mm_bytes` (default `1048576`).

Response fields:
- `messages` (array)
  - `id` (number)
  - `idx` (number)
  - `role` (string)
  - `created_unix_ms` (number)
  - `content` (string; possibly truncated)
  - `content_truncated` (bool)
  - `content_bytes` (number): original byte size in DB
  - `mm_json` (string; possibly truncated)
  - `mm_json_truncated` (bool)
  - `mm_bytes` (number): original byte size in DB
  - `mm_truncated` (number, optional): 1 when stored payload was already truncated

### List UI client events

`GET /api/v1/db/client_events?session_id=...&limit=...&offset=...`

Response fields:
- `client_events` (array)
  - `id` (number)
  - `ts_unix_ms` (number)
  - `type` (string)
  - `data_json` (string|null)
  - `data` (object, optional): parsed form of `data_json` when parsing succeeds

## Notes

- If the DB is disabled, endpoints return `404` (not found) or `{ok:false,error:"db disabled"}` depending on call site.
- For richer queries, use `sqlite3` directly against `db_path` shown in `/api/v1/config`.
