# Agentd SQLite DB (Canonical Daemon Store)

Date: 2026-02-19

Status: rolling

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

## Additional goals

- Provide a full analytics layer (materialized views, aggregates, and common query helpers).
- Support binary blobs (images/audio) with explicit size limits, storage tiers, and path-based fallbacks
  (see "Blob Storage Tiers" below).

## DB backend decision (why SQLite)

This repo currently **does not** plan to replace SQLite as the default embedded database.

SQLite is a strong fit for agentd’s workload because it provides, in one small dependency:

- **ACID transactions** (crash-safe durability for workflows/jobs/scheduler state).
- **Relational queries + indexes** (cheap stats, backpressure admission, fairness scans, trace correlation).
- **A stable migration story** (schema versioning is already built into `agentd`).
- **Portability** (single file; works across Linux/macOS/Windows; easy to ship for edge gateways).
- **Tunable, deterministic performance** (WAL mode + bounded writes; predictable behavior under load).

`agentd` already configures SQLite pragmas for multi-connection safety and performance:
- `PRAGMA journal_mode=WAL;`
- `PRAGMA synchronous=NORMAL;`
- `PRAGMA busy_timeout=5000;`
- `PRAGMA foreign_keys=ON;`

### When a different embedded DB would be justified

Replacing SQLite is a **major architectural move** that should be driven by a concrete requirement that SQLite cannot satisfy.
Examples:

- **High write concurrency across many processes** with strict tail latency requirements (SQLite is optimized for 1-writer; WAL helps, but it is still single-writer).
- **Built-in replication / multi-writer** semantics required *inside* the DB layer (not via agent-level interop protocols).
- **Pure key/value access patterns** where SQL adds overhead and no ad hoc queries are needed.
- **Columnar analytics** on large historical datasets where OLAP speed matters more than OLTP durability.

### Alternatives (for future, not current default)

If the project’s requirements change, likely candidates (each with real tradeoffs):

- **libSQL / “SQLite with replication”**: keeps the SQLite model + SQL, adds replication primitives (useful if we need durable multi-agent sync at the DB layer).
- **RocksDB / LevelDB-style LSM KV stores**: great for high write throughput + range scans, but you give up SQL and will rebuild query/migration/constraints in application code.
- **LMDB**: extremely fast local KV with mmap, but still not SQL; different concurrency model; careful with file semantics on some filesystems.
- **DuckDB**: excellent local analytics, but not a drop-in OLTP replacement for a scheduler DB.

For “power unleashed” in this repo, the current highest-leverage path is to keep SQLite and focus engineering time on:
task scheduling, durable workflows, memory architecture, interop/collaboration protocols, and correctness surfaces.

## Enabling

`agentd` uses SQLite when compiled with SQLite support. You can choose where the DB lives:
- `--db-path <path>` (optional): set an explicit SQLite file path.
- `AGENTD_DB_PATH` env var (optional): alternative to the flag.
- Default: `<state_dir>/agentd.db` (state_dir defaults to the daemon working directory or `AGENTD_STATE_DIR`).

Note: DB persistence respects `no_session: true` requests. If a run is marked ephemeral, the daemon will not persist it to disk.

## Schema versioning

The DB includes a small `meta` table with a single key:

- `schema_version` (integer stored as text)

The daemon runs idempotent schema setup on open and will migrate older DB files forward. If the DB is newer than the current
binary (e.g. you downgrade `agentd`), `agentd` refuses to open it rather than silently corrupting the schema.

## Schema (v31)

All timestamps are Unix milliseconds.

### `meta`

- `key TEXT PRIMARY KEY`
- `value TEXT NOT NULL`

### `sessions`

- `session_id TEXT PRIMARY KEY`
- `created_unix_ms INTEGER`
- `updated_unix_ms INTEGER`

### `messages`

