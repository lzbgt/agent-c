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

Memory scheduling (disabled by default; requires `summary_model` for recaps):
- `--memory-recap-daily-interval-ms <n>`
- `--memory-recap-weekly-interval-ms <n>`
- `--memory-recap-daily-days <n>`
- `--memory-recap-weekly-days <n>`

Env overrides:
- `AGENTD_MEMORY_RECAP_DAILY_INTERVAL_MS`
- `AGENTD_MEMORY_RECAP_WEEKLY_INTERVAL_MS`
- `AGENTD_MEMORY_RECAP_DAILY_DAYS`
- `AGENTD_MEMORY_RECAP_WEEKLY_DAYS`

Memory correlation index (best-effort, on-demand or via recap/consolidation):
- `POST /api/v1/memory/correlation/index` builds the cross-run correlation index.
- `GET /api/v1/memory/correlate` returns `daily_entries` + `recap_entries` when the index exists.
- The daemon refreshes the index after recap generation or memory consolidation (best-effort).
- Index path: `state_dir/memory/.memory_correlation.sqlite3` (SQLite builds) or `state_dir/memory/.memory_correlation.json`.

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

Sandbox mount allowlist:
- For sandboxed tool runners, host mounts are gated by `~/.config/agent/mount-allowlist.json`.
- Generate a template with `python3 tools/create_mount_allowlist.py`.
- Diagnostics expose status in `/api/v1/diagnostics` (`sandbox_mount_allowlist`).
- Validate mounts via `POST /api/v1/sandbox/mount_validate` (returns allow/deny + readonly decision).
  - Example request in `docs/TOOLS.md`.
- `POST /api/v1/avm/capsule_run` and workflow tasks with `kind: avm_capsule` can optionally include `capsule.mounts`.
  - Each mount is validated against the same allowlist before launch.
  - `capsule.host_effects` is an explicit request surface for AVM host capabilities:
    - `fs` gated by `AGENTD_AVM_ALLOW_FS=1`
    - `proc` gated by `AGENTD_AVM_ALLOW_PROC=1`
    - `net` gated by `AGENTD_AVM_ALLOW_NET=1`
  - `capsule.mounts` requires `capsule.host_effects.fs=true`; `capsule.allow_domains` requires `capsule.host_effects.net=true`.
  - Allowed mounts are forwarded to the AVM subprocess via `AGENTD_AVM_MOUNTS_JSON` plus `AGENTD_AVM_MOUNT_<n>_*` env vars.
  - Explicit host-effect policy is forwarded to the AVM subprocess via `AGENTD_AVM_HOST_EFFECT_{FS,PROC,NET}`.
  - Workflow `avm_capsule` tasks also persist durable session artifacts:
    - `.../governance_bundle.json` with `job_scan`, `policy_scan`, `inspect`, `verify_strict`, sanitized capsule args,
      run summary, and program/job hash keys
    - `.../output.log` with the bounded AVM subprocess output backing `output.raw_text`
    - These artifacts are queryable through `GET /api/v1/session/artifacts`.
  - If the allowlist is missing, invalid, or rejects a requested mount, execution fails closed before the subprocess starts.

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

## Workflow admission control + durable budget evidence

The workflow scheduler already exposes the core scheduling/isolation MVP for durable execution:

- Submit-time admission control:
  - `--workflow-admit-max-inflight-tasks-total`
  - `--workflow-admit-max-inflight-tasks-per-session`
  - rejected submits return HTTP `429` with deterministic counters + `retry_after_ms`
- Durable workflow-level budgets on `/api/v1/workflow/submit` via `workflow_limits`:
  - `max_tool_calls_total`
  - `max_steps_total`
  - `max_elapsed_ms_total`
  - `max_total_tokens`
- Per-attempt caps on delegated run execution:
  - workflow `attempt_caps` clamp `timeout_ms`, `max_steps`,
    `max_tool_calls_total`, and `max_tool_calls_per_tool`
  - request `host_policy: "readonly"` keeps workflow-triggered tool execution
    inside the daemon’s restricted host surface

