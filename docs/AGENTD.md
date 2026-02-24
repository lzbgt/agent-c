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

## Auth (optional)

If you bind `agentd` to non-loopback (e.g. `--host 0.0.0.0`), you must set an auth token
(agentd refuses to start otherwise):

```bash
./build/agentd --auth-token "your_token"
```

Clients must send `Authorization: Bearer your_token` to all endpoints (except `/api/v1/health`, `/api/v1/ready`, and `/metrics`).
You can also accept the token via a cookie name (useful for browser clients):

```bash
./build/agentd --auth-token "your_token" --auth-cookie "agentd_auth"
```

## Daemon CORS (browser clients)

The WebUI typically runs on a different origin (e.g. `http://localhost:5173`) than the daemon (`http://127.0.0.1:8123`),
so `agentd` emits CORS headers for browser fetches:

- When binding to loopback (`127.0.0.1` / `localhost`): CORS defaults to `Access-Control-Allow-Origin: *`.
- When binding to a non-loopback host: CORS is disabled by default. Enable it explicitly with `--cors-origin`.

Example (remote UI origin allowlist):

```bash
./build/agentd --host 0.0.0.0 --auth-token "your_token" --cors-origin "https://your-ui.example"
```

If you use cookie auth, allow credentials so the browser can send cookies:

```bash
./build/agentd --host 0.0.0.0 --auth-token "your_token" --auth-cookie "agentd_auth" \
  --cors-origin "https://your-ui.example" --cors-allow-credentials
```

Per-route origin policies (longest path prefix wins):

```bash
./build/agentd --cors-route '{"path_prefix":"/api/v1/openrouter","origins":["https://ui.example"]}'
```

By default, `agentd` allows common headers needed by the UI, including `Authorization` (daemon auth), `X-OpenRouter-Key`,
and request correlation headers `X-Request-Id` / `X-Trace-Id` (also exposed for browser access).

## Secrets and local key files

For local development, you can store provider keys in a gitignored file named `.not_in_repo` at the repo root.
This keeps keys out of the UI/browser and lets `agentd` load them automatically.

Accepted formats (either style; one per line):

```text
DEEPSEEK_API_KEY=sk-...
OPENROUTER_API_KEY=sk-...
KIMI_API_KEY_CN=sk-...
MOONSHOT_API_KEY=sk-...
```

or:

```text
- deepseek: sk-...
- openrouter: sk-...
```

Notes:
- `.not_in_repo` is gitignored; do not commit keys.
- `agentd` prefers `.not_in_repo` over `project.local.md` when auto-loading provider keys.
- To set keys via a local file, copy `project.local.md.example` to `project.local.md` and fill in real values.
- You can point `agentd` at a specific dotenv file with `AGENTD_DOTENV_PATH=/path/to/.env`.
- `~/.env` is a last-resort key source only; base URLs still come from flags/env/config.
  For Moonshot, set `MOONSHOT_API_BASE` or pass `--base-url https://api.moonshot.cn/v1`.

## Durable memory

`agentd` maintains a durable memory directory under `state_dir/memory/` and exposes host tools
(`memory_write/get/search/put`) to persist and retrieve long-lived facts/preferences/tasks.
See `docs/MEMORY.md`.

## Job GC (longevity)

`agentd` keeps async job state in memory for UI progress streaming. Finished jobs are garbage-collected:
- `--job-ttl-ms <n>`: remove done/error jobs older than `n` ms (default: 1800000)
- `--max-jobs <n>`: keep at most `n` jobs in memory (default: 256)

## Tool exposure: YOLO vs host policy

- Default daemon mode is YOLO (unrestricted) to match local development needs.
- `yolo` is now a tool exposure knob (primarily enabling/disabling process execution tools). It does not sandbox filesystem paths.
- For safer deployments, `agentd` supports `--host-policy full|readonly`:
  - `full`: enables process exec + patch application + filesystem inspection (`shell_exec`, `proc_exec`, `file_apply_patch`, `fs_*`, `text_search`)
  - `readonly`: disables process exec and patch application (keeps only `fs_*` + `text_search`)
  - Requests can also pass `host_policy: "readonly"` to tighten permissions for that run (cannot expand beyond daemon default).

## Policy hooks (allow/deny + budget caps)

`agentd` can apply deterministic policy hooks to tool runs for auditing and enforcement:

Modes:
- `off`: disabled.
- `audit`: record `policy_decision` events but do **not** enforce allow/deny or caps.
- `enforce`: deny tools and cap budgets.