Stores the conversation transcript (role + content) plus multimodal JSON payloads. Tool timelines remain in `events` and `tool_records`.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `session_id TEXT NOT NULL`
- `idx INTEGER NOT NULL` (0-based order within the session)
- `role TEXT NOT NULL` (`system|user|assistant|tool|...`)
- `content TEXT NOT NULL` (text-only; multimodal payloads live in `mm_json`)
- `mm_json TEXT` (optional JSON string for multimodal content)
- `mm_bytes INTEGER` (best-effort byte size of `mm_json`)
- `mm_truncated INTEGER` (0/1; set when stored multimodal payload is truncated)
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
- `request_json TEXT` (redacted run request JSON; optional)
- `response_json TEXT` (redacted run response JSON; optional)
- `replay_sha256 TEXT` (best-effort; deterministic hash token over replay bundle)
- `replay_sha256_alg TEXT` (e.g. `agent_json_c14n_v1`)
- `replay_sha256_schema TEXT` (e.g. `run_replay_bundle_v1`)
- `replay_error TEXT` (best-effort; populated when replay bundle cannot be stored)
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

### `approval_requests`

Approval queue entries for gated tool calls.

- `approval_id TEXT PRIMARY KEY`
- `run_id INTEGER` (optional; backfilled after run persistence)
- `trace_id TEXT`
- `session_id TEXT`
- `job_id TEXT`
- `team_id TEXT`
- `tool_name TEXT NOT NULL`
- `tool_call_id TEXT`
- `tool_args_hash TEXT` (SHA256 hex of tool args JSON)
- `required_approvals INTEGER NOT NULL`
- `role_constraints_json TEXT` (JSON array string)
- `status TEXT NOT NULL` (`pending|approved|denied|expired`)
- `created_unix_ms INTEGER NOT NULL`
- `expires_unix_ms INTEGER` (optional)
- `decision_reason TEXT` (optional)

Indexes:
- `CREATE INDEX approval_requests_by_status ON approval_requests(status, created_unix_ms DESC)`
- `CREATE INDEX approval_requests_by_trace ON approval_requests(trace_id, created_unix_ms DESC)`
- `CREATE INDEX approval_requests_by_run ON approval_requests(run_id, created_unix_ms DESC)`

### `approval_decisions`

Approval decisions tied to an approval request.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `approval_id TEXT NOT NULL`
- `member_id TEXT NOT NULL`
- `member_role TEXT` (optional)
- `decision TEXT NOT NULL` (`approve|deny`)
- `decision_unix_ms INTEGER NOT NULL`
- `note TEXT` (optional)

Index:
- `CREATE INDEX approval_decisions_by_approval ON approval_decisions(approval_id, id)`

### `artifacts`

Mirrors explicit `artifact` events emitted by the tool loop (see `docs/PROTOCOL.md`).

The DB does not store binary blobs; it stores file references + playback hints and includes the original artifact JSON
for forward-compatibility. Binary blobs are tracked in `blob_manifest` and linked via `artifact_blobs`.

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

### `blob_manifest`

Tracks locally stored binary blobs (content-addressed; tiered storage v0 uses local files).

- `blob_id TEXT PRIMARY KEY` (`sha256:<hex>`)
- `size_bytes INTEGER NOT NULL`
- `mime TEXT` (optional)
- `sha256_hex TEXT NOT NULL`
- `created_utc_ms INTEGER NOT NULL`
- `last_access_utc_ms INTEGER` (optional)
- `ref_count INTEGER NOT NULL DEFAULT 0`
- `tier TEXT NOT NULL` (`local` in v0)
- `location TEXT NOT NULL` (relative path under `state_dir`)
- `etag TEXT` (optional; object store)
- `storage_class TEXT` (optional; object store)

Indexes:
- `CREATE INDEX blob_manifest_by_last_access ON blob_manifest(last_access_utc_ms DESC)`
- `CREATE INDEX blob_manifest_by_ref_count ON blob_manifest(ref_count, created_utc_ms DESC)`

### `artifact_blobs`

Links artifacts to blobs (ref-counted GC uses this association).

- `artifact_id INTEGER NOT NULL`
- `blob_id TEXT NOT NULL`
- `PRIMARY KEY(artifact_id, blob_id)`

Indexes:
- `CREATE INDEX artifact_blobs_by_blob ON artifact_blobs(blob_id)`

### `ui_actions`

