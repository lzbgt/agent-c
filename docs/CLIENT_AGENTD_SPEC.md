# Client ↔ agentd API + Collaboration Spec (Living)

Date: 2026-02-01

This document is a **quick reference** for implementing new clients (mobile app, Slack bot, CLI front-end, etc.)
that talk to `agentd` over HTTP.

It focuses on:

- the HTTP API surface (`/api/v1/...`)
- how clients identify themselves
- how clients and the agent coordinate via events
- how the **durable Scene** works (server-owned, persisted in the DB)

For deeper dives, see the linked draft docs:

- `docs/PROTOCOL.md` (overall UI↔agentd protocol; runs, artifacts, tools)
- `docs/CLIENT_COLLAB.md` (event-driven collaboration + DoD patterns)
- `docs/CLIENT_ENTITIES.md` (entity/scene abstraction for client-agnostic UI)
- `docs/UI_ACTION.md`, `docs/UI_CLIENT_EVENTS.md`, `docs/UI_WAIT_EVENT.md` (concrete event/tool schemas)

## Terminology

- **client**: any UI/integration that drives the daemon (WebUI, Slack, mobile).
- **session**: the namespace for message history + durable Scene + client event log.
- **run**: one LLM prompt execution (sync or async).
- **event**: structured log record; used for artifacts, UI actions, client acknowledgements, etc.
- **Scene (durable)**: a per-session, server-owned JSON object mapping `entity_id -> entity`.

## Authentication

Most endpoints call `daemon_require_auth(...)`.

- If `agentd` is started **without** an auth token (`cfg.auth_token` empty), requests are accepted without auth.
- If `agentd` is started **with** an auth token, clients must send:
  - `Authorization: Bearer <token>`

The health endpoint is always unauthenticated:
- `GET /api/v1/health`
- `GET /api/v1/ready`
- `GET /metrics`

Request correlation:
- Clients may send `X-Request-Id` (safe ASCII token). The server will echo it back in responses.
- If omitted, agentd generates a request id and returns it in `X-Request-Id`.
- Streaming endpoints also echo `X-Request-Id` in their initial SSE response headers.
- For `/api/v1/run` and `/api/v1/run_async`, clients may send `X-Trace-Id`. If the JSON body omits `trace_id`,
  the daemon will use the header value (must pass `trace_id_is_safe`).
 - When `X-Trace-Id` is present, agentd echoes it in response headers for easier log correlation.

## Session IDs (safety contract)

`session_id` is treated like a filename key. It must pass `session_id_is_safe()`:

- length: `1..200`
- characters: `[A-Za-z0-9_.-]` only
- must not contain `/`, `\\`, `".."`, `"."`, or `".."`-like traversal

Clients should treat invalid IDs as a client-side bug (don’t retry).

## Common Response Pattern

Most JSON responses follow:

```json
{"ok": true, "...": "..."}
```

On failure:

```json
{"ok": false, "error": "human-readable error string"}
```

HTTP status codes are meaningful:
- `400` for invalid/missing arguments
- `401` for auth failures (when auth is enabled)
- `500` for server/database failures

## Endpoint Catalog (HTTP)

This list matches daemon route registration in `daemon/src/agentd_api.cpp`.

### Service / Config

- `GET /api/v1/health`
  - returns `{ ok, service:"agentd", version:"0.1", now_unix_ms, uptime_ms }`
- `GET /api/v1/ready`
  - returns `{ ok, ready, service:"agentd", version:"0.1", now_unix_ms, uptime_ms, checks:{db_open} }`
- `GET /metrics`
  - Prometheus text format (uptime, readiness, DB status, unix time)
- `GET /api/v1/config`
  - returns daemon config **without secrets** (booleans like `api_key_set` / `provider_keys_set`)
- `POST /api/v1/config/update`
  - persists runtime defaults (model/base_url/proxy/timeout + provider keys) into the DB
