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
  - AVM subprocess exec now closes inherited non-stdio file descriptors before launch, so an orphaned runner cannot keep
    agentd listener sockets pinned across an ungraceful same-port restart.
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
  broker token. If that peer was recovered as a live persisted runtime after agentd restart, the delete path now
  preserves the same explicit terminal signal/result in `voice_runtime_cleanup.peer` before clearing the record.
- `GET /api/v1/session/audit?session_id=<id>&include_rotated=0|1` returns recent per-run audit entries.
- `POST /api/v1/session/voice_control` persists minimal session-scoped `media_play` / `media_pause` / `media_snapshot`
  control requests for browser clients.
- `GET /api/v1/session/voice_stats?session_id=<id>` summarizes observed voice/media RPC outcomes for the session.
- `POST /api/v1/session/voice_webrtc_peer` starts or stops the managed WebRTC media peer for a session.
- `runtime_kind` on that endpoint is start-only; stop requests now ignore it and act on the actual managed runtime
  state for the session instead of rejecting unrelated backend values.
- `GET /api/v1/session/voice_webrtc_peer?session_id=<id>` reports managed media-peer runtime status, readiness, and final result.
- The current implementation exposes an explicit backend seam:
  - `default_runtime_kind=bundled|external`
  - `default_runtime_kind_source=auto|env|config`
  - `default_runtime_kind_available=true|false`
  - `builtin_available=false`
  - `bundled_available=true|false`
  - `external_available=true|false`
  - `builtin_unavailable_reason`, `bundled_unavailable_reason`, `external_unavailable_reason`,
    `default_runtime_kind_unavailable_reason`
  - `peer.runtime_kind=bundled|external`
  so the shipped Node/Playwright peer is now clearly modeled as an explicit backend family with durable default
  selection rather than an implicit implementation detail.
- The bundled/external backends now persist runtime snapshots in the agentd DB plus a per-session stdout log, so
  `GET /api/v1/session/voice_webrtc_peer` can recover running or stopped peer state across agentd restarts with
  `peer.status_source=memory|persisted`, and duplicate compatible `start` calls after restart can return
  `already_running` without re-supplying broker parameters.
- If a bundled/external peer is already running, `action=start` is now idempotent only when the effective resolved
  runtime config still matches the live peer. Explicit/requested conflicts, config-driven backend changes, and
  config changes that make that selected backend unlaunchable now fail closed with `409` plus the existing `peer`
  snapshot; that includes explicit `runtime_kind=builtin`, which now conflicts against a live non-builtin runtime
  instead of falling through to the reserved not-implemented branch.
- Child exit state is now persisted eagerly as the peer process ends, so later status reads and daemon restarts do not
  depend on an in-memory refresh to observe that the peer already stopped.
- If the canonical session row is gone but stale `voice_webrtc_peer` state still exists, the status read now self-heals
  that stale runtime, clears local runtime artifacts, and reports the cleanup in `cleanup_on_missing_session`.
- If the persisted `session.voice_webrtc_peer.*` record is corrupt, status/start/stop now self-heal it by clearing the
  bad persisted record, removing stale local runtime artifacts, and exposing that recovery in
  `cleanup_on_corrupt_record` instead of returning a hard `500`.
- If a dead daemon process left behind a persisted `session.voice_webrtc_peer.*` record with `running=true`, status,
  stop, and later starts now self-heal that stale snapshot too: agentd clears the persisted record, removes stale
  local runtime artifacts, exposes the recovery in `cleanup_on_stale_record`, and treats the runtime as not running
  instead of surfacing a fake persisted stopped peer.
- `POST /api/v1/session/voice_webrtc_peer` `action=stop` now also reports actual local teardown semantics: when the
  peer already exited, the response returns `stopped=false` and `reason=not_running` instead of claiming a live stop,
  while still attempting owned broker-session cleanup when applicable.
- If a still-live bundled/external peer is recovered from persisted running state after agentd restart, a later
  `action=stop` now preserves an explicit terminal signal/result in that persisted snapshot too, instead of degrading
  to a generic stopped record with no authoritative final cause.