Mirrors explicit `ui_action` events emitted by the tool loop (see `docs/CLIENT.md`).

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
(see `docs/CLIENT.md`).

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
- `deadline_unix_ms INTEGER` (optional; scheduler-level workflow deadline; 0/NULL disables)
- `idempotency_key TEXT` (optional; submit dedupe key; unique per `COALESCE(session_id,'')`)
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
- `CREATE INDEX workflows_by_status_prio_created ON workflows(status, priority DESC, created_unix_ms ASC, workflow_id)` (scheduler scan; oldest-first)
- `CREATE INDEX workflows_by_trace ON workflows(trace_id)`
- `CREATE INDEX workflows_by_session ON workflows(session_id, updated_unix_ms DESC)`
- `CREATE INDEX workflows_by_deadline ON workflows(deadline_unix_ms)`
- `CREATE UNIQUE INDEX workflows_by_idempotency_scope_key ON workflows(COALESCE(session_id,''), idempotency_key) WHERE idempotency_key IS NOT NULL AND idempotency_key <> ''`

### `workflow_tasks`

Stores durable workflow tasks. Each task is typically a regular `POST /api/v1/run` request executed by the workflow engine,
with explicit dependencies and retries.

- `workflow_id TEXT NOT NULL`
- `task_id TEXT NOT NULL`
- `priority INTEGER` (optional; higher tasks run sooner; default 0)
- `created_unix_ms INTEGER NOT NULL`
- `updated_unix_ms INTEGER NOT NULL`
- `status TEXT NOT NULL` (`queued|running|done|error|cancelled`)
- `allow_error INTEGER NOT NULL DEFAULT 0` (if 1, task status=error does not fail the workflow; supports best_of_n / first_ok patterns)
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
- `tool_calls_total_cum INTEGER NOT NULL DEFAULT 0` (cumulative tool calls across attempts; retry-safe budget accounting)
- `steps_executed_cum INTEGER NOT NULL DEFAULT 0` (cumulative tool-loop steps across attempts)
- `elapsed_ms_cum INTEGER NOT NULL DEFAULT 0` (cumulative elapsed_ms across attempts; best-effort)
- `prompt_tokens_cum INTEGER NOT NULL DEFAULT 0` (cumulative provider-reported `usage.prompt_tokens` across attempts; best-effort)
- `completion_tokens_cum INTEGER NOT NULL DEFAULT 0` (cumulative provider-reported `usage.completion_tokens` across attempts; best-effort)
- `total_tokens_cum INTEGER NOT NULL DEFAULT 0` (cumulative provider-reported `usage.total_tokens` across attempts; best-effort)

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

### `workflow_fairq_sessions` (scheduler internal)

Durable fair-queue session state for workflow scheduling policies (v2.3+).

Used to persist per-session DRR deficits across daemon restarts (best-effort). This makes scheduler fairness less “reset”
after restarts when the daemon is under sustained load.

- `session_id TEXT PRIMARY KEY`
- `deficit INTEGER NOT NULL` (may be negative for “debt” in future cost-aware scheduling; current policy typically keeps it non-negative)
- `weight INTEGER NOT NULL` (best-effort last observed weight)
- `updated_unix_ms INTEGER NOT NULL`

Index:
- `CREATE INDEX workflow_fairq_sessions_by_updated ON workflow_fairq_sessions(updated_unix_ms DESC)`

### `edge_nodes`

UM‑EAIS (edge embedded-agent interop) node registry. This is the platform/broker view of a heterogeneous MCU fleet.

- `node_id TEXT PRIMARY KEY`
- `model TEXT` (optional)
- `fw_git_sha TEXT` (optional)
- `caps_sha256 TEXT` (optional; manifest hash)
- `manifest_json TEXT` (optional; UM‑ACDS manifest JSON object string)
- `tags_json TEXT` (optional; JSON array string)
- `tools_json TEXT` (optional; JSON array string of tool names)
- `hardware_presence_json TEXT` (optional; JSON object string)
- `health_json TEXT` (optional; JSON object string)
- `last_auth_seq INTEGER` (optional; best-effort monotonic `auth.seq` anti-replay state for authenticated envelopes)
- `last_hello_utc_ms INTEGER` (optional)
- `last_heartbeat_utc_ms INTEGER` (optional)

Indexes:
- `CREATE INDEX edge_nodes_by_heartbeat ON edge_nodes(last_heartbeat_utc_ms DESC, node_id)`

### `edge_inbox_messages`

Durable inbound UM‑BMP envelopes. Used for dedupe, audit/debug, and replay.