- `POST /api/v1/ota/update`
  - triggers an OTA update via operator-configured command (see `docs/spec/ota/agentd_ota_v0.md`)
  - request JSON: `{ url, sha256?, version?, reason?, drain_timeout_ms?, trace_id? }`
- `GET /api/v1/ota/status`
  - returns `{ ok, status, ota_id?, updated_unix_ms?, last_error?, plan_path?, drain_active?, drain_until_unix_ms?, drain_reason? }`
    plus best-effort inflight counts (`jobs_running`, `jobs_queued`, `workflow_tasks_running`,
    `workflow_tasks_queued`, `workflows_running`) when DB is available.

### Tools + Files

- `GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=...`
  - returns the effective tool registry (`defs[]` with `name`, `description`, `parameters_json`)
- `GET /api/v1/file?path=...&session_id=...`
  - serves a (size-capped) file for UI preview (image/audio/video/text)

Notes:
- `yolo` no longer controls filesystem path restrictions. It only affects which *tools* are exposed (notably process execution tools like `shell_exec` / `proc_exec`).
- Requests cannot exceed the daemon’s `--tools` ceiling; exceeding it returns HTTP 400.
- The response includes `daemon_tools` and (when provided) `requested_tools` for clarity.
- Requires auth when `--auth-token` is enabled.
- Session folder root:
  - The daemon uses a single base directory for state + session folders. You can set it explicitly with env `AGENT_WD` (operator-friendly) or `AGENTD_STATE_DIR` / `AGENTD_SESSIONS_ROOT`.
  - If not set, it defaults to the daemon process **startup working directory**.
- File path resolution:
  - If `path` is absolute, it is served directly.
  - If `path` is relative and `session_id` is provided, it is resolved relative to the session directory (`<sessions_root>/session_<session_id>/...`).
  - If `path` is relative and `session_id` is omitted, it is resolved relative to the daemon process working directory.
- Host tool default working directory (important for client implementers):
  - When `session_id` is set, host exec tools (`shell_exec`, `proc_exec`) run with default `cwd` = the session root directory (`<sessions_root>/session_<session_id>/`).
  - Convention:
    - write intermediate files under `work/`
    - write user-facing artifacts under `out/` and then call `artifact_register` with `path: "out/<file>"`.

### Memory (durable + deterministic)

- `POST /api/v1/memory/consolidate`
  - promotes explicit `@mem` markers from daily logs into `STRUCTURED.md`
- `POST /api/v1/memory/retention/enforce`
  - deterministic retention (daily logs + checkpoints); supports `dry_run`
- `GET /api/v1/memory/checkpoints`
  - lists rolling structured memory snapshots with sha256
- `GET /api/v1/memory/index`
  - lightweight index of memory files (size/line/token estimates)
- `GET /api/v1/memory/correlate`
  - trace_id correlation over structured checkpoints (bounded)
- `GET /api/v1/memory/query`
  - deterministic query over newest structured checkpoint (bounded)

### Sessions (messages / audit / artifacts / client events)

- `GET /api/v1/sessions`
  - returns `{ ok, sessions:[...] }`
- `POST /api/v1/session/new`
  - request JSON: `{ session_id?, create_files? }`
    - `create_files` is a legacy name; currently it means “eagerly create the session row in the DB” (by writing empty messages).
  - response: `{ ok, session_id, created }`
- `GET /api/v1/session?session_id=...`
  - returns `{ ok, session_id, messages:[{role,content,mm_json?,mm_bytes?,mm_truncated?}, ...] }`
- `DELETE /api/v1/session?session_id=...`
  - deletes session data from the DB (and attempts best-effort legacy file cleanup)
- `POST /api/v1/session/upload`
  - request JSON: `{ session_id, files:[{name, mime?, data_base64}, ...] }`
  - response JSON: `{ ok, session_id, files:[...], errors? }`
  - per-file cap: `--upload-max-bytes` or `AGENTD_UPLOAD_MAX_BYTES` (decoded bytes; default 32 MiB; `0` disables)
  - when no files are accepted, returns `ok=false` and HTTP 400 with `errors[]`