Evidence surface:
- When a budget is exceeded, the engine emits a durable
  `workflow_budget_exceeded` event under `GET /api/v1/workflow/events`
  with used/remaining counters.
- The same cancellation path bulk-cancels still-queued follow-on tasks so the
  workflow converges to a deterministic terminal state.

Proof tests:
- `tests/agentd_workflow_admission_control_smoke.sh`
- `tests/agentd_workflow_budget_tool_calls_smoke.sh`
- `tests/agentd_workflow_budget_steps_smoke.sh`
- `tests/agentd_workflow_budget_tokens_smoke.sh`
- `tests/agentd_workflow_budget_tokens_stream_smoke.sh`
- `tests/agentd_workflow_budget_events_smoke.sh`

### Automation profiles (nonblocking defaults)

- `/api/v1/caps` includes `features.automation` with `default_profile` and supported `profiles`.
- `POST /api/v1/run` accepts `automation_profile` to override daemon defaults:
  - `full`: `yolo_default=true`, `host_policy=full`, `policy_mode=off`.
  - `guided`: `yolo_default=false`, `host_policy=readonly`, `policy_mode=audit`.
  - `strict`: `yolo_default=false`, `host_policy=readonly`, `policy_mode=enforce`.
  - `custom`: no override (use daemon config as-is).
- Run responses include `effective_automation_profile`.

### Moderator control plane (nonblocking)

- `POST /api/v1/moderator/directive` publishes a moderator directive as a client event (`type=moderator_directive`).
- `POST /api/v1/moderator/task` publishes a moderator task (`type=moderator_task_published`).
- `GET /api/v1/moderator/events?session_id=...` tails moderator events for a session (filters client events).
- These events are stored in the client events log and are replayable; they do not block runs unless a policy hook requires it.

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

## Workflow schedules (cron, UTC-only)

Agentd supports cron-bound workflow schedules (durable templates + tick audit log).
Timezone is currently **UTC only**.

- `POST /api/v1/workflow_schedules`
  - Body: `{ "cron": "0 9 * * 1-5", "timezone": "UTC", "spec": { ...workflow submit... }, "metadata": {...} }`
- `GET /api/v1/workflow_schedules?status=active|paused|error&limit=100&offset=0`
- `GET /api/v1/workflow_schedule?schedule_id=<id>`
- `POST /api/v1/workflow_schedule/pause` (body `{ "schedule_id": "..." }`)
- `POST /api/v1/workflow_schedule/resume` (body `{ "schedule_id": "..." }`)
- `DELETE /api/v1/workflow_schedule?schedule_id=<id>`
- `GET /api/v1/workflow_schedule/runs?schedule_id=<id>&limit=100&offset=0`

## Approval queue API

- `GET /api/v1/approvals?status=pending&trace_id=...&job_id=...&tool_name=...&run_id=...&limit=100`
  - Lists approvals (most recent first). Filters are optional.
- `GET /api/v1/approvals/<approval_id>`
  - Returns a single approval with its decisions array.
- `POST /api/v1/approvals/<approval_id>/decisions`
  - Body: `{ "member_id": "...", "member_role": "...", "decision": "approve|deny", "note": "..." }`
  - If the approval has `role_constraints`, `member_role` is required and must match the allowlist.

## OpenRouter model discovery

`GET /api/v1/openrouter/models?min_total=0.01&max_total=0.50&require_multimodal_input=1&require_tools=1&limit=50`
fetches OpenRouter’s `/models` catalog (using `OPENROUTER_API_KEY` or `Authorization: Bearer ...`),
filters it, sorts by total price ($/1M prompt+completion), and returns a recommended cheapest model id.

## Session browsing

- `GET /api/v1/sessions` lists known sessions.
- `GET /api/v1/session?session_id=<id>` returns the message history.
- `DELETE /api/v1/session?session_id=<id>` erases the canonical SQLite-backed session record and its
  dependent runs/messages/tool records/events/artifacts via DB cascade. If the session currently owns a managed
  `voice_webrtc_peer` runtime, agentd now also stops that peer, clears its persisted runtime/log artifacts, and can
  delete the owned broker audio session using either the request `broker_token` or the daemon's configured default
  broker token.