- `msg_id TEXT PRIMARY KEY`
- `ts_utc_ms INTEGER NOT NULL`
- `type TEXT NOT NULL`
- `from_id TEXT` (optional)
- `to_id TEXT` (optional)
- `envelope_json TEXT NOT NULL` (full envelope JSON object string)
- `processed INTEGER NOT NULL DEFAULT 0` (platform applied side effects; dedupe should return early when 1)
- `processed_utc_ms INTEGER` (best-effort processing timestamp)

Indexes:
- `CREATE INDEX edge_inbox_by_type ON edge_inbox_messages(type, ts_utc_ms DESC)`

### `edge_outbox_messages`

Durable outbound UM‑BMP envelopes. Nodes poll via an outbox cursor (`outbox_id`).

- `outbox_id INTEGER PRIMARY KEY AUTOINCREMENT`
- `node_id TEXT NOT NULL`
- `ts_utc_ms INTEGER NOT NULL`
- `envelope_json TEXT NOT NULL`

Indexes:
- `CREATE INDEX edge_outbox_by_node ON edge_outbox_messages(node_id, outbox_id)`

### `edge_tasks`

Durable platform-side task tracking for UM‑BMP tasking (`TASK_ASSIGN`/`TASK_*`).

- `task_id TEXT NOT NULL`
- `step_id TEXT NOT NULL`
- `node_id TEXT NOT NULL`
- `idempotency_key TEXT NOT NULL`
- `trace_id TEXT` (optional; best-effort trace correlation id)
- `resource_lock TEXT` (optional; best-effort platform-side resource lock key)
- `mode TEXT NOT NULL` (`invoke|agent`)
- `tool_name TEXT` (optional; best-effort tool name for `mode=invoke`)
- `deadline_utc_ms INTEGER NOT NULL`
- `payload_json TEXT NOT NULL` (JSON object string)
- `state TEXT NOT NULL` (`QUEUED|RUNNING|SUCCEEDED|FAILED|TIMED_OUT|CANCELED`)
- `created_utc_ms INTEGER NOT NULL`
- `updated_utc_ms INTEGER NOT NULL`
- `result_json TEXT` (optional)
- `result_sha256 TEXT` (optional; best-effort platform-computed sha256 of stored `result_json` bytes)
- `attest_json TEXT` (optional; best-effort node-provided attestation blob, stored as a JSON object string)
- `error TEXT` (optional)

Keys/constraints:
- `PRIMARY KEY(task_id, step_id)`
- `UNIQUE(node_id, idempotency_key)` (platform dispatch dedupe)

Indexes:
- `CREATE INDEX edge_tasks_by_state ON edge_tasks(state, updated_utc_ms DESC)`
- `CREATE INDEX edge_tasks_by_node ON edge_tasks(node_id, updated_utc_ms DESC)`
- `CREATE INDEX edge_tasks_by_tool ON edge_tasks(node_id, tool_name, updated_utc_ms DESC)`
- `CREATE INDEX edge_tasks_by_trace ON edge_tasks(trace_id, updated_utc_ms DESC)`
- `CREATE INDEX edge_tasks_by_node_lock_state ON edge_tasks(node_id, resource_lock, state, updated_utc_ms DESC)`
- `CREATE INDEX edge_tasks_by_result_sha256 ON edge_tasks(result_sha256, updated_utc_ms DESC)`

### `edge_task_events`

Append-only task event log (platform-side). Typically mirrors UM‑BMP `TASK_EVENT` plus terminal markers.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `task_id TEXT NOT NULL`
- `step_id TEXT NOT NULL`
- `ts_utc_ms INTEGER NOT NULL`
- `state TEXT NOT NULL`
- `data_json TEXT NOT NULL` (JSON object string)

Indexes:
- `CREATE INDEX edge_task_events_by_task ON edge_task_events(task_id, step_id, id)`

### `edge_sensor_events`

Durable ingestion of UM‑BMP `SENSOR_EVENT`.

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `node_id TEXT NOT NULL`
- `event_type TEXT NOT NULL`
- `ts_utc_ms INTEGER NOT NULL`
- `confidence REAL` (optional)
- `data_json TEXT NOT NULL` (JSON object string)

Indexes:
- `CREATE INDEX edge_sensor_by_type ON edge_sensor_events(event_type, ts_utc_ms DESC, id)`

### `edge_tool_rate_state`

Platform-side per-node per-tool rate limiter state (best-effort guardrail).