- `GET /api/v1/session/audit?session_id=...&max_bytes=...`
  - returns per-run audit entries (parsed JSON objects)
- `GET /api/v1/session/artifacts?session_id=...&max_bytes=...&max_artifacts=...`
  - returns recent artifacts derived from DB-backed artifact records

### Client events (client → agentd)

These two endpoints currently share the same handler and semantics:

- `POST /api/v1/session/client_event`
- `POST /api/v1/session/ui_event` (legacy alias; same behavior)

Request JSON (shape):

```json
{
  "session_id": "sess-...",
  "type": "artifact_rendered",
  "ts_unix_ms": 1730000000000,
  "client": { "id": "slack", "kind": "slack", "instance_id": "workspace-A" },
  "data": { "tool_call_id": "call_123", "path": "out/foo.wav" },
  "append_to_session": true
}
```

Notes:
- `ts_unix_ms` is optional; if omitted, the daemon sets it to “now”.
- `client` is optional but strongly recommended for multi-client debugging.
- `append_to_session` default is `true`:
  - when true, the daemon appends a synthetic `role="user"` message of the form:
    - `[client_event] { ...json payload... }`
  - this makes the event visible to the LLM via session history

Client fields (`client.id`, `client.kind`, `client.instance_id`) are bounded:
- max length `200`
- must not contain control characters

Read-back endpoints for debugging:

- `GET /api/v1/session/client_events?session_id=...&max_bytes=...`
  - returns `{ ok, events:[{type,ts_unix_ms,client?,data}, ...] }`
- `GET /api/v1/session/clients?session_id=...&max_bytes=...`
  - returns `{ ok, clients:[{id,kind?,instance_id?,last_ts_unix_ms,last_type?}, ...] }`

### Durable Scene (server-owned, refresh-proof)

The durable Scene is stored in the daemon DB and is the **post-refresh source of truth** for UIs that render a Scene panel.

- `GET /api/v1/session/scene?session_id=...`
  - returns `{ ok, session_id, updated_unix_ms, scene }`
  - `scene` is an object mapping `entity_id -> entity object`

- `POST /api/v1/session/scene/apply`
  - request JSON: `{ session_id, ops:[...] }`
  - response JSON:
    - `{ ok, session_id, updated_unix_ms, apply, scene }`
    - `apply` contains per-op results (`ok`, `op`, ids, errors, etc.)

If the DB is not available, these endpoints return `{ ok:false, error:"db not available" }` with HTTP 500.

### Runs / Jobs

- `POST /api/v1/run`
  - sync run; response can include `assistant_text` and (optionally) `events[]` if `verbose=true`
  - accepts optional `trace_id` (client-provided correlation id); if omitted, daemon generates one
  - returns `{ ..., trace_id }` and each `events[]` entry includes `trace_id`
- `POST /api/v1/run_async`
  - returns `{ ok, job_id, trace_id }` and runs in the background
  - if request omitted `trace_id`, daemon generates one and uses it consistently for:
    - SSE job events (`/api/v1/job/stream`)
    - final job result JSON (`/api/v1/job`)
- `GET /api/v1/job?job_id=...`
  - returns `{ ok, status, trace_id?, result? }` (`trace_id` present for in-memory jobs)
- `POST /api/v1/job/cancel?job_id=...`
  - requests cancellation
- `DELETE /api/v1/job?job_id=...`
  - deletes job state (if supported by the server build)

Some daemon builds also expose:
- `GET /api/v1/job/stream?job_id=...&cursor=...` (SSE for incremental job progress)
  - `agent_event.data` may be truncated for memory safety; when truncated the event includes
    `data_truncated`, `data_bytes`, and `data_bytes_kept`.