- `GET /api/v1/session/audit?session_id=<id>&include_rotated=0|1` returns recent per-run audit entries.
- `POST /api/v1/session/voice_control` persists minimal session-scoped `media_play` / `media_pause` / `media_snapshot`
  control requests for browser clients.
- `GET /api/v1/session/voice_stats?session_id=<id>` summarizes observed voice/media RPC outcomes for the session.
- `POST /api/v1/session/voice_webrtc_peer` starts or stops the managed WebRTC media peer for a session.
- `GET /api/v1/session/voice_webrtc_peer?session_id=<id>` reports managed media-peer runtime status, readiness, and final result.
- The current implementation exposes an explicit backend seam:
  - `default_runtime_kind=bundled` when the shipped repo helper is discoverable
  - `builtin_available=false`
  - `bundled_available=true|false`
  - `peer.runtime_kind=bundled|external`
  so the shipped Node/Playwright peer is now clearly modeled as a default bundled backend with an explicit `external`
  override path instead of an implicit implementation detail.
- The bundled/external backends now persist runtime snapshots in the agentd DB plus a per-session stdout log, so
  `GET /api/v1/session/voice_webrtc_peer` can recover running or stopped peer state across agentd restarts with
  `peer.status_source=memory|persisted`, and duplicate `start` calls after restart can return `already_running`
  without re-supplying broker parameters.
- Child exit state is now persisted eagerly as the peer process ends, so later status reads and daemon restarts do not
  depend on an in-memory refresh to observe that the peer already stopped.
- If the canonical session row is gone but stale `voice_webrtc_peer` state still exists, the status read now self-heals
  that stale runtime, clears local runtime artifacts, and reports the cleanup in `cleanup_on_missing_session`.
- `POST /api/v1/session/voice_webrtc_peer` no longer requires callers to pre-create the broker audio session:
  if `broker_session_id` is omitted and `broker_agent_id` is provided, agentd now creates the broker audio session,
  launches the peer against it, and reports `peer.managed_broker_session=true` plus the chosen
  `peer.broker_agent_id` / `peer.broker_deployment_id` in runtime status.
- `POST /api/v1/session/voice_webrtc_peer` now supports daemon-level broker defaults through
  `AGENTD_AUDIO_WEBRTC_BROKER_URL` and `AGENTD_AUDIO_WEBRTC_BROKER_TOKEN` or `/api/v1/config/update`, so callers may
  omit `broker_url` / `broker_token` when those defaults are configured. Runtime status also reports
  `broker_url_default_configured` / `broker_token_default_configured`.
- `POST /api/v1/session/voice_webrtc_peer` `action=stop` now accepts optional `broker_token`; when the runtime owns the
  broker audio session, agentd can use either that request token or the daemon's configured default token to delete the
  session itself if the peer died before delivering `bye`.

## Data governance

- `POST /api/v1/memory/retention/enforce` applies deterministic retention policies to daily logs and
  structured checkpoints, with `dry_run` and per-call overrides.
- `DELETE /api/v1/session?session_id=<id>` is the primary erase surface. After a successful delete,
  session history and run-backed evidence for that session no longer resolve through
  `GET /api/v1/session`, `GET /api/v1/run/replay`, or `GET /api/v1/run/attestation`. The delete response now also
  reports `voice_runtime_cleanup` when a managed WebRTC peer existed for the session.
- `GET /api/v1/db/analytics/workflows/export?format=json|csv&scope=all|durable|edge` exports durable
  workflow analytics snapshots for audit/reporting.
- `GET /api/v1/db/analytics/edge/export?format=json|csv&scope=all|edge_tasks|edge_nodes` exports edge
  analytics snapshots for the same governance/reporting workflows.
- `GET /api/v1/run/replay?run_id=<id>` returns a redacted replay bundle (`run_replay_bundle_v1`).
  Sensitive request fields such as `api_key` are omitted from the bundle.
- `GET /api/v1/run/attestation?run_id=<id>` returns a signed or unsigned
  `run_attestation_bundle_v1` referencing the replay hash and carrying stable daemon
  `node_id` evidence (`AGENTD_NODE_ID` when set, else `listen_host:listen_port`).