Flags:
- `--policy-mode off|audit|enforce`
- `--policy-tool-allow <csv>` (tool names; comma-separated, repeatable)
- `--policy-tool-deny <csv>` (tool names; comma-separated, repeatable)
- `--policy-max-steps <n>`
- `--policy-max-tool-calls-total <n>`
- `--policy-max-tool-calls-per-tool <n>`
- `--policy-max-tool-call-args-chars <n>`
- `--policy-max-tool-result-chars <n>`
- `--policy-approval-tools <csv>` (tool names requiring approvals)
- `--policy-approval-required <n>` (approvals required; default: 1)
- `--policy-approval-roles <csv>` (optional role allowlist)
- `--policy-approval-timeout-ms <n>` (default: 300000)
- `--policy-approval-poll-ms <n>` (default: 500)

Env overrides:
- `AGENTD_POLICY_MODE`
- `AGENTD_POLICY_TOOL_ALLOWLIST`
- `AGENTD_POLICY_TOOL_DENYLIST`
- `AGENTD_POLICY_MAX_STEPS`
- `AGENTD_POLICY_MAX_TOOL_CALLS_TOTAL`
- `AGENTD_POLICY_MAX_TOOL_CALLS_PER_TOOL`
- `AGENTD_POLICY_MAX_TOOL_CALL_ARGS_CHARS`
- `AGENTD_POLICY_MAX_TOOL_RESULT_CHARS`
- `AGENTD_POLICY_APPROVAL_TOOLS`
- `AGENTD_POLICY_APPROVAL_REQUIRED`
- `AGENTD_POLICY_APPROVAL_ROLES`
- `AGENTD_POLICY_APPROVAL_TIMEOUT_MS`
- `AGENTD_POLICY_APPROVAL_POLL_MS`

Runtime config (`/api/v1/config`):

```json
{
  "policy": {
    "mode": "audit",
    "tool_allowlist": ["memory_write", "text_search"],
    "tool_denylist": ["shell_exec"],
    "max_steps": 32,
    "max_tool_calls_total": 64,
    "max_tool_calls_per_tool": 8,
    "max_tool_call_args_chars": 4000,
    "max_tool_result_chars": 8000,
    "approval_tools": ["shell_exec"],
    "approval_required": 2,
    "approval_roles": ["security", "reviewer"],
    "approval_timeout_ms": 300000,
    "approval_poll_ms": 500
  }
}
```

Policy decisions emit `policy_decision` events in the run/job event streams for auditability.

### Policy approvals (tool-level quorum)

- Approvals gate tool execution for tools in `approval_tools`.
- In `enforce` mode, the tool loop waits until approvals resolve or time out (requires SQLite DB).
- In `audit` mode, approvals emit `approval_request` + `approval_resolved` events but do not block.
- Approval lifecycle events: `approval_request`, `approval_update`, `approval_resolved`.

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

## Approval queue API

- `GET /api/v1/approvals?status=pending&trace_id=...&job_id=...&tool_name=...&run_id=...&limit=100`
  - Lists approvals (most recent first). Filters are optional.
- `GET /api/v1/approvals/<approval_id>`
  - Returns a single approval with its decisions array.
- `POST /api/v1/approvals/<approval_id>/decisions`
  - Body: `{ "member_id": "...", "decision": "approve|deny", "note": "..." }`

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

## Client prefs (WebUI connection profiles)

The daemon can persist WebUI connection profiles so they survive browser resets:

- `GET /api/v1/client/prefs?client_id=<id>&client_kind=webui`
- `POST /api/v1/client/prefs` with `{ client_id, client_kind, prefs }`

Notes:
- Requires auth when `--auth-token` is set.
- v1 stores **non-secret** connection fields only (URLs/agent ids). Tokens stay client-side.

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
- `AGENTD_RUN_ATTEST_HMAC_KID` (optional; signs `/api/v1/run/attestation` when paired with key)
- `AGENTD_RUN_ATTEST_HMAC_KEY` (optional; HMAC key used to sign attestation bundles)
- `AGENTD_RUN_ATTEST_ED25519_KID` (optional; Ed25519 signing key id for `/api/v1/run/attestation`)
- `AGENTD_RUN_ATTEST_ED25519_SEED` (optional; Ed25519 seed, hex or base64, 32 bytes)

## WebUI

For WebUI dev/build/runtime config and reliability notes, see `docs/WEBUI.md`.