Debug/correlation helper:
- `GET /api/v1/trace?trace_id=...`
  - best-effort lookup of persisted records for a `trace_id`
  - includes durable run/workflow audit records (`audit_records`)
  - also includes best-effort edge interop records (edge task events + inbound UM‑BMP envelopes) when present
  - note: only runs with `no_session=false` appear in `audit_records` (DB-backed)

See `docs/STREAMING.md` for streaming semantics.

Tools ceiling:
- Run requests cannot exceed the daemon’s `--tools` setting (e.g., a daemon started with `--tools basic` will reject `tools:"host"`).
  Such requests return HTTP 400.

Reconnect / continuity guidance:
- `agentd` should continue executing an **async** job even if a client disconnects (tab refresh, app restart, network flap).
  - Only an explicit `POST /api/v1/job/cancel?job_id=...` should stop the job.
- Clients that want refresh-stable UX should persist:
  - `session_id` (the conversation namespace)
  - `job_id` (if an async run is active)
  - `cursor` (if consuming SSE/job tail incrementally)
- On reconnect, a client can:
  - `GET /api/v1/job?job_id=...` to get current status and (when done) the final result
  - resume `/api/v1/job/stream?job_id=...&cursor=...` or polling (`include_events=1`) to render progress
  - `GET /api/v1/session/audit?session_id=...` to reconstruct history if needed

### DB query endpoints (optional; read-only)

When DB support is enabled, the daemon exposes read-only debugging endpoints:

- `GET /api/v1/db/sessions`
- `GET /api/v1/db/messages?session_id=...&max_mm_bytes=...`
  - includes `mm_json/mm_bytes/mm_truncated` alongside message text
- `GET /api/v1/db/runs?session_id=...`
- `GET /api/v1/db/run?...`
- `GET /api/v1/db/artifacts?session_id=...`
- `GET /api/v1/db/ui_actions?session_id=...`
- `GET /api/v1/db/client_events?session_id=...`
- `GET /api/v1/db/blobs?limit=...&offset=...&tier=...`
- `GET /api/v1/db/blob?blob_id=...&include_artifacts=1`
- `GET /api/v1/db/analytics/blobs?top_mime_limit=...`
- `GET /api/v1/db/workflows?status=...&session_id=...&trace_id=...`
- `GET /api/v1/db/workflow?workflow_id=...`
- `GET /api/v1/db/workflow_tasks?workflow_id=...`
- `GET /api/v1/db/workflow_events?workflow_id=...`
- `GET /api/v1/db/edge_workflows?status=...`
- `GET /api/v1/db/edge_workflow?workflow_id=...`
- `GET /api/v1/db/edge_workflow_steps?workflow_id=...`
- `GET /api/v1/db/edge_workflow_events?workflow_id=...`
- `GET /api/v1/db/analytics/workflows?scope=all`
- `GET /api/v1/db/analytics/workflows/export?format=json|csv&scope=all|durable|edge`
- `GET /api/v1/db/analytics/edge?active_within_ms=...`
- `GET /api/v1/db/analytics/edge/export?format=json|csv&scope=all|edge_tasks|edge_nodes`

These are intended for troubleshooting and UI indexing, not for core client functionality.

### Blob endpoints (optional; local + object-store tiers)

When blob storage is enabled (default local tier), the daemon exposes blob APIs:

- `POST /api/v1/blob/upload` (JSON with `data_base64`, or raw bytes; capped by `upload_max_bytes`)
- `GET /api/v1/blob?blob_id=...` (supports `Range`; object-store mode may redirect or proxy)
- `GET /api/v1/blob/meta?blob_id=...`
- `POST /api/v1/blob/retain` (adjust ref count)
- `POST /api/v1/blob/gc` (ref-count GC sweep)
- `POST /api/v1/blob/tier/enforce` (apply tiering policy once; operator/maintenance use)
- `POST /api/v1/blob/archive` (copy blobs to archive storage class; operator-only)
- `POST /api/v1/blob/restore` (request restore for archived blobs; operator-only)