Host-test proof points:
- `tests/agentd_memory_retention_smoke.sh`
- `tests/agentd_session_delete_governance_smoke.sh`
- `tests/agentd_db_analytics_export_governance_smoke.sh`
- `tests/agentd_run_replay_smoke.sh`
- `tests/agentd_run_attestation_ed25519_smoke.sh`

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
and `db_path` (SQLite; canonical daemon state store). For the managed WebRTC lane, the safe snapshot now also exposes
`daemon.audio_webrtc.{broker_url_default_configured,broker_token_default_configured}` so operators can verify whether
caller-free voice runtime bring-up is configured without exposing the token itself.

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

Example (store default broker settings for managed WebRTC voice runtimes):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"audio_webrtc":{"broker_url":"http://127.0.0.1:8090","broker_token":"audio-agentd-token"}}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Notes:
- The response never includes secrets. Use `GET /api/v1/config` to see booleans like `provider_keys_set`.
- For managed voice/WebRTC broker defaults, `GET /api/v1/config` exposes only
  `daemon.audio_webrtc.{broker_url_default_configured,broker_token_default_configured}`.
- The WebUI exposes Settings buttons to “Save defaults to daemon” and “Save API key to daemon”.

Edge trust-root rotation:
- `GET /api/v1/edge/auth/trust_roots` returns a safe edge-auth trust-root bundle with rotation metadata,
  HMAC `kid` list, Ed25519 public keys, and optional server-side attestation.
- `POST /api/v1/edge/auth/trust_roots/rotate` applies a monotonic rotation epoch and updates the current
  HMAC / Ed25519 trust-root set in the durable runtime config.
- `POST /api/v1/edge/auth/trust_roots/send` enqueues that same signed trust-root bundle to a recipient node’s
  outbox as `PLATFORM_TRUST_ROOTS_BUNDLE`.
- `GET /api/v1/edge/auth/cert_roots` returns the durable PEM certificate-root bundle with optional
  server-side attestation, and `POST /api/v1/edge/auth/cert_roots/rotate` updates it with a monotonic epoch.
- `POST /api/v1/edge/auth/cert_roots/send` enqueues that same signed certificate-root bundle to a recipient node’s
  outbox as `PLATFORM_CERT_ROOTS_BUNDLE`.
- `tools/edge_cert_roots_tool.py` inspects those signed bundles, exports a CA PEM file, and runs
  `openssl verify` against candidate cert chains so operators can validate shipped root material even
  before policy-enforced manifest identity checks are enabled on `/api/v1/edge/message`.
- `POST /api/v1/edge/auth/cert_roots/verify_chain` performs the same chain check inside `agentd`
  against the currently stored root set and returns structured verification metadata (`verified`,
  `verify_error`, matched root kids, chain subjects, leaf summary).
- `POST /api/v1/config/update` supports `edge_auth_require_manifest_cert_chain`. When enabled,
  `NODE_CAPS_RSP.body.manifest.identity.cert_pem` plus optional `cert_chain_pem[]` must verify against
  the current PEM root bundle or the manifest is rejected at ingest.
- `GET /api/v1/edge/auth/revocations` returns the durable revoked-`kid` / revoked-node bundle, with optional
  server-side attestation when run-attestation signing is configured.
- `POST /api/v1/edge/auth/revocations/update` applies a monotonic revocation epoch and updates the current
  revoked `kid` / node-id set in the durable runtime config.
- `POST /api/v1/edge/auth/revocations/send` enqueues that same signed revocation bundle to a recipient node’s
  outbox as `PLATFORM_REVOCATIONS_BUNDLE`.
- `GET /api/v1/edge/auth/node_binding?node_id=...` shows the effective binding and revocation status for one node,
  and `POST /api/v1/edge/auth/provision_node` remains the per-node bootstrap/rotation helper.
- `POST /api/v1/edge/node/manifest_bundle/send` enqueues a signed `PLATFORM_MANIFEST_BUNDLE` to a recipient node’s
  outbox so peer manifest/identity material can travel over the same UM-BMP poll path as other node traffic.