- `node_id TEXT NOT NULL`
- `tool_name TEXT NOT NULL`
- `window_start_utc_ms INTEGER NOT NULL`
- `window_count INTEGER NOT NULL`
- `last_call_utc_ms INTEGER NOT NULL`

Keys/constraints:
- `PRIMARY KEY(node_id, tool_name)`

### `edge_rules`

Event-triggered automation rules: `SENSOR_EVENT` → `TASK_ASSIGN`.

- `rule_id TEXT PRIMARY KEY`
- `enabled INTEGER NOT NULL` (0/1)
- `event_type TEXT NOT NULL`
- `min_confidence REAL NOT NULL`
- `cooldown_ms INTEGER NOT NULL`
- `last_fired_utc_ms INTEGER NOT NULL`
- `action_json TEXT NOT NULL` (JSON object string; supports `{type:"task_assign", ...}` and `{type:"durable_workflow_submit", workflow:{...}}`)
- `created_utc_ms INTEGER NOT NULL`
- `updated_utc_ms INTEGER NOT NULL`

Indexes:
- `CREATE INDEX edge_rules_by_event ON edge_rules(event_type, enabled, updated_utc_ms DESC)`

### `edge_workflows`

Durable edge workflows (UM‑WF executed over UM‑BMP `TASK_ASSIGN`), scheduled by the platform.

- `workflow_id TEXT PRIMARY KEY`
- `goal TEXT` (optional)
- `status TEXT NOT NULL` (`QUEUED|RUNNING|SUCCEEDED|FAILED|CANCELED`)
- `priority INTEGER NOT NULL`
- `spec_json TEXT NOT NULL` (JSON object string; original submitted workflow spec)
- `created_utc_ms INTEGER NOT NULL`
- `updated_utc_ms INTEGER NOT NULL`
- `error TEXT` (optional)

Indexes:
- `CREATE INDEX edge_workflows_by_status ON edge_workflows(status, priority DESC, updated_utc_ms DESC)`

### `edge_workflow_steps`

Durable workflow steps for `edge_workflows`.

- `workflow_id TEXT NOT NULL`
- `step_id TEXT NOT NULL`
- `kind TEXT NOT NULL` (`invoke_tool|run_agent|join`)
- `depends_on_json TEXT NOT NULL` (JSON array string)
- `target_json TEXT NOT NULL` (JSON object string)
- `payload_json TEXT NOT NULL` (JSON object string)
- `join_mode TEXT` (optional; `all|any` for `kind=join`)
- `deadline_utc_ms INTEGER NOT NULL`
- `attempt INTEGER NOT NULL` (dispatch attempts already performed; starts at 0)
- `max_attempts INTEGER NOT NULL` (upper bound; default 1)
- `next_ready_utc_ms INTEGER NOT NULL` (dispatch backoff scheduling; default 0)
- `backoff_ms INTEGER NOT NULL` (base backoff; default 0)
- `state TEXT NOT NULL` (`PENDING|QUEUED|RUNNING|SUCCEEDED|FAILED|TIMED_OUT|CANCELED`)
- `created_utc_ms INTEGER NOT NULL`
- `updated_utc_ms INTEGER NOT NULL`
- `error TEXT` (optional)

Keys/constraints:
- `PRIMARY KEY(workflow_id, step_id)`

Indexes:
- `CREATE INDEX edge_workflow_steps_by_state ON edge_workflow_steps(workflow_id, state, updated_utc_ms DESC)`
- `CREATE INDEX edge_workflow_steps_by_ready ON edge_workflow_steps(workflow_id, state, next_ready_utc_ms, updated_utc_ms DESC)`

### `edge_workflow_events`

Append-only workflow event log (best-effort debug surface; may be expanded later).

- `id INTEGER PRIMARY KEY AUTOINCREMENT`
- `workflow_id TEXT NOT NULL`
- `ts_utc_ms INTEGER NOT NULL`
- `type TEXT NOT NULL`
- `data_json TEXT NOT NULL`

Indexes:
- `CREATE INDEX edge_workflow_events_by_workflow ON edge_workflow_events(workflow_id, id)`

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

## Blob Storage Tiers (Design + Status)

This section defines a future-proof, multi-tier blob storage plan for agentd. The DB remains a metadata index; binary
blobs live in tiered stores with explicit retention, budgets, and deterministic references.

