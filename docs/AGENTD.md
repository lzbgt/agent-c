# agentd (daemon) Operations

Date: 2026-02-19

This guide covers local daemon usage, runtime configuration, and the main HTTP/SSE surfaces.
For production hardening, see `docs/DEPLOYMENT.md`.

## Run the daemon (local)

```bash
./build/agentd --host 127.0.0.1 --port 8123
```

Safety:
- `agentd` refuses to bind to non-loopback hosts unless `--auth-token` is set.
- Override (insecure): `./build/agentd --host 0.0.0.0 --port 8123 --allow-unauth`

Proxy override:

```bash
./build/agentd --host 127.0.0.1 --port 8123 --proxy http://localhost:8120
```

SQLite DB (canonical daemon state):

```bash
./build/agentd --host 127.0.0.1 --port 8123

# or pin it explicitly (recommended for production / multi-daemon):
./build/agentd --host 127.0.0.1 --port 8123 --db-path "$HOME/.agent/agentd.db"
```

The DB stores sessions, runs, events, tool records, artifacts, UI actions, client events, and audit records.
See `docs/DB.md` for query examples.

Health check:

```bash
curl http://127.0.0.1:8123/api/v1/health
curl http://127.0.0.1:8123/api/v1/ready
```

## Tool exposure: YOLO vs host policy

- Default daemon mode is YOLO (unrestricted) to match local development needs.
- `yolo` is now a tool exposure knob (primarily enabling/disabling process execution tools). It does not sandbox filesystem paths.
- For safer deployments, `agentd` supports `--host-policy full|readonly`:
  - `full`: enables process exec + patch application + filesystem inspection (`shell_exec`, `proc_exec`, `file_apply_patch`, `fs_*`, `text_search`)
  - `readonly`: disables process exec and patch application (keeps only `fs_*` + `text_search`)
  - Requests can also pass `host_policy: "readonly"` to tighten permissions for that run (cannot expand beyond daemon default).

## Daemon-side key loading and client identity

- `agentd` supports **daemon-side** key loading per-run based on the request `base_url`, so WebUI can omit `api_key`.
- The WebUI sends a `client` identity with each run (e.g. `client.kind="webui"`). `agentd` can use this to inject a
  client-specific system prompt profile for default presentation/DoD semantics. See `docs/CLIENT.md`.

## Verbose inspection

- Pass `verbose: true` to `POST /api/v1/run` to return a structured `events` log suitable for UIs
  (LLM request/response, tool calls, tool results).
- Pass `trace: true` to also return a plain-text transcript (`trace_text`).
- Optional: pass `max_capture_bytes` to cap large verbose event payloads for UI stability (default: 256KB; daemon clamps for tool loops).
- `agentd` ignores `SIGPIPE` so client disconnects (UI refresh, SSE close) do not terminate the daemon.

File serving:
- `GET /api/v1/file?path=<...>&session_id=<sid>` serves files for previews (up to 10MB).
  - If `session_id` is provided and `path` is relative, the file is resolved under `<sessions_root>/<sid>/...`.
  - If `path` is absolute, it is served directly.

## Assistant streaming (provider-dependent)

Clients can set `stream_assistant: true` to request OpenAI-compatible SSE streaming (`stream: true`):
- `tools: "none"`: the daemon emits `assistant_delta` events while the assistant message is streaming.
- `tools: "basic"` / `"host"`: streaming requests are used for tool-loop steps and (best-effort) tool-call reconstruction
  from streamed `delta.tool_calls` (and legacy `delta.function_call`). It also emits `assistant_delta` events during the
  final assistant step (provider-dependent).

Some providers ignore `stream: true` and return a normal JSON completion; the daemon falls back to non-stream parsing.
Implementation notes: `docs/STREAMING.md`.

## Tool schema introspection (extensible tools)

`GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=<id>` returns the tool registry the
daemon will expose: `name`, `description`, `parameters_json` (OpenAI-compatible JSON Schema).

Notes:
- Tool exposure is constrained by the daemon's `--host-policy` (response includes `effective_host_policy`).
- Requests cannot exceed the daemon's `--tools` setting; exceeding it returns HTTP 400.
- The response includes `daemon_tools` and (when provided) `requested_tools` for clarity.
- When `session_id` is provided (and `tools=host`), the registry may include session-scoped tools such as `ui_wait_event`.