- `POST /api/v1/edge/message` now also accepts `CONSENSUS_FRAME` relay traffic:
  validated `edge_node_consensus_frame_v1` bodies are forwarded to recipient node outboxes as platform-relayed
  peer frames, and sender consensus summaries are surfaced through `GET /api/v1/edge/node` and `GET /api/v1/edge/nodes`.
  The repo’s host bring-up tool `agentd_edge_consensus_node` consumes that same outbox path and turns it into an
  autonomous vote/commit loop, covered by `tests/agentd_edge_consensus_autonomous_smoke.sh`.
- `POST /api/v1/edge/node/consensus_runtime` starts or stops that same autonomous consensus helper under agentd
  lifecycle ownership. The default runtime backend is now builtin, so agentd can run the same poll/process/post loop
  in-process without spawning the standalone helper; `runtime_kind=external` remains available for bring-up/debug parity.
  `GET /api/v1/edge/node/consensus_runtime?node_id=<id>` reports managed runtime status plus the latest final result JSON.
- Start requests can include `campaign_delay_ms`, `campaign_retry_ms`, `campaign_retry_max_ms`, and
  `campaign_retry_backoff_factor`, plus `leader_heartbeat_ms` and `leader_lease_ms`; the reported runtime/result
  surfaces expose that same bounded retry and leader-freshness policy so operators can confirm whether a candidate
  re-campaigned before quorum formed and when follower failover should trigger.
- The same runtime start surface also accepts `membership_epoch` and `member_node_ids`, and the emitted runtime/result
  JSON mirrors that explicit member-set view for deterministic compatibility checks.
- `GET /api/v1/edge/consensus/membership?cluster_id=<id>` now exports a signed durable
  `edge_consensus_membership_v1` bundle for one cluster, and `POST /api/v1/edge/consensus/membership/rotate`
  persists the monotonic membership epoch, member set, default retry timing, and leader heartbeat/lease policy for
  that cluster.
- `POST /api/v1/edge/consensus/membership/send` enqueues that same bundle to a recipient node outbox as
  `PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE`, so non-HTTP nodes can poll membership policy through the shipped UM-BMP lane.
- When a managed consensus runtime start omits `membership_epoch`, `member_node_ids`, `campaign_delay_ms`,
  `campaign_retry_ms`, `campaign_retry_max_ms`, `campaign_retry_backoff_factor`, `leader_heartbeat_ms`, or
  `leader_lease_ms`, agentd now defaults those fields from the stored cluster membership bundle.
- Operator bring-up can still set `AGENTD_EDGE_CONSENSUS_NODE_TOOL=/abs/path/to/agentd_edge_consensus_node` to force
  `runtime_kind=external`, but the normal managed path no longer depends on that helper being configured.
- `GET /api/v1/edge/node` and `GET /api/v1/edge/nodes` now surface `consensus_runtime` when a node has a managed
  runtime record, so protocol-level consensus state and runtime/process state are visible together.
- `POST /api/v1/config/update` supports `edge_confidentiality_required` and `edge_confidentiality_keys`.
  When enabled, `/api/v1/edge/message` rejects plaintext `body` and requires AES-GCM `body_enc`.
- `POST /api/v1/edge/auth/trust_roots/send`, `.../cert_roots/send`, `.../revocations/send`, and
  `POST /api/v1/edge/node/manifest_bundle/send` accept `confidential_kid` to emit encrypted outbox
  envelopes instead of plaintext bundle bodies.

## Client prefs (WebUI connection profiles)

The daemon can persist WebUI connection profiles so they survive browser resets:

- `GET /api/v1/client/prefs?client_id=<id>&client_kind=webui`
- `POST /api/v1/client/prefs` with `{ client_id, client_kind, prefs }`

Notes:
- Requires auth when `--auth-token` is set.
- v1 stores **non-secret** connection fields only (URLs/agent ids). Tokens stay client-side.
- Workflow waits (composer resume state) are stored under `client_kind=webui-workflow` when available.

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