- `POST /api/v1/session/voice_webrtc_peer` no longer requires callers to pre-create the broker audio session:
  if `broker_session_id` is omitted and `broker_agent_id` is provided, agentd now creates the broker audio session,
  launches the peer against it, and reports `peer.managed_broker_session=true` plus the chosen
  `peer.broker_agent_id` / `peer.broker_deployment_id` in runtime status.
- When callers do provide `broker_session_id`, agentd now preflights that broker session through the broker before
  launching the peer and returns `400 broker_session_id not found` instead of spawning a child against a missing
  signaling session.
- `broker_session_id` is now mutually exclusive with `broker_agent_id` / `broker_deployment_id`; agentd rejects that
  ambiguous mixed mode at request validation time instead of silently ignoring the auto-create fields.
- `POST /api/v1/session/voice_webrtc_peer` now supports daemon-level broker defaults through
  `AGENTD_AUDIO_WEBRTC_BROKER_URL` and `AGENTD_AUDIO_WEBRTC_BROKER_TOKEN` or `/api/v1/config/update`, so callers may
  omit `broker_url` / `broker_token` when those defaults are configured. Runtime status also reports
  `broker_url_default_configured` / `broker_token_default_configured`.
- That same daemon-level `audio_webrtc` config now also persists the `external` backend seam itself:
  `peer_tool_path` for `runtime_kind=external`, `node_bin` for bundled/external peer launch, and
  `default_runtime_kind` for no-request backend selection, so operators no longer have to rely only on process
  environment or implicit autodetect behavior to keep the managed WebRTC backend wired correctly across restarts.
- `GET /api/v1/config` and `POST /api/v1/config/update` now expose the same safe backend-availability facts for that
  managed WebRTC lane:
  `builtin_available=false`, `bundled_available=true|false`, `external_available=true|false`,
  `default_runtime_kind_available=true|false`, plus per-backend unavailable reasons. That lets operators see when
  `default_runtime_kind=external` is persisted but currently unusable because the external helper seam is not
  configured, or when a configured backend is unlaunchable because `node_bin` is invalid/missing.
- Daemon startup also honors `AGENTD_AUDIO_WEBRTC_DEFAULT_RUNTIME_KIND=bundled|external`; runtime/config status then
  reports `default_runtime_kind_source=env` until a persisted daemon config override takes precedence.
- If persisted runtime config is corrupted to an invalid `audio_webrtc.default_runtime_kind`, agentd now self-heals it
  back to `auto` on load instead of reporting an impossible configured backend while silently falling back internally.
- `POST /api/v1/session/voice_webrtc_peer` now also performs bounded startup confirmation. If the managed peer process
  exits before it ever reaches ready, agentd returns a failed start instead of `started=true`, reports
  `startup_confirmed=false`, and cleans up any owned broker audio session before returning.
- `POST /api/v1/session/voice_webrtc_peer` `action=stop` now accepts optional `broker_token`; when the runtime owns the
  broker audio session, agentd can use either that request token or the daemon's configured default token to delete the
  session itself if the peer died before delivering `bye`.
- The stop/delete cleanup path now validates broker tokens lazily: a malformed configured default broker token no longer
  blocks stop or session delete for borrowed broker sessions where agentd does not own broker-session deletion.
- If agentd does own the broker audio session but broker deletion fails, stop/session delete now still complete local
  teardown and surface the broker cleanup failure in `broker_session_deleted=false` plus
  `broker_session_delete_error`, instead of failing the whole local teardown path.

## Data governance

- `POST /api/v1/memory/retention/enforce` applies deterministic retention policies to daily logs and
  structured checkpoints, with `dry_run` and per-call overrides.
- `DELETE /api/v1/session?session_id=<id>` is the primary erase surface. After a successful delete,
  session history and run-backed evidence for that session no longer resolve through
  `GET /api/v1/session`, `GET /api/v1/run/replay`, or `GET /api/v1/run/attestation`. The delete response now also
  reports `voice_runtime_cleanup` when a managed WebRTC peer existed for the session, including the final recovered
  runtime snapshot if delete had to stop a live peer that survived an earlier daemon restart.
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
`daemon.audio_webrtc.{broker_url_default_configured,broker_token_default_configured,peer_tool_path_configured,builtin_available,bundled_available,external_available,builtin_unavailable_reason,bundled_unavailable_reason,external_unavailable_reason,default_runtime_kind,default_runtime_kind_source,default_runtime_kind_available,default_runtime_kind_unavailable_reason,node_bin}`
so operators can verify whether caller-free voice runtime bring-up and backend selection are configured without
exposing the token itself.
For the managed edge consensus lane, the safe snapshot now also exposes
`edge_consensus.{node_tool_path_configured,builtin_available,external_available,external_unavailable_reason,default_runtime_kind,default_runtime_kind_source,default_runtime_kind_available,default_runtime_kind_unavailable_reason,clusters_set,cluster_ids}`
so operators can see whether the optional `runtime_kind=external` helper seam is configured and whether the current default backend is actually launchable.

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