Status (2026-02-19):
- v0 local tier shipped.
- v1 object-store tier shipped (S3/MinIO; presigned reads + proxy mode; optional read-through cache).
- v2 tiering policy engine (promote/evict) is operator-driven via `/api/v1/blob/tier/enforce`.

### Goals

- Support binary artifacts (images/audio/video) and large tool outputs without inflating the SQLite DB.
- Provide deterministic blob identity (hash + size) so evidence bundles and replay workflows are stable.
- Allow multiple storage tiers with policy-driven promotion/eviction (local hot cache → object store → archive).
- Keep reads and streaming safe: range support, size limits, auth, and access logging.
- Preserve operability: simple local-only mode for dev, object-store mode for production.

### Constraints

- DB remains a metadata index, not a binary store.
- Storage must be safe under concurrent writes and crash recovery.
- Security: blobs are always auth-gated; external URLs must be signed and short-lived.
- Retention policies must be explicit and auditable.

### Proposed data model

#### Blob identity

- `blob_id`: `sha256:<hex>` (content-addressed).
- `size_bytes`: integer.
- `mime`: best-effort MIME type.
- `created_utc_ms`: creation time.

#### Suggested DB tables

`blob_manifest` (new):
- `blob_id TEXT PRIMARY KEY`
- `size_bytes INTEGER NOT NULL`
- `mime TEXT`
- `sha256_hex TEXT NOT NULL` (redundant but explicit)
- `created_utc_ms INTEGER NOT NULL`
- `last_access_utc_ms INTEGER`
- `ref_count INTEGER NOT NULL DEFAULT 0`
- `tier TEXT NOT NULL` (`local` | `object` | `archive`)
- `location TEXT NOT NULL` (local path or object key)
- `etag TEXT` (object store)
- `storage_class TEXT` (object store class)

`artifact_blobs` (new join):
- `artifact_id INTEGER NOT NULL`
- `blob_id TEXT NOT NULL`
- `PRIMARY KEY(artifact_id, blob_id)`

Artifacts retain `path` for legacy compatibility, but new records should prefer `blob_id`.

### Tier layout

#### Tier 1: Local file store (default)

- Root: `${state_dir}/blobs/sha256/<aa>/<bb>/<sha256>`.
- Atomic writes: write temp + fsync + rename.
- Safe delete: GC by ref_count and retention policy.
- Upload size cap: enforced via the daemon `upload_max_bytes` limit in v0.

#### Tier 2: Object store (S3/MinIO)

- Key: `blobs/sha256/<aa>/<bb>/<sha256>` (prefix configurable).
- Presigned URLs for clients; agentd can proxy for auth-only environments.
- Optional local cache on read-through (bounded by size).

#### Tier 3: Archive

- Optional cold storage for compliance.
- Read-through may be async with restore delay; surfacing a `restore_pending` state.

### Write path

1) Tool emits artifact → blob bytes stored in local tier (unless object-store mode with cache disabled).
2) Compute hash + size, register in `blob_manifest` (`tier`, `location`, `etag`).
3) Create `artifact_blobs` association.
4) Background mover (future) enforces policies:
   - promote to object store
   - evict local based on LRU, size cap, or TTL

### Read path

- Resolve `blob_id` → tier + location.
- If local: stream with `Range` support.
- If object store:
  - `read_mode=redirect` returns `302` to a signed URL.
  - `read_mode=proxy` streams via agentd.
- Cache on read when `cache_mode=read-through` and size <= `cache_max_bytes`.

### APIs (current)

Read:
- `GET /api/v1/blob?blob_id=...` (supports `Range`)
  - local tier: returns bytes
  - object tier: `302` redirect (default) or proxy stream (see `read_mode`)

Upload:
- `POST /api/v1/blob/upload` (JSON `data_base64` or raw binary)
  - response includes `tier`, `location`, `etag`, and `storage_class` when object-store is enabled

Metadata:
- `GET /api/v1/blob/meta?blob_id=...`
- `POST /api/v1/blob/retain` (adjust ref count; `{blob_id, delta}`)
- `POST /api/v1/blob/gc` (ref-count GC sweep; `{min_age_ms, max_rows, dry_run}`)
- `POST /api/v1/blob/tier/enforce` (apply tiering policy once; safe maintenance endpoint)

