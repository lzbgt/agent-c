# Agentd SQLite DB (Troubleshooting Store)

Date: 2026-01-30

This repo currently persists:

- session messages (conversation): `~/.agent/sessions/<id>.sess` (+ optional `.json`)
- detailed tool/LLM timeline (“audit”): `~/.agent/sessions/<id>.events.jsonl`

Those files are convenient and portable, but they are awkward to query when debugging problems like:

- “Why did the daemon keep running a tool after the job finished?”
- “How many times did we call `shell_exec` in this job?”
- “Show me all tool outputs larger than N bytes.”
- “Which runs produced repeated camera captures?”

This document specifies an **optional SQLite store** that mirrors daemon runs/sessions into a queryable database.

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

- Replace the portable `.sess` format (the DB is a mirror / troubleshooting store first).
- Provide a full “analytics” layer (we just want reliable storage + simple queries).
- Store binary blobs (images/audio). Those should be stored as files, with DB rows referencing paths.

## Enabling

`agentd` adds:

- `--db-path <path>` (optional): enables the DB mirror and writes to the given SQLite file.
- `AGENTD_DB_PATH` env var (optional) as an alternative to the flag.

If DB is disabled, daemon behavior is unchanged (file-based session + JSONL audit only).

Note: DB mirroring respects `no_session: true` requests. If a run is marked ephemeral, the daemon will not persist it
to disk (including the DB mirror).

## Schema (v1)

All timestamps are Unix milliseconds.

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