### Diagnostics endpoints (auth required)

The daemon exposes lightweight diagnostics endpoints for operational checks and provider key visibility:

- `GET /api/v1/diagnostics`
  - returns `{ ok, ready, db, jobs, workflows, warnings? }`
  - includes DB table counts and workflow scheduler stats when DB is enabled
- `GET /api/v1/diagnostics/providers`
  - returns provider key presence + source metadata (config/env/file), base URL, and model defaults
- `POST /api/v1/diagnostics/provider_test`
  - runs a small provider test (no session) and returns `{ ok, provider, model, duration_ms, assistant_text?, error? }`
  - accepts optional overrides: `base_url`, `model`, `prompt`, `expect`, `tools`, `require_tool_call`, plus tool-loop limits

## Scene Management Spec (Durable Scene)

### Scene state schema (server-side)

`scene` is a JSON object mapping `entity_id` to an entity:

```json
{
  "ent-1": {
    "id": "ent-1",
    "kind": "canvas2d",
    "title": "Plot",
    "props": { "width": 640, "height": 240 },
    "created_ms": 1730000000000,
    "updated_ms": 1730000000500
  }
}
```

Entity fields:
- `id` (string): stable identifier (key in the map)
- `kind` (string): entity type (`canvas2d`, `dom`, `artifact`, …)
- `title` (string, optional)
- `props` (object, optional): client-defined JSON data
- `created_ms` / `updated_ms` (int64): unix ms timestamps (set by daemon when applying ops)

### Apply ops schema

`POST /api/v1/session/scene/apply` accepts an array of ops. Supported ops:

- `create`
  - `{ op:"create", id?, entity_kind, title?, props? }`
  - `id` is optional; if omitted, the daemon generates one
- `update`
  - `{ op:"update", id, props }`
  - merges `props` shallowly into existing `props`
- `delete` / `remove`
  - `{ op:"delete", id }`
- `action`
  - `{ op:"action", id, action, args? }`
  - daemon records this as `props.last_action = { name, args, ts_unix_ms }`
- `clear` (destructive)
  - `{ op:"clear", entity_kind? }`
  - removes all entities (or only matching `entity_kind` if provided)

Each op yields a result record in `apply.results[]`:
- successful ops include `ok:true`, `op`, and ids/counts
- failed ops include `ok:false` and `error`

The daemon persists the updated scene state even if some ops fail.

### Safety guidance (for clients)

The durable Scene is persisted in the daemon DB; destructive operations affect what users see after refresh.

Client guidance:
- Prefer **targeted delete** (`{op:"delete", id:"..."}`) over any global wipe.
- Prefer creating a **new session** over wiping a session’s durable Scene.
- If you implement `clear`, gate it behind explicit confirmation / debug mode.

WebUI policy (current):
- the WebUI frontend disables sending `clear` ops (including removing the one-click “Clear Scene” button).

## Minimal client implementation checklist

To get a new client working end-to-end:

1. Choose/create a session:
   - `POST /api/v1/session/new` and store `session_id`
2. Start runs:
   - `POST /api/v1/run` (simple) or `POST /api/v1/run_async` + `GET /api/v1/job`
3. Render artifacts and UI actions:
   - parse `events[]` (when `verbose=true`) and/or poll audit/artifacts endpoints
   - fetch artifact bytes via `/api/v1/file?session_id=...&path=...` (or `/api/v1/blob?blob_id=...` when provided)
4. Post acknowledgements as client events:
   - `POST /api/v1/session/client_event` with `{type, client, data}`
5. If you render a Scene:
   - `GET /api/v1/session/scene` to hydrate on startup/refresh
   - `POST /api/v1/session/scene/apply` to persist durable updates (create/update/delete/action)