Archive controls (v3, operator-only):
- `POST /api/v1/blob/archive` with `{ blob_id | blob_ids, storage_class }` to copy objects into a cold storage
  class (CopyObject) and mark blobs as `tier="archive"`.
- `POST /api/v1/blob/restore` with `{ blob_id | blob_ids, restore_days, restore_tier? }` to request a restore
  (RestoreObject). Reads remain blocked until the restore is ready.

Notes:
- Archive/restore requires `blob_store.mode=object` and configured object-store credentials.
- Archive uses object-store CopyObject to change storage class; `storage_class` must be provided.
- Restore issues an object-store RestoreObject request and does **not** change the tier; reads are allowed only when
  restore status indicates readiness (from `x-amz-restore` in HEAD responses).
- Restore responses include `restore_status` and best-effort `restore_expiry_utc_ms` when available.

These are additive; legacy `GET /api/v1/file` remains supported.

### Retention + GC policy

- Policies by tier, size, MIME, and age.
- Ref-counted GC: remove only when `ref_count == 0` and TTL expired.
- Optional "pin" labels for evidence bundles.

### Observability

- Counters: bytes per tier, objects count, cache hit/miss, promotion/eviction counts.
- Auditable events for GC and tier transitions.
- DB query endpoints: `/api/v1/db/blobs`, `/api/v1/db/blob`, `/api/v1/db/analytics/blobs`.

### Object-store configuration (v1)

Runtime config (JSON / `POST /api/v1/config/update`):
- `blob_store`:
  - `mode`: `local` or `object`
  - `endpoint`: `https://s3.us-east-1.amazonaws.com` or `http://localhost:9000`
  - `region`: AWS region (default `us-east-1`)
  - `bucket`: bucket name
  - `prefix`: object key prefix (default `blobs/sha256`)
  - `path_style`: `true` for path-style bucket addressing (MinIO-friendly)
  - `read_mode`: `redirect` (presigned URL) or `proxy` (agentd stream)
  - `cache_mode`: `read-through` or `none`
  - `cache_max_bytes`: max size to read-through cache (bytes)
  - `presign_ttl_sec`: TTL for signed URLs (seconds, 1..604800)
  - `timeout_ms`: object store timeout
- `blob_store_secrets`:
  - `access_key`, `secret_key`, `session_token` (optional)

Env vars (startup defaults):
- `AGENTD_BLOB_STORE_MODE`, `AGENTD_BLOB_STORE_ENDPOINT`, `AGENTD_BLOB_STORE_REGION`,
  `AGENTD_BLOB_STORE_BUCKET`, `AGENTD_BLOB_STORE_PREFIX`, `AGENTD_BLOB_STORE_PATH_STYLE`,
  `AGENTD_BLOB_STORE_READ_MODE`, `AGENTD_BLOB_STORE_CACHE_MODE`, `AGENTD_BLOB_STORE_CACHE_MAX_BYTES`,
  `AGENTD_BLOB_STORE_PRESIGN_TTL_SEC`, `AGENTD_BLOB_STORE_TIMEOUT_MS`,
  `AGENTD_BLOB_STORE_ACCESS_KEY`, `AGENTD_BLOB_STORE_SECRET_KEY`, `AGENTD_BLOB_STORE_SESSION_TOKEN`.

### Tiering policy engine (v2)

The tiering policy engine is **explicit** and **operator-driven**: it runs only when invoked
via `/api/v1/blob/tier/enforce` (or an operator cron), making it deterministic and easy to audit.

Policies (config defaults):
- `blob_tier.local_max_bytes`: cap total local cache bytes for **object-tier** blobs (0 disables).
- `blob_tier.local_max_age_ms`: evict object-tier local cache older than this age (0 disables).
- `blob_tier.promote_after_ms`: promote **local-tier** blobs to object store when older than this age (0 disables).
- `blob_tier.promote_max_bytes`: per-blob size cap for promotion (0 disables).

Notes:
- Local eviction never deletes **local-tier** blobs (avoids data loss).
- Promotion requires object-store configuration and `blob_store.mode=object`.
- `cache_mode=none` forces eviction of any object-tier local cache discovered.

Endpoint:
`POST /api/v1/blob/tier/enforce` with optional overrides:
```json
{
  "dry_run": false,
  "local_max_bytes": 1073741824,
  "local_max_age_ms": 604800000,
  "promote_after_ms": 86400000,
  "promote_max_bytes": 33554432,
  "max_rows": 5000
}
```