Example (persist the operator-configured external peer backend seam):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"audio_webrtc":{"peer_tool_path":"/opt/agentd/tools/agentd_audio_webrtc_peer.js","default_runtime_kind":"external","node_bin":"node"}}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Example (persist the operator-configured external consensus helper seam):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer YOUR_AGENTD_TOKEN" \
  -d '{"edge_consensus":{"node_tool_path":"/opt/agentd/bin/agentd_edge_consensus_node","default_runtime_kind":"external"}}' \
  http://127.0.0.1:8123/api/v1/config/update
```

Notes:
- The response never includes secrets. Use `GET /api/v1/config` to see booleans like `provider_keys_set`.
- For managed voice/WebRTC broker defaults, `GET /api/v1/config` exposes only
  `daemon.audio_webrtc.{broker_url_default_configured,broker_token_default_configured,peer_tool_path_configured,builtin_available,bundled_available,external_available,builtin_unavailable_reason,bundled_unavailable_reason,external_unavailable_reason,default_runtime_kind,default_runtime_kind_source,default_runtime_kind_available,default_runtime_kind_unavailable_reason,node_bin}`.
- For the managed edge consensus helper seam, `GET /api/v1/config` exposes only
  `edge_consensus.{node_tool_path_configured,builtin_available,external_available,external_unavailable_reason,default_runtime_kind,default_runtime_kind_source,default_runtime_kind_available,default_runtime_kind_unavailable_reason,clusters_set,cluster_ids}`.
- `POST /api/v1/edge/node/consensus_runtime` now uses bounded startup confirmation for both builtin and external managed
  runtimes, so a loop/helper that dies immediately with failure returns `startup_confirmed=false` with no stale runtime
  left behind, while a runtime that commits successfully during that same window still returns success.
- `POST /api/v1/edge/node/consensus_runtime` `action=stop` now reports `reason=not_running` when the managed runtime has
  already completed, and includes the final runtime snapshot/result instead of claiming a live stop occurred.
- `POST /api/v1/edge/node/consensus_runtime` `action=start` is now idempotent only for the same running config. If the
  same `node_id` already has a different effective running runtime config, agentd returns `409` with the existing runtime snapshot.
- Managed consensus runtime snapshots now persist in DB meta too, so
  `GET /api/v1/edge/node/consensus_runtime?node_id=<id>` can recover the last finished/stopped runtime after agentd
  restart with `runtime.status_source=persisted`, and can also recover a still-live external helper from its persisted
  running snapshot.
- If that recovered live external helper is later stopped through agentd, the persisted final runtime snapshot now also
  records the stop signal instead of collapsing to a signal-less stopped state after restart recovery.
- Persisted consensus runtime records now self-heal on read: corrupt records and stale builtin `running=true` records
  from a dead daemon process are cleared along with dead local runtime artifacts instead of being reported forever as
  live or unusable managed state.
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
  `GET /api/v1/edge/node/consensus_runtime?node_id=<id>` reports managed runtime status plus the latest final result JSON,
  and now also recovers the last finished/stopped runtime from persisted DB state after restart.
  Both start and status now also expose `external_available` plus `external_unavailable_reason`, so operator tooling can
  see whether the external helper seam is actually launchable before trying `runtime_kind=external`.
  External starts now also use bounded startup confirmation and return `startup_confirmed=false` with a `500` response
  if the helper exits immediately after launch instead of reporting a false-positive started runtime.
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
- The same external helper path can now also be persisted through `POST /api/v1/config/update` as
  `edge_consensus.node_tool_path`, and `GET /api/v1/config` reports whether that seam is configured and available
  without exposing the literal path.
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
