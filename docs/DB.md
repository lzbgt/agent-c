# Agentd SQLite DB (Canonical Daemon Store)

Date: 2026-02-04

When built with SQLite support (`AGENT_HAVE_SQLITE3`), `agentd` stores its canonical daemon state in SQLite:
- sessions + message history
- runs + events + tool records
- artifacts + UI actions
- client events (client ↔ daemon collaboration)
- audit records (the Web UI “History” feed)
- durable scene snapshots (`scene_states`)
- durable async job metadata (`jobs`)
- durable workflows (`workflows`, `workflow_tasks`, `workflow_events`)

This is convenient for debugging problems like:

- “Why did the daemon keep running a tool after the job finished?”
- “How many times did we call `shell_exec` in this job?”
- “Show me all tool outputs larger than N bytes.”
- “Which runs produced repeated camera captures?”

Note: the CLI (`./build/agent`) still uses the portable file-backed session store under `~/.agent/sessions/` by default.

## Goals

- Provide a **single queryable store** for:
  - sessions (session ids + message history)
  - runs (prompt + config + outcome)
  - events (typed event log entries, including tool calls/results and streaming deltas)
  - tool records (arguments + outputs + truncation metadata)
- Keep the implementation **host-only** (daemon/CLI), with the core remaining JSON/HTTP/fs-free.
- Keep the DB writer robust and safe:
  - bounded writes (we already cap event fields / tool outputs)
  - WAL mode for concurrency
  - simple schema migrations

## Non-goals

- Provide a full “analytics” layer (we just want reliable storage + simple queries).
- Store binary blobs (images/audio). Those should be stored as files, with DB rows referencing paths.

## Enabling

`agentd` uses SQLite when compiled with SQLite support. You can choose where the DB lives:
- `--db-path <path>` (optional): set an explicit SQLite file path.
- `AGENTD_DB_PATH` env var (optional): alternative to the flag.
- Default: `./agentd.db` in the daemon working directory.

Note: DB persistence respects `no_session: true` requests. If a run is marked ephemeral, the daemon will not persist it to disk.

## Schema versioning

The DB includes a small `meta` table with a single key:

- `schema_version` (integer stored as text)

The daemon runs idempotent schema setup on open and will migrate older DB files forward. If the DB is newer than the current
binary (e.g. you downgrade `agentd`), `agentd` refuses to open it rather than silently corrupting the schema.

## Schema (v12)

All timestamps are Unix milliseconds.

### `meta`

- `key TEXT PRIMARY KEY`
- `value TEXT NOT NULL`

### `sessions`

- `session_id TEXT PRIMARY KEY`
- `created_unix_ms INTEGER`
- `updated_unix_ms INTEGER`

### `messages`

Stores the conversation transcript (role + content). Tool timelines remain in `events` and `tool_records`.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `session_id TEXT NOT NULL`
- `idx INTEGER NOT NULL` (0-based order within the session)
- `role TEXT NOT NULL` (`system|user|assistant|tool|...`)
- `content TEXT NOT NULL`
- `created_unix_ms INTEGER` (best-effort; currently “now” at write time)

Index:
- `CREATE INDEX messages_by_session ON messages(session_id, idx)`

### `runs`