Response (JSON):
- `ok` (boolean)
- `generated_utc_ms` (number)
- `promoted_count`, `promoted_bytes`
- `evicted_count`, `evicted_bytes`
- `total_local_bytes_before`, `total_local_bytes_after`
- `errors` (array, optional)

### Phased delivery

1) **v0 (local-only)**: `blob_manifest` + local store + read endpoint + ref-count GC. (shipped)
2) **v1 (object store)**: S3/MinIO backend + signed URLs + read-through cache. (shipped)
3) **v2 (tiering)**: policy engine + background mover + size budgets.
4) **v3 (archive)**: cold storage restore workflow + operator controls.

### Compatibility

- Existing artifacts with `path` remain valid.
- New artifact JSON should include `blob_id` when available.

## DB Query API (Troubleshooting)

When `agentd` is started with `--db-path` (or `AGENTD_DB_PATH`), it mirrors runs/events/tool records/artifacts into an SQLite DB.
This section defines a small, read-only HTTP surface so operators and the Web UI can query that DB directly for
troubleshooting.

These endpoints are intentionally **not** a stable public API; they are a debugging convenience.

### Goals

- Provide a reliable way to inspect daemon history when:
  - the JSONL audit file is too large to manually inspect
  - the UI wants to show “last runs” / “recent errors”
  - you need to correlate runaway tool loops with artifacts / tool calls
- Keep endpoints read-only and safe:
  - require daemon auth when configured (same as other endpoints)
  - return bounded, paged results

### Additional goals

- Make the DB query API a first-class canonical surface alongside `.sess` / `.events.jsonl`, with a clear migration path.
- Provide advanced analytics endpoints and precomputed aggregates for operators and UIs.

### Endpoints

All endpoints return JSON with:
- `ok` (bool)
- `error` (string, optional)

#### List runs

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

#### Fetch run details

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

#### List workflows

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

#### Fetch workflow details

`GET /api/v1/db/workflow?workflow_id=...&include_tasks=1&include_events=1`

Response fields:
- `workflow` (object) basic workflow fields + `spec_json`/`result_json`
- `tasks` (array, optional) ordered by `updated_unix_ms DESC`
- `events` (array, optional) ordered by `event_id`

#### List workflow tasks

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

#### List workflow events

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

#### List edge workflows

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

#### Fetch edge workflow details

`GET /api/v1/db/edge_workflow?workflow_id=...&include_steps=1&include_events=1`

Response fields:
- `edge_workflow` (object) basic workflow fields + `spec_json`
- `steps` (array, optional) ordered by `updated_utc_ms DESC`
- `events` (array, optional) ordered by `id`

#### List edge workflow steps

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

#### List edge workflow events

`GET /api/v1/db/edge_workflow_events?workflow_id=...&limit=...&offset=...`

Response fields:
- `edge_workflow_events` (array)
  - `id` (number)
  - `workflow_id` (string)
  - `ts_utc_ms` (number)
  - `type` (string)
  - `data_json` (string|null)
  - `data` (object, optional): parsed form of `data_json` when parsing succeeds

#### Workflow analytics aggregates

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

#### Workflow analytics exports (CSV/JSON)

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

#### Edge analytics aggregates

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

#### Edge analytics exports (CSV/JSON)

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

#### List artifacts

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

#### List UI actions

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

#### List DB sessions (most recently updated)

`GET /api/v1/db/sessions?limit=...&offset=...`

Response fields:
- `sessions` (array)
  - `session_id` (string)
  - `created_unix_ms` (number)
  - `updated_unix_ms` (number)

#### List session messages (DB mirror)

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

#### List UI client events

`GET /api/v1/db/client_events?session_id=...&limit=...&offset=...`

Response fields:
- `client_events` (array)
  - `id` (number)
  - `ts_unix_ms` (number)
  - `type` (string)
  - `data_json` (string|null)
  - `data` (object, optional): parsed form of `data_json` when parsing succeeds

### Notes

- If the DB is disabled, endpoints return `404` (not found) or `{ok:false,error:"db disabled"}` depending on call site.
- For richer queries, use `sqlite3` directly against `db_path` shown in `/api/v1/config`.