## OpenRouter model discovery

`GET /api/v1/openrouter/models?min_total=0.01&max_total=0.50&require_multimodal_input=1&require_tools=1&limit=50`
fetches OpenRouter’s `/models` catalog (using `OPENROUTER_API_KEY` or `Authorization: Bearer ...`),
filters it, sorts by total price ($/1M prompt+completion), and returns a recommended cheapest model id.

## Session browsing

- `GET /api/v1/sessions` lists known sessions.
- `GET /api/v1/session?session_id=<id>` returns the message history.
- `GET /api/v1/session/audit?session_id=<id>&include_rotated=0|1` returns recent per-run audit entries.

## Async runs (UI-friendly)

- `POST /api/v1/run_async` starts a background run and returns `{ ok, job_id }`.
- `GET /api/v1/job?job_id=<id>` returns `{ ok, status, result? }` (result shape matches `/api/v1/run`).
  - Add `include_events=1` to include the tool/LLM `events` captured so far (best-effort).
  - Use `cursor=<n>&max_events=<m>` to tail new events while the job is running.
- `GET /api/v1/job/stream?job_id=<id>&cursor=<n>` streams job progress via SSE:
  - emits `agent_event` (same objects as the `events` array)
  - ends with `job_done` containing the final `result`
- While jobs are running, the daemon may emit `heartbeat` events when no other events have been produced for a short period.

## Job cancellation (best-effort)

- `POST /api/v1/job/cancel?job_id=<id>` requests cancellation.
- Cancellation is cooperative:
  - the tool loop stops at safe boundaries (between tool calls / LLM requests)
  - long-running host tools (`shell_exec` / `proc_exec`) terminate their subprocess on cancellation

## Daemon config snapshot (debug)

For debugging client/daemon mismatches (CORS, sandbox defaults, job GC), `agentd` exposes:
- `GET /api/v1/config`

This endpoint requires auth when `--auth-token` is set. It intentionally does not include secrets.
The WebUI surfaces this snapshot in Settings as “Daemon config”, including `state_dir`, `sessions_root_dir`,
and `db_path` (SQLite; canonical daemon state store).

## Update daemon defaults at runtime

`agentd` supports updating defaults at runtime (persisted in SQLite at `db_path`) so the browser UI does not need to store keys.

Endpoint:
- `POST /api/v1/config/update`

Example (set default provider + model):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"base_url":"https://api.deepseek.com","model":"deepseek-chat"}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Example (store a provider key server-side; do **not** do this over an unauthenticated network):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"provider":"deepseek","api_key":"sk-REDACTED"}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Notes:
- The response never includes secrets. Use `GET /api/v1/config` to see booleans like `provider_keys_set`.
- The WebUI exposes Settings buttons to “Save defaults to daemon” and “Save API key to daemon”.

## State dir / multi-agent safety

`agentd` stores canonical state in SQLite (`--db-path`, default: `<state_dir>/agentd.db`; `state_dir` defaults to the
daemon working directory or `AGENTD_STATE_DIR`). If you run multiple daemons, use a distinct DB per instance:

```bash
./build/agentd --db-path /tmp/agentd_state_1.db
./build/agentd --db-path /tmp/agentd_state_2.db
```

Legacy (non-canonical) session-store directories are configurable for backwards compatibility and cleanup:

```bash
./build/agentd --sessions-root /tmp/agentd_sessions
```

Environment overrides:
- `AGENTD_STATE_DIR`
- `AGENTD_SESSIONS_ROOT`
- `AGENTD_DB_PATH`
- `AGENTD_ACCESS_LOG` (`1` for text logs, `json` for JSON logs)
- `AGENTD_HTTP_MAX_BODY_BYTES` (default `67108864`, `0` to disable limit)
- `AGENTD_HTTP_MAX_HEADER_BYTES` (default `1048576`, `0` to disable limit)
- `AGENTD_HTTP_READ_TIMEOUT_MS` (default `15000`, `0` to disable read timeouts)
- `AGENTD_UPLOAD_MAX_BYTES` (per-file upload cap, default `33554432`, `0` to disable cap)
- `AGENTD_MAX_TOOL_CALL_ARGS_CHARS_DEFAULT` (default `0`, disables tool-arg length guard)

## WebUI

For WebUI dev/build/runtime config and reliability notes, see `docs/WEBUI.md`.