- `run_id INTEGER PRIMARY KEY AUTOINCREMENT`
- `session_id TEXT NOT NULL`
- `job_id TEXT` (optional; async jobs)
- `ts_unix_ms INTEGER NOT NULL`
- `prompt TEXT NOT NULL`
- `tools TEXT NOT NULL`
- `model TEXT`
- `base_url TEXT`
- `stream_assistant INTEGER` (0/1)
- `ok INTEGER` (0/1)
- `stop_reason TEXT` (best-effort; `"done"` when ok, else last error `reason` when known)
- `steps_executed INTEGER` (tool-loop steps executed; 0 for tools=none)
- `tool_calls_total INTEGER` (tool calls executed; usually equals tool_records count; 0 for tools=none)
- `tool_calls_by_tool_json TEXT` (JSON object string mapping `tool_name -> count`)
- `last_error_reason TEXT` (best-effort; last `error` event's `reason`)
- `error TEXT`
- `http_status INTEGER`
- `http_body TEXT` (capped; troubleshooting only)

Indexes:
- `CREATE INDEX runs_by_session ON runs(session_id, ts_unix_ms DESC)`
- `CREATE INDEX runs_by_job ON runs(job_id)`

### `events`

- `event_id INTEGER PRIMARY KEY AUTOINCREMENT`
- `run_id INTEGER NOT NULL`
- `ts_unix_ms INTEGER NOT NULL`
- `type TEXT NOT NULL`
- `data_json TEXT NOT NULL` (JSON object string)

Index:
- `CREATE INDEX events_by_run ON events(run_id, event_id)`

### `tool_records`

Mirrors the host tool records captured during the tool loop.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `run_id INTEGER NOT NULL`
- `tool_name TEXT NOT NULL`
- `tool_call_id TEXT`
- `arguments_json TEXT`
- `result_text TEXT`
- `result_for_prompt_text TEXT`
- `result_truncated_for_prompt INTEGER` (0/1)

Index:
- `CREATE INDEX tool_records_by_run ON tool_records(run_id, id)`

### `artifacts`

Mirrors explicit `artifact` events emitted by the tool loop (see `docs/PROTOCOL.md`).

The DB does not store binary blobs; it stores file references + playback hints and includes the original artifact JSON
for forward-compatibility.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `run_id INTEGER NOT NULL`
- `ts_unix_ms INTEGER NOT NULL`
- `session_id TEXT NOT NULL`
- `tool_call_id TEXT` (optional)
- `path TEXT NOT NULL` (host path; typically relative to tools root / session state)
- `kind TEXT` (optional; `image|audio|video|text|file`)
- `mime TEXT` (optional)
- `title TEXT` (optional)
- `autoplay INTEGER NOT NULL` (0/1)
- `repeat INTEGER NOT NULL`
- `artifact_json TEXT NOT NULL` (JSON object string; stable fallback for future schema changes)

Indexes:
- `CREATE INDEX artifacts_by_run ON artifacts(run_id, id)`
- `CREATE INDEX artifacts_by_session ON artifacts(session_id, ts_unix_ms DESC)`
- `CREATE INDEX artifacts_by_path ON artifacts(path)`

### `ui_actions`

Mirrors explicit `ui_action` events emitted by the tool loop (see `docs/UI_ACTION.md`).

The DB does not execute actions; it stores them for troubleshooting and UI indexing.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `run_id INTEGER NOT NULL`
- `ts_unix_ms INTEGER NOT NULL`
- `session_id TEXT NOT NULL`
- `tool_call_id TEXT` (optional)
- `type TEXT` (optional; `notify|client_rpc|...`)
- `title TEXT` (optional)
- `message TEXT` (optional)
- `path TEXT` (optional; for media actions)
- `mime TEXT` (optional)
- `autoplay INTEGER NOT NULL` (0/1)
- `repeat INTEGER NOT NULL`
- `action_json TEXT NOT NULL` (JSON object string; stable fallback for future schema changes)

Indexes:
- `CREATE INDEX ui_actions_by_run ON ui_actions(run_id, id)`
- `CREATE INDEX ui_actions_by_session ON ui_actions(session_id, ts_unix_ms DESC)`
- `CREATE INDEX ui_actions_by_type ON ui_actions(type)`

### `client_events`

Mirrors explicit client → daemon events posted via `POST /api/v1/session/client_event` (legacy alias: `/session/ui_event`)
(see `docs/UI_CLIENT_EVENTS.md`).

These are useful for troubleshooting “did the user/UI actually do X?” and for allowing future runs to incorporate
UI-side acknowledgements.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `ts_unix_ms INTEGER NOT NULL`
- `session_id TEXT NOT NULL`
- `type TEXT NOT NULL`
- `data_json TEXT NOT NULL` (JSON object string; stable fallback for future schema changes)

Indexes:
- `CREATE INDEX client_events_by_session ON client_events(session_id, ts_unix_ms DESC)`
- `CREATE INDEX client_events_by_type ON client_events(type)`

### `audit_records`

Stores the per-run “History” records returned by `GET /api/v1/session/audit`.

Each row stores the original JSON object string (the same shape returned by the endpoint) so schema changes remain
forward-compatible.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `session_id TEXT NOT NULL`
- `ts_unix_ms INTEGER NOT NULL`
- `run_id INTEGER` (nullable)
- `record_json TEXT NOT NULL` (JSON object string)

Index:
- `CREATE INDEX audit_records_by_session ON audit_records(session_id, ts_unix_ms DESC, id DESC)`

### `scene_states`

Stores the per-session durable “Scene” snapshot (server-owned, DB-backed), used by the Web UI Scene panel.

- `session_id TEXT PRIMARY KEY`
- `updated_unix_ms INTEGER NOT NULL`
- `scene_json TEXT NOT NULL` (JSON object string)

Index:
- `CREATE INDEX scene_states_by_updated ON scene_states(updated_unix_ms DESC)`

### `jobs`

Stores durable async job metadata so job state remains inspectable after daemon restart.

- `job_id TEXT PRIMARY KEY`
- `session_id TEXT` (nullable; jobs may be session-less)
- `trace_id TEXT` (optional; correlation ID)
- `request_json TEXT` (optional; redacted request JSON used for job resume)
- `priority INTEGER` (optional; higher runs sooner; default 0)
- `created_unix_ms INTEGER NOT NULL`
- `updated_unix_ms INTEGER NOT NULL`
- `status TEXT NOT NULL` (`queued|running|done|error|cancelled|interrupted`)
- `cancel_requested INTEGER NOT NULL` (0/1)
- `error TEXT` (optional; last error string)
- `stop_reason TEXT` (optional; best-effort terminal reason)
- `result_json TEXT` (optional; final response JSON blob from the run)
- `last_heartbeat_unix_ms INTEGER` (optional; best-effort liveness signal)

Indexes:
- `CREATE INDEX jobs_by_status ON jobs(status, updated_unix_ms DESC)`
- `CREATE INDEX jobs_by_status_prio ON jobs(status, priority DESC, updated_unix_ms DESC)`
- `CREATE INDEX jobs_by_session ON jobs(session_id, updated_unix_ms DESC)`
- `CREATE INDEX jobs_by_trace ON jobs(trace_id)`

### `workflows`

Stores durable workflow metadata. A workflow is a DAG of tasks that can be resumed after daemon restart.

- `workflow_id TEXT PRIMARY KEY`
- `session_id TEXT` (nullable; workflows can be session-less)
- `trace_id TEXT` (optional; correlation ID, typically shared across the workflow)
- `priority INTEGER` (optional; higher workflows run sooner; default 0)
- `created_unix_ms INTEGER NOT NULL`
- `updated_unix_ms INTEGER NOT NULL`
- `status TEXT NOT NULL` (`queued|running|done|error|cancelled`)
- `cancel_requested INTEGER NOT NULL` (0/1)
- `error TEXT` (optional; best-effort terminal error summary)
- `spec_json TEXT NOT NULL` (JSON object string; submit request, best-effort redacted)
- `result_json TEXT` (optional; aggregated final workflow result)

Indexes:
- `CREATE INDEX workflows_by_status ON workflows(status, updated_unix_ms DESC)`
- `CREATE INDEX workflows_by_status_prio ON workflows(status, priority DESC, updated_unix_ms DESC)`
- `CREATE INDEX workflows_by_trace ON workflows(trace_id)`
- `CREATE INDEX workflows_by_session ON workflows(session_id, updated_unix_ms DESC)`

### `workflow_tasks`

Stores durable workflow tasks. Each task is typically a regular `POST /api/v1/run` request executed by the workflow engine,
with explicit dependencies and retries.

- `workflow_id TEXT NOT NULL`
- `task_id TEXT NOT NULL`
- `priority INTEGER` (optional; higher tasks run sooner; default 0)
- `created_unix_ms INTEGER NOT NULL`
- `updated_unix_ms INTEGER NOT NULL`
- `status TEXT NOT NULL` (`queued|running|done|error|cancelled`)
- `attempt INTEGER NOT NULL` (number of started attempts)
- `max_attempts INTEGER NOT NULL` (retry cap)
- `ready_unix_ms INTEGER NOT NULL` (do not start before this time; used for retry backoff)
- `started_unix_ms INTEGER` (best-effort; when the first attempt started)
- `finished_unix_ms INTEGER` (best-effort; when the last attempt finished)
- `depends_on_json TEXT` (optional; JSON array string of `task_id`s)
- `request_json TEXT NOT NULL` (JSON object string; run request payload)
- `expect_json TEXT` (optional; JSON object string; deterministic assertions on the run output)
- `result_json TEXT` (optional; JSON object string; run response payload)
- `error TEXT` (optional; best-effort error / expectation failure summary)

Indexes:
- `CREATE INDEX workflow_tasks_by_workflow ON workflow_tasks(workflow_id, updated_unix_ms DESC)`
- `CREATE INDEX workflow_tasks_by_status ON workflow_tasks(status, ready_unix_ms, updated_unix_ms DESC)`
- `CREATE INDEX workflow_tasks_by_status_prio ON workflow_tasks(status, priority DESC, ready_unix_ms, updated_unix_ms DESC)`

### `workflow_events`

Durable workflow event log used for:

- workflow progress SSE streaming (`GET /api/v1/workflow/stream`)
- debugging/correlation (what happened when, in what order)

- `event_id INTEGER PRIMARY KEY AUTOINCREMENT`
- `workflow_id TEXT NOT NULL`
- `task_id TEXT` (nullable; task events only)
- `ts_unix_ms INTEGER NOT NULL`
- `type TEXT NOT NULL` (e.g. `workflow_created`, `workflow_status`, `task_status`, `workflow_done`)
- `data_json TEXT NOT NULL` (JSON object string; stable fallback for schema evolution)

Indexes:
- `CREATE INDEX workflow_events_by_workflow ON workflow_events(workflow_id, event_id)`

## Source of truth

The runtime schema and migration logic live in:

- `daemon/src/agent_db.cpp`

If this doc drifts from the code, treat the code as canonical and update this doc accordingly.

## Example queries

Last 20 runs for a session:

```sql
SELECT run_id, ts_unix_ms, ok, tools, model, prompt
FROM runs
WHERE session_id = 'default'
ORDER BY ts_unix_ms DESC
LIMIT 20;
```

Find runs that repeatedly called a tool (e.g. camera capture via `proc_exec`/`shell_exec`):

```sql
SELECT r.run_id, r.ts_unix_ms, tr.tool_name, COUNT(*) AS calls
FROM tool_records tr
JOIN runs r ON r.run_id = tr.run_id
GROUP BY r.run_id, tr.tool_name
HAVING calls >= 5
ORDER BY calls DESC;
```

Show the event timeline for a run:

```sql
SELECT event_id, type, data_json
FROM events
	WHERE run_id = 123
	ORDER BY event_id;
```

List recent artifacts for a session:

```sql
SELECT ts_unix_ms, path, kind, mime, title, repeat, autoplay
FROM artifacts
WHERE session_id = 'default'
ORDER BY ts_unix_ms DESC
LIMIT 50;
```

Find runs that produced many artifacts (e.g. runaway camera captures):

```sql
SELECT r.run_id, r.ts_unix_ms, COUNT(*) AS artifacts
FROM artifacts a
JOIN runs r ON r.run_id = a.run_id
GROUP BY r.run_id
HAVING artifacts >= 5
ORDER BY artifacts DESC;
```

Find recent UI actions for a session:

```sql
SELECT ts_unix_ms, type, title, message, path, repeat, autoplay
FROM ui_actions
WHERE session_id = 'default'
ORDER BY ts_unix_ms DESC
LIMIT 50;
```

## Daemon query endpoints (optional)

If you want to query the DB from the Web UI (or via `curl`) without opening SQLite manually, `agentd` exposes a small read-only
API when DB is enabled:

- `GET /api/v1/db/runs?session_id=...`
- `GET /api/v1/db/run?run_id=...&include_events=1&include_tools=1&include_artifacts=1`
- `GET /api/v1/db/artifacts?session_id=...`

See `docs/DB_QUERY.md`.
