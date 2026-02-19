<!-- GENERATED FILE. DO NOT EDIT. -->
<!-- Edit docs/handbook/OVERVIEW.md and source docs; run tools/build_handbook_bundle.py. -->

# Agent Handbook (Unified)

Date: 2026-02-19

This handbook consolidates the essential information needed to build, run, extend, and operate the agent stack.
It merges the key content from the architecture, protocol, workflows, tools, memory, diagnostics, DB, streaming,
limits, broker, and platform notes into a single place so you can onboard without jumping across many files.

If you need deeper, versioned specs, see the bundled OpenAPI + spec sections later in this file
(sources: `docs/openapi/README.md` and `docs/spec/README.md`).

---

## 1) Quickstart build + verify

Build + test (default host build):

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

One-command verify (configure + build + tests; writes logs under `build/`):

```bash
tools/verify.sh
```

Optional repo guards in the same run:

```bash
tools/verify.sh --repo-guards
```

Core-only build (portable core library; skips host binaries):

```bash
cmake -S . -B build-core -DAGENT_BUILD_HOST=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

Host build dependencies (macOS/Homebrew):

```bash
brew install cmake pkg-config curl jsoncpp sqlite
```

Windows (native build via vcpkg):

```powershell
vcpkg install curl jsoncpp sqlite3
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build -j
```

---

## 2) Architecture at a glance

### Goals
- Fast startup, small footprint.
- OpenAI-compatible backends (OpenAI/OpenRouter/DeepSeek/Moonshot, etc.).
- Seamless compaction for long sessions.
- Portability: desktop + daemon + embedded/VM.
- Deterministic, inspectable execution (workflows, audits, replay bundles).

### Layered stack
1. `agent_core` (portable C library)
   - Session model, compaction, tool registry, tool-loop logic.
   - No env, filesystem, network, or JSON dependencies.
2. `agentd` (daemon)
   - HTTP/SSE API, tool loop execution, persistence, audit logs, workflows.
3. `agent` CLI
   - Flags/env/config parsing, transport, local session store, host tools.
4. Broker (optional)
   - Secure relay, agent registry, OIDC auth, mTLS connectors.
5. Clients (WebUI, mobile, integrations)
   - Rich UX and collaboration on top of agentd APIs.

Design details: `DESIGN.md`

---

## 3) Runtime modes

### Daemon (`agentd`)
- Long-running service with HTTP/SSE APIs.
- Persists sessions, runs, events, artifacts, workflows, jobs.
- Supports tool plugins/servers and broker connectivity.

### CLI (`agent`)
- Runs locally with file-backed sessions (default under `~/.agent/sessions/`).
- Can point at daemon endpoints for shared sessions/workflows.

### Core-only (embedded/VM)
- `agent_core` exposes a portable C API with minimal assumptions.
- Session codec supports a JSON-free, line-based format for embedded persistence.

---

## 4) Configuration + secrets

### Auth + CORS (daemon)
- `--auth-token` is required when binding to non-loopback.
- When binding to loopback, CORS defaults to `*` for local dev.
- For remote binds, set explicit origins via `--cors-origin` and optionally `--cors-allow-credentials`.

### Provider keys (preferred path)
- `.not_in_repo` at repo root (gitignored) supports:
  - `DEEPSEEK_API_KEY=...`
  - `OPENROUTER_API_KEY=...`
  - `KIMI_API_KEY_CN=...`
  - `MOONSHOT_API_KEY=...`
- `AGENTD_DOTENV_PATH=/path/to/.env` overrides dotenv lookup.
- `project.local.md` is supported for local-only keys (gitignored).

### Useful env overrides
- `AGENTD_DB_PATH`, `AGENTD_STATE_DIR`
- `AGENTD_HTTP_MAX_BODY_BYTES`, `AGENTD_HTTP_MAX_HEADER_BYTES`, `AGENTD_HTTP_READ_TIMEOUT_MS`
- `AGENTD_UPLOAD_MAX_BYTES`
- `AGENTD_CORS_ORIGINS`, `AGENTD_CORS_ALLOW_CREDENTIALS`

---

## 5) Session + run protocol (client <-> agentd)

### Session safety
- `session_id` is a filename-like key and must be safe:
  - length 1..200
  - characters `[A-Za-z0-9_.-]`
  - no `/`, `\\`, `.` or `..` segments

### Session creation
- `POST /api/v1/session/new`
  - returns `{ ok, session_id, created }`
  - optional request: `{ session_id?, create_files? }`

### Uploads (multimodal inputs)
- `POST /api/v1/session/upload` with `{ session_id, files:[{name,mime?,data_base64}] }`
- Stored under `<sessions_root>/session_<id>/uploads/` with size caps:
  - per-file cap: `--upload-max-bytes` / `AGENTD_UPLOAD_MAX_BYTES`
  - HTTP body cap: `AGENTD_HTTP_MAX_BODY_BYTES`
- Requires DB enabled (`--db-path`) for session bookkeeping.

### Artifacts + audit
- `GET /api/v1/session/artifacts?session_id=...`
- `GET /api/v1/session/audit?session_id=...`

### Durable Scene (server-owned)
- `GET /api/v1/session/scene?session_id=...`
- `POST /api/v1/session/scene/apply` with `{ session_id, ops:[...] }`
- Requires DB enabled (`--db-path`) for persistence.

### Runs
- `POST /api/v1/run` (sync)
- `POST /api/v1/run_async` (async job)

Common run fields:
- `session_id`, `prompt`, `model`, `base_url`
- `tools`: `none | basic | host`
- Safety limits: `max_steps`, `max_tool_calls_total`, `max_tool_calls_per_tool`, `max_tool_call_args_chars`, `max_tool_result_chars`, `max_repeated_tool_calls`, `tool_call_limits`
- `stream_assistant: true` for SSE deltas

### Replay bundles (deterministic audit)
- `GET /api/v1/run/replay?run_id=...`
- Redacted snapshots + tool records + deterministic hash (`agent_json_c14n_v1`).

---

## 6) Client collaboration (events + UI actions)

### Client identity
Clients should include:

```json
{
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" }
}
```

### Client event ingestion
- `POST /api/v1/session/client_event` (alias: `/api/v1/session/ui_event`)
- Event payload fields: `session_id`, `type`, `ts_unix_ms`, `client`, `data`, `append_to_session`.

Read-back:
- `GET /api/v1/session/client_events?session_id=...`
- `GET /api/v1/db/client_events?session_id=...`
- `GET /api/v1/session/clients?session_id=...`

### UI actions (agent -> client)
- Tool: `ui_action`
- Types: `notify`, `request_client_state`, `client_rpc` (aliases: `collab_rpc`, `client_probe`).

### Client RPC flow
1) Agent calls `ui_action` with `{ type:"client_rpc", rpc_id, rpc:{kind,args} }`
2) Client executes allowlisted handler.
3) Client posts `client_rpc_result` / `client_rpc_progress` events.
4) Agent waits deterministically via `client_wait_event` / `client_wait_any` / `client_wait_all`.

### DoD (definition of done)
UI-visible effects (artifacts, ui_action) can use the DoD handshake so the agent stops only after the UI acknowledges delivery.
See the DoD handshake appendix below (source: `docs/DOD_ACK.md`).

---

## 7) Tools and extensions

### Tool modes
- `none`: no tools, plain chat completion.
- `basic`: safe built-ins (e.g., calculator).
- `host`: full host tools (fs, exec, UI actions, memory).

### Tool servers (out-of-process)
Supported on Linux/macOS (Windows: tool servers disabled; plugins still supported).

Enable with:

```bash
./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tests/tool_server_echo.py" \
  --tool-server-timeout-ms 30000 \
  --tool-server-max-line-bytes $((4*1024*1024))
```

Protocol: JSON-lines over stdout with `manifest` and `execute` ops.

### Tool plugins (in-process)
- Load shared libraries with `--tool-plugin`.
- Required symbols:
  - `agentd_tool_plugin_manifest_json`
  - `agentd_tool_plugin_execute_json`
  - `agentd_tool_plugin_free`
- Optional config-aware variants:
  - `agentd_tool_plugin_manifest_json_ex`
  - `agentd_tool_plugin_execute_json_ex`

### Sandbox host (plugin isolation)
- Use the tool server protocol to run plugins in a separate host process:

```bash
./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll)"
```

---

## 8) Durable workflows

`agentd` includes a durable workflow scheduler (DAG of tasks) with restart recovery.
Key semantics:

- **Deadlines** (`deadline_unix_ms`): cancels queued tasks once exceeded.
- **Priorities** (`priority`): scheduling hint, clamped to `[-1000, 1000]`.
- **Fairness caps**:
  - `--workflow-max-inflight-per-workflow` (default 2)
  - `--workflow-max-inflight-per-session` (default 0)
- **Admission control**:
  - `--workflow-admit-max-inflight-tasks-per-session`
  - `--workflow-admit-max-inflight-tasks-total`
- **Retries/backoff**: bounded quadratic backoff with optional `retry_in_ms`.
- **Idempotency**: `idempotency_key` for safe submit retries.
- **Correctness checks**: `expect` assertions (substring, JSON pointers, etc.).

Workflow API:
- `POST /api/v1/workflow/submit`
- `GET /api/v1/workflow?workflow_id=...`
- DB query endpoints: `GET /api/v1/db/workflows`, `/workflow_tasks`, `/workflow_events`.

---

## 9) Memory (durable + searchable)

Memory lives under `state_dir/memory/`:
- `MEMORY.md` (core facts/preferences)
- `YYYY-MM-DD.md` (daily logs)
- `STRUCTURED.md` (structured memory)
- `sessions/<session_id>.md` (optional session layer)
- `checkpoints/structured_<ts>.json` (structured snapshots)
- `recaps/recap_<ts>.json` (LLM recaps)

Host tools:
- `memory_write`, `memory_observe`, `memory_get`, `memory_search`, `memory_timeline`, `memory_put`

Context injection modes (`memory_context_mode`):
- `files` (default), `search`, `index` (progressive), `salience`.

Privacy tags:
- Wrap sensitive content in `<private>...</private>` to prevent durable writes.

---

## 10) Streaming (OpenAI-compatible)

Streaming is handled via shared parsers/decoders:
- Core SSE framing: `core/include/agent/sse_parser.h`
- Shared stream decoder: `core/include/agent/stream_decoder.h`
- Host adapters: `cli/src/openai_stream_decoder.*`, `cli/src/openai_stream_adapter.*`

Behavioral goals:
- Parse assistant deltas and tool-call deltas reliably across OpenAI-ish providers.
- Recover usage metrics when `stream_options.include_usage=true` is supported.

Compatibility notes and probe tooling are included in the streaming appendix below (source: `docs/STREAMING.md`).

---

## 11) Run limits and safety guards

Key limits (all default to daemon config when omitted):
- `max_steps`: tool-loop steps
- `max_tool_calls_total`: total tool calls
- `max_tool_calls_per_tool`: per-tool caps
- `max_tool_call_args_chars`: input size guard
- `max_tool_result_chars`: output truncation for prompt
- `max_repeated_tool_calls`: repeated exact call guard
- `tool_call_limits`: per-tool explicit caps

When a hard limit is hit, the core emits a structured `error` event and returns `AGENT_ERR_LIMIT`.
Full semantics are included in the limits appendix below (source: `docs/LIMITS.md`).

---

## 12) Diagnostics + DB observability

Diagnostics endpoints (bearer auth required if `--auth-token` is set):
- `GET /api/v1/diagnostics` (health snapshot)
- `GET /api/v1/diagnostics/providers` (key presence + base URLs)
- `POST /api/v1/diagnostics/provider_test` (quick provider smoke test)

SQLite DB (when enabled with `--db-path`):
- Canonical store for sessions, runs, events, tools, workflows, artifacts, audit.
- Query endpoints provide read-only troubleshooting and analytics surfaces.

Primary DB docs are included in the DB appendix below (source: `docs/DB.md`).

---

## 13) Broker (secure relay)

Broker provides:
- OIDC/JWT client auth
- mTLS agent connectors (agent identity via cert CN)
- Agent registry + membership audit
- HTTP + SSE proxy to agentd deployments

Key endpoints:
- `GET /v1/agents` / `POST /v1/agents`
- `GET/POST/DELETE /v1/agents/{agent_id}/members`
- `POST /v1/agents/{agent_id}/disconnect`
- Proxy: `/v1/agents/{agent_id}/proxy/...`
- SSE proxy: `/v1/agents/{agent_id}/proxy_sse/...`

Full protocol details and security model are included in the broker appendix below (source: `docs/BROKER.md`).

---

## 14) Deployment + hardening (summary)

Recommended: expose broker publicly, keep agentd private.

Hardening highlights:
- Always set `--auth-token` for non-loopback binds.
- Use strict CORS allowlists for browser clients.
- Keep provider keys server-side (`.not_in_repo` or `AGENTD_DOTENV_PATH`).
- Set HTTP size/time limits via env.
- For OTA: enable `--ota-enable` and set `--ota-command`.

Production details are included in the deployment appendix below (source: `docs/DEPLOYMENT.md`).

---

## 15) Platform support

| Feature | Linux | macOS | Windows |
|---|---|---|---|
| agentd HTTP API + WebUI | yes | yes | yes |
| SQLite (`--db-path`) | yes | yes | yes |
| Tool plugins | yes | yes | yes |
| Tool servers | yes | yes | no |
| AVM endpoints | yes | yes | no (501) |

Full matrix is included in the platform support appendix below (source: `docs/PLATFORM_SUPPORT.md`).

---

## 16) OpenAPI + specs

- OpenAPI and versioned specs are bundled below (sources: `docs/openapi/README.md` and `docs/spec/README.md`).

---

## 17) Additional references (bundled below)

- `docs/TOOLS.md` (tool servers/plugins)
- `docs/WORKFLOWS.md` (workflow engine deep dive)
- `docs/MEMORY.md` (memory internals)
- `docs/DIAGNOSTICS.md` (diagnostics API)
- `docs/DB.md` (SQLite schema + query API)
- `docs/STREAMING.md` (streaming notes + compatibility)
- `docs/BROKER.md` (broker design + API)
- `docs/PROTOCOL.md` and `docs/CLIENT.md` (detailed client protocol)

---

# Appendix: Source Documents (verbatim)

The sections below are copied verbatim from their source files.

## Sources
- `README.md`
- `DESIGN.md`
- `docs/AGENTD_LIB.md`
- `docs/BROKER.md`
- `docs/CLIENT.md`
- `docs/DB.md`
- `docs/DEPLOYMENT.md`
- `docs/DIAGNOSTICS.md`
- `docs/DOD_ACK.md`
- `docs/EDGE_INTEROP.md`
- `docs/EMBEDDED_C_API.md`
- `docs/ESP32S3_AGENT_CORE_MATURITY.md`
- `docs/LIMITS.md`
- `docs/MACOS_PACKAGING.md`
- `docs/MEMORY.md`
- `docs/OREN_LANG_ECOSYSTEM.md`
- `docs/PLATFORM_SUPPORT.md`
- `docs/PROTOCOL.md`
- `docs/STREAMING.md`
- `docs/TOOLS.md`
- `docs/VENDORED.md`
- `docs/WORKFLOWS.md`
- `docs/openapi/README.md`
- `docs/spec/README.md`

---

# Source: README.md

# agent (prototype)

This repo is an early scaffold for a **portable agent core** (env-free, persistence-agnostic) plus a **desktop CLI host adapter** (env/config + persistence + HTTP).

Docs quickstart:
- Unified handbook (single-file merged docs; generated): `docs/HANDBOOK.md`
- Architecture + roadmap: `DESIGN.md` (also included in the handbook)

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

For a single command that runs configure/build/tests (and writes timestamped logs under `build/`), use:

```bash
tools/verify.sh
```

To include repo hygiene guards in the same run:

```bash
tools/verify.sh --repo-guards
```

To run the same verification pass but first source `${HOME}/.env` (so provider keys like `DEEPSEEK_API_KEY` are available
to smoke tests), use:

```bash
tools/verify_prod.sh
```

Host builds (`agent` / `agentd`) require `libcurl` and `jsoncpp` (via `pkg-config`).

**macOS (Homebrew)**:

```bash
brew install cmake pkg-config curl jsoncpp sqlite
```

macOS production packaging (signed/notarized `.pkg`) is documented in `docs/MACOS_PACKAGING.md`.

**Windows (native build)**: install dependencies via vcpkg, then configure with its toolchain file:

```powershell
vcpkg install curl jsoncpp sqlite3
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build -j
```

Or run the helper:

```powershell
tools\verify_windows_build.ps1 -Config Release
```

To auto-install vcpkg + deps:

```powershell
tools\verify_windows_build.ps1 -InstallDeps -VcpkgRoot C:\vcpkg -Config Release
```

CI helpers (optional):
- `tools/trigger_ci_windows_build.sh [ref]` (dispatches Windows CI; requires token or `gh auth login`)
- `tools/check_ci_windows_build.sh` (prints latest Windows CI status; requires token for private repos)

Platform-specific notes (Windows limitations, plugin support, AVM endpoints) live in `docs/PLATFORM_SUPPORT.md`.

Smoke tests:
- `ctest` includes many bash-based `agentd_*_smoke.sh` tests that start/stop the daemon; shared helpers live in `tests/lib/agentd_smoke_lib.sh`.
- Audio signaling smokes (`broker_audio_signal_docker_smoke`, `agentd_audio_signal_loopback_smoke`) normally spin up a
  temporary Postgres via Docker; if Docker is unavailable but `initdb`/`pg_ctl` are usable, they attempt to launch a local
  ephemeral Postgres instead. You can always override with `AGENTD_TEST_PG_DSN` to point at any reachable Postgres DSN.

### Core-only build (portable; no CURL required)

If you only want the portable core library + core unit tests (e.g. embedded/toolchain bring-up), disable host builds:

```bash
cmake -S . -B build-core -DAGENT_BUILD_HOST=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

Or:

```bash
tools/verify.sh --core-only
```

This builds `agent_core` and `agent_core_tests`, but skips `agent_host`, `agent`, `agentd`, and host/network smokes.

### Persistence port (hosts/embedded)

Core defines an optional persistence interface (`core/include/agent/persist.h`) so hosts can swap persistence implementations
without changing the core call sites (filesystem `.sess`, SQLite, NVS/flash, etc.).

### Durable memory (daemon state)

`agentd` maintains a durable memory directory under `state_dir/memory/` and exposes host tools (`memory_write/get/search/put`)
to let models persist and retrieve long-lived facts/preferences/tasks. See `docs/MEMORY.md`.

### Daemon longevity (job GC)

`agentd` keeps async job state in memory for UI progress streaming. Finished jobs are garbage-collected:
- `--job-ttl-ms <n>`: remove done/error jobs older than `n` ms (default: 1800000)
- `--max-jobs <n>`: keep at most `n` jobs in memory (default: 256)

### Daemon auth (optional)

If you bind `agentd` to non-loopback (e.g. `--host 0.0.0.0`), you must set an auth token (agentd refuses to start otherwise):

```bash
./build/agentd --auth-token "your_token"
```

Clients must send `Authorization: Bearer your_token` to all endpoints (except `/api/v1/health`, `/api/v1/ready`, and `/metrics`).
You can also accept the token via a cookie name (useful for browser clients):

```bash
./build/agentd --auth-token "your_token" --auth-cookie "agentd_auth"
```

### Daemon CORS (browser clients)

The Web UI typically runs on a different origin (e.g. `http://localhost:5173`) than the daemon (`http://127.0.0.1:8123`),
so `agentd` emits CORS headers for browser fetches:

- When binding to loopback (`127.0.0.1` / `localhost`): CORS defaults to `Access-Control-Allow-Origin: *` for local dev ergonomics.
- When binding to a non-loopback host: CORS is **disabled by default**. Enable it explicitly with one or more `--cors-origin` values.

Example (remote UI origin allowlist):

```bash
./build/agentd --host 0.0.0.0 --auth-token "your_token" --cors-origin "https://your-ui.example"
```

If you use cookie auth, allow credentials so the browser can send cookies:

```bash
./build/agentd --host 0.0.0.0 --auth-token "your_token" --auth-cookie "agentd_auth" \
  --cors-origin "https://your-ui.example" --cors-allow-credentials
```

Per-route origin policies (longest path prefix wins) are supported:

```bash
./build/agentd --cors-route '{"path_prefix":"/api/v1/openrouter","origins":["https://ui.example"]}'
```

By default, `agentd` allows common headers needed by the UI, including `Authorization` (daemon auth), `X-OpenRouter-Key`,
and request correlation headers `X-Request-Id` / `X-Trace-Id` (also exposed for browser access).
(provider key for the OpenRouter model catalog endpoint).

`ctest` includes two network smoke tests (OpenRouter + DeepSeek). They will run if keys are present
either via environment variables or `project.local.md` (gitignored). Disable them with:

```bash
export AGENT_DISABLE_NETWORK_TESTS=1
```

If you want to keep DeepSeek/Moonshot network tests but skip OpenRouter-only tests:

```bash
export AGENT_TEST_SKIP_OPENROUTER=1
```

To set keys via a local file, copy `project.local.md.example` to `project.local.md` and fill in real values.

Network tests also assume an HTTP proxy may be required; the scripts default to `http://localhost:8120`
via `https_proxy` / `http_proxy`.

### Local secrets file: `.not_in_repo` (preferred)

For local development, you can store provider keys in a gitignored file named `.not_in_repo` at the repo root.
This keeps keys out of the UI/browser, and lets `agentd` load them automatically.

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
- You can point `agentd` at a specific dotenv file with `AGENTD_DOTENV_PATH=/path/to/.env` (useful for services
  running under a different user).
- `~/.env` is a last-resort key source only; base URLs still come from flags/env/config. For Moonshot, set
  `MOONSHOT_API_BASE` or pass `--base-url https://api.moonshot.cn/v1`.

### Tool extensions: plugins + tool servers (out-of-process)

`agentd` supports adding tools without rebuilding:
- **Tool plugins** (in-process, dlopen): `--tool-plugin /path/to/plugin.so`
- **Tool servers** (out-of-process, stdio JSON-lines): `--tool-server-cmd "<cmd>"`

Tool servers are the preferred “fast bring-up” path for big integrations (Playwright, device bridges, AVM policy runners)
because failures are isolated across a process boundary. See `docs/TOOLS.md`.

## Real end-to-end (agentd + browser) test

This repo includes a “real” E2E harness that drives the Web UI in a real browser (headless) using Playwright and makes real
provider calls via `agentd`.

Prereqs:
- `.not_in_repo` populated with your provider key(s) (or env vars set)
- `./build/agentd` built (`tools/verify.sh`)
- UI deps installed (`cd ui && npm install`)

Run:

```bash
tools/e2e_real.sh
```

Logs are written under `build/e2e/`.

Notes:
- The harness expects provider keys to be available via env or `.not_in_repo` (preferred).
- `agentd` now also supports **daemon-side** key loading per-run based on the request `base_url`, so the Web UI can omit `api_key`.
- The Web UI sends a `client` identity with each run (e.g. `client.kind="webui"`). `agentd` can use this to inject a
  client-specific system prompt “profile” for default presentation/DoD semantics. See `docs/CLIENT.md`.

### Daemon config snapshot (debug)

For debugging client/daemon mismatches (CORS, sandbox defaults, job GC), `agentd` exposes:
- `GET /api/v1/config`

This endpoint requires auth when `--auth-token` is set. It intentionally does not include secrets (auth token, provider API keys).
The Web UI surfaces this snapshot in the Settings panel as “Daemon config”, including the effective `state_dir`, `sessions_root_dir`,
and `db_path` (SQLite; canonical daemon state store).

### Update daemon defaults at runtime (model/base_url + server-side keys)

`agentd` supports updating its defaults **at runtime** (persisted in `agentd`’s SQLite DB at `db_path`) so the browser UI does not need
to store provider keys.

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
- The response intentionally never includes secrets. Use `GET /api/v1/config` to see booleans like `provider_keys_set`.
- The Web UI also exposes buttons in Settings to “Save defaults to daemon” and “Save API key to daemon”.

### Daemon state dir / multi-agent safety

`agentd` stores its canonical state in SQLite (`--db-path`, default: `./agentd.db` relative to the daemon working directory).
If you run multiple `agentd` instances and want to avoid accidental session collisions, use a distinct DB path per daemon:

```bash
./build/agentd --db-path /tmp/agentd_state_1.db
./build/agentd --db-path /tmp/agentd_state_2.db
```

Legacy (non-canonical) session-store directories are still configurable for backwards compatibility and best-effort cleanup:

```bash
./build/agentd --sessions-root /tmp/agentd_sessions
```

Environment variables:
- `AGENTD_STATE_DIR`
- `AGENTD_SESSIONS_ROOT`
- `AGENTD_DB_PATH`
- `AGENTD_ACCESS_LOG` (`1` for text logs, `json` for JSON logs)
- `AGENTD_HTTP_MAX_BODY_BYTES` (default `67108864`, `0` to disable limit)
- `AGENTD_HTTP_MAX_HEADER_BYTES` (default `1048576`, `0` to disable limit)
- `AGENTD_HTTP_READ_TIMEOUT_MS` (default `15000`, `0` to disable read timeouts)
- `AGENTD_UPLOAD_MAX_BYTES` (per-file upload cap, default `33554432`, `0` to disable cap)
- `AGENTD_MAX_TOOL_CALL_ARGS_CHARS_DEFAULT` (default `0`, disables tool-arg length guard)

## Run in production (agentd + WebUI)

This repo’s “production” shape is:
- `agentd` runs as the backend daemon (tools + persistence + HTTP API)
- the Web UI runs as a separate static site (Vite build) that talks to `agentd` via HTTP
  - it can also run in **broker mode** (OIDC) and exposes a broker console for agent selection + membership management

For a full production checklist (TLS, broker/connector, auth hardening, backups), see `docs/DEPLOYMENT.md`.

### 1) Start agentd (prod defaults: YOLO + host tools)

Recommended: pick a stable working directory (so relative artifact paths like `out/foo.wav` are always resolvable) and keep
all generated files under it.

Example (local machine, loopback only; no auth required):

```bash
cd /path/to/agent
./build/agentd \
  --host 127.0.0.1 \
  --port 8123 \
  --tools host \
  --yolo \
  --host-scope "$(pwd)" \
  --tools-root "@host" \
  --db-path "$(pwd)/agentd.db"
```

Example (LAN / production; requires auth + explicit CORS allowlist):

```bash
cd /path/to/agent

# Optional: load real provider keys for the daemon process (do NOT commit secrets)
set -a
source "${HOME}/.env"
set +a

./build/agentd \
  --host 0.0.0.0 \
  --port 8123 \
  --auth-token "REPLACE_WITH_RANDOM_TOKEN" \
  --cors-origin "http://127.0.0.1:5173" \
  --cors-origin "http://127.0.0.1:8100" \
  --tools host \
  --yolo \
  --host-scope "$(pwd)" \
  --tools-root "@host" \
  --db-path "$(pwd)/agentd.db"
```

macOS convenience (launchd install/uninstall):

```bash
AGENTD_AUTH_TOKEN="REPLACE_WITH_RANDOM_TOKEN" tools/install_agentd_launchd.sh
tools/uninstall_agentd_launchd.sh
```

Notes:
- `--yolo` enables unrestricted host tools (fully autonomous, side effects allowed). The Web UI defaults to requesting this.
- `--tools` acts as a **maximum** toolset; run requests and tool registry queries cannot exceed it.
- `--tools-root "@host"` makes relative paths stable (anchored to `--host-scope`). This avoids brittle “depends on process CWD”
  behavior for artifact fetches like `GET /api/v1/file?path=out/foo.wav&yolo=1`.
- If you omit `--db-path`, `agentd` defaults to creating/using `./agentd.db` in its working directory.
- If you bind to non-loopback (`--host 0.0.0.0`), `agentd` refuses to start without `--auth-token` unless you pass `--allow-unauth`.

### 2) Start the Web UI

Dev (hot reload):

```bash
cd ui
npm install
npm run dev -- --port 5173
```

If you prefer port `8100`:

```bash
cd ui
npm run dev -- --port 8100
```

Production build + local preview:

```bash
cd ui
npm ci
npm run build
npm run preview -- --host 127.0.0.1 --port 8100
```

Optional defaults (no rebuild): edit `ui/public/agentui-config.js` (copied to `ui/dist/`) to prefill:
- `connectionMode` (`direct` or `broker`)
- `daemonBaseUrl`, `brokerBaseUrl`, `brokerAgentId`, `brokerDeploymentId`
- `daemonAuthToken`, `brokerAuthToken` (if you accept storing tokens in a static file)
- `model`, `baseUrl`, `proxyUrl`, `timeoutMs`

Then open the UI and set:
- Daemon base URL: `http://127.0.0.1:8123`
- Daemon auth token (if enabled): the same token passed to `agentd --auth-token`
- Model/provider defaults (Settings → Model / Provider), or use the OpenRouter model picker if needed.
- Run limits (Settings → Run limits) to cap `max_steps` / tool-call budgets.
- Diagnostics (Settings → Diagnostics) to confirm provider keys and run `provider_test`.

### Manual verification (macOS)
1) Broker health (if using broker/connector):
```bash
curl -k https://127.0.0.1:8443/healthz
```
2) Agentd health:
```bash
curl http://127.0.0.1:8123/api/v1/health
curl http://127.0.0.1:8123/api/v1/ready
curl http://127.0.0.1:8123/metrics
```
Optional diagnostics (requires auth if enabled):
```bash
curl http://127.0.0.1:8123/api/v1/diagnostics
curl http://127.0.0.1:8123/api/v1/diagnostics/providers
```
See `docs/DIAGNOSTICS.md` for provider_test usage.
`/api/v1/diagnostics/providers` includes `base_url_source` (`config`/`env`/`default`) to explain how a provider base URL was chosen.
3) UI functional check:
- Run a short prompt that emits audio (artifact or scene).
- If autoplay is blocked, click once in the UI to unlock media playback.

If you want a one-command local check (agentd + WebUI only, no Docker/broker):
```bash
tools/verify_mac_local_stack.sh
```
Optional env:
- `MAC_LOCAL_SKIP_UI=1` (skip WebUI build/serve)
- `MAC_LOCAL_UI_INSTALL=0` (skip `npm ci` if deps already exist)
- `MAC_LOCAL_PROVIDER_TEST=1` (run diagnostics provider tests if keys are available)
- `MAC_LOCAL_PROVIDER_TEST_TIMEOUT_MS=30000` (provider test timeout override)

The Web UI defaults to:
- YOLO enabled
- client RPC enabled
- client RPC side effects enabled
- full tool-call/event visibility in History (so users can inspect tool arguments/results)

Reliability notes (important for production UX):
- The UI persists the active async `job_id` + SSE cursor in `localStorage`, so a browser refresh can resume a running job stream.
- The UI persists the selected `session_id` per daemon base URL in `localStorage` (helps when running multiple local daemons).
- The UI persists the Scene (client-side entities) per `session_id` in `localStorage`.
- The UI posts client acknowledgement events (`ui_action_shown`, `client_rpc_result`, `artifact_rendered`/`artifact_render_failed`)
  so agents can implement deterministic “Definition of Done” handshakes (see `docs/DOD_ACK.md`).

Vendored subtrees live under `ref/` and are treated read-only in this repo.
If changes are needed, update upstream or copy into first-party code. See
`docs/VENDORED.md`.

## Cleanup (disk usage)

Build artifacts and run logs can grow quickly (especially `build/`, `build-core*/`, and `out/`). To keep the repo lean:

```bash
tools/clean.sh
```

Options:
- `--aggressive`: drop build*/out regardless of size + UI build caches.
- `--purge-deps`: remove dependency caches (`ui/node_modules`, `.agent_deps`, `ref/*/venv`).
- `--purge-state`: remove stateful data (`state/`, `db/`, `memory/`, `session_*`) — **data loss**.
- `--purge-ref-git`: remove nested `.git` dirs under `ref/` for vendored repo bloat.
- `--report`: print a repo size report after cleanup.
- `--dry-run`: preview what would be deleted.
- `--out-max-days N`: prune log files older than N days (0 = delete all).
- `--threshold-gb N`: size threshold (GiB) for build/out removal.
- `--max-repo-gb N`: fail if total repo size exceeds N GiB after cleanup.

To inspect disk usage and spot bloat quickly:

```bash
tools/repo_size_report.py --depth 2 --top 20
```

Largest files (helps find sudden bloat):

```bash
tools/repo_size_report.py --largest-files 20 --largest-min-bytes 1048576
```

Exclude common bulky paths (git objects, builds, deps):

```bash
tools/repo_size_report.py --exclude-defaults --depth 2 --top 20
```

CI guard (fail if the repo exceeds a size cap):

```bash
tools/repo_size_report.py --exclude .git --max-total-gb 5 --fail-on-nested-git
```

List nested .git dirs (useful before running cleanup):

```bash
tools/repo_size_report.py --list-nested-git
```

Stub file scan (fails on empty/placeholder files):

```bash
tools/stub_file_scan.py --fail
```

Tracked file size guard (fails if any tracked file exceeds 10 MiB):

```bash
tools/tracked_file_guard.py --max-mb 10
```

Untracked file size guard (ignores common build/cache dirs):

```bash
tools/untracked_file_guard.py --exclude-defaults --max-mb 100
```

Vendored subtree guard (fails if `ref/` changes relative to base; requires
base refs in CI; also checks staged/unstaged working tree locally):

```bash
tools/vendored_guard.py --path ref
```

Optional git hook (local pre-commit) to run the vendored guard:

```bash
tools/install_git_hooks.sh
```

Remove the hook later:

```bash
tools/uninstall_git_hooks.sh
```

Check local hook status:

```bash
tools/hooks_status.sh
```

JSON output:

```bash
tools/hooks_status.sh --json
```

Fail if the vendored guard is not installed:

```bash
tools/hooks_status.sh --check
```

Scriptable check with JSON:

```bash
tools/hooks_status.sh --json --check
```

If you use a custom hooks directory:

```bash
git config core.hooksPath .githooks
tools/install_git_hooks.sh
```

Hook modes (vendored guard):

- Verbose pre-commit output (when debugging vendored guard issues):

```bash
VENDORED_GUARD_VERBOSE=1 git commit
```

- Install a permanently verbose hook:

```bash
tools/install_git_hooks.sh --verbose
```

- Quiet pre-commit output (only lists changed paths on failure):

```bash
VENDORED_GUARD_QUIET=1 git commit
```

- Precedence (if both set): `VENDORED_GUARD_QUIET` overrides `VENDORED_GUARD_VERBOSE`.

- Install a permanently quiet hook:

```bash
tools/install_git_hooks.sh --quiet
```

See `docs/VENDORED.md` for the full vendored policy and hook details.

Run all repo hygiene guards locally:

```bash
tools/verify_repo_guards.sh
tools/verify_repo_guards.sh --strict
tools/verify_repo_guards.sh --max-total-gb 2 --max-file-mb 5
tools/verify_repo_guards.sh --max-untracked-mb 200
```

## Docker Compose (prod-like local verification)

If you want a **prod-like stack** locally (Postgres + Keycloak OIDC + broker + connector + agentd + WebUI):

```bash
./tools/verify_compose_stack.sh
```

Note: the WebUI container mounts `tools/agentui-config.compose.js` to default to broker mode (edit as needed).
If you want prebuilt images instead of local builds, set:
- `BROKER_IMAGE`, `AGENTD_IMAGE`, `CONNECTOR_IMAGE`, `WEBUI_IMAGE`
- run with `COMPOSE_BUILD=0 COMPOSE_PULL=1` to pull missing images automatically.

If Docker is unavailable or resource constrained, use:
```bash
tools/verify_mac_local_stack.sh
```

If Docker builds are blocked but Docker can run containers, use the host-mode stack:
```bash
tools/verify_mac_full_stack_host.sh
```

Notes:
- The script will auto-pick free host ports for services that commonly conflict (Broker/Keycloak/Postgres) and export:
  - `BROKER_PUBLISHED_PORT` (default 8443)
  - `KEYCLOAK_PUBLISHED_PORT` (default 8081)
  - `POSTGRES_PUBLISHED_PORT` (default 5433)
- It will also auto-pick free host ports for the user-facing services:
  - `WEBUI_PUBLISHED_PORT` (default 8100)
  - `AGENTD_PUBLISHED_PORT` (default 8123)
- It sets `COMPOSE_PROJECT_NAME` automatically (defaults to `agent_${WEBUI_PUBLISHED_PORT}`) so you can run multiple stacks concurrently.
- Keycloak is intentionally accessed via `keycloak.lvh.me` (resolves to `127.0.0.1`) so the `iss` claim in minted JWTs
  matches what the broker validates. If you request tokens via `http://127.0.0.1:<port>`, you’ll get issuer-mismatch errors.
- If Docker build hits `unpigz`/`runc` resource errors, restart Docker Desktop or increase CPU/RAM (Settings → Resources),
  and consider raising the disk image size. You can also set `PIGZ=-p1 GZIP=-p1` to reduce decompression pressure,
  or skip builds by using prebuilt images (`COMPOSE_BUILD=0 COMPOSE_PULL=1` with `BROKER_IMAGE`, `AGENTD_IMAGE`,
  `CONNECTOR_IMAGE`, `WEBUI_IMAGE` set).

This starts:
- WebUI: `http://127.0.0.1:${WEBUI_PUBLISHED_PORT}`
- agentd: `http://127.0.0.1:${AGENTD_PUBLISHED_PORT}` (auth token: `dev-agentd-token`)
- Keycloak: `http://keycloak.lvh.me:${KEYCLOAK_PUBLISHED_PORT}` (realm: `agentd`, user/pass: `test`/`test`)
- Broker: `https://127.0.0.1:${BROKER_PUBLISHED_PORT}` (self-signed CA for local dev; mTLS for connectors)

## Git remote (publishing)

This workspace may not have a git remote configured. If `git push` fails with “No configured push destination”,
configure `origin` explicitly:

```bash
git remote add origin <your_repo_url>
git push -u origin "$(git rev-parse --abbrev-ref HEAD)"
```

Or use the helper script (does not guess a URL):

```bash
AGENT_GIT_REMOTE_URL="<your_repo_url>" tools/setup_git_remote.sh --push
```

To run a full local verify + push in one command:

```bash
tools/publish.sh --skip-ui
```

The helper can also read `git_remote` from your gitignored `project.local.md`:

```bash
cp project.local.md.example project.local.md
# edit project.local.md and set:
# - git_remote: <your_repo_url>
tools/setup_git_remote.sh --push
```

If `origin` exists but points to the wrong place, pass `--force` to update it:

```bash
tools/setup_git_remote.sh --url "<your_repo_url>" --force --push
```

Note: there are additional host-tool network smokes (DeepSeek/OpenRouter) that validate bounded tools like
`fs_read` and `fs_find` end-to-end (model → tool call → tool output).

## CLI usage

### One prompt (persisted session by default)

```bash
export OPENAI_API_KEY=...
./build/agent run "hello"
```

Notes:
- The CLI prints the assistant text to **stdout**.
- When `--trace` is enabled (default), the CLI prints a compact transcript (tool calls + results + errors) to **stderr** so piping stdout remains clean.
- Use `--transcript-jsonl <path>` to additionally append raw tool-loop events as JSONL (useful for UI replay/debugging).

This uses session id `default` and stores it at `~/.agent/sessions/default.sess` (portable, JSON-free). If built with JSONCPP,
it also writes `~/.agent/sessions/default.json` for debugging/interoperability.
If this project is built without JSONCPP, `.sess` is the only persisted session format.

### Proxy (when outbound networking requires it)

If your environment requires an HTTP proxy for outbound HTTPS, pass an explicit proxy URL (or set env `HTTPS_PROXY` / `http_proxy`):

```bash
./build/agent run "hello" --proxy http://localhost:8120
```

### Explicit session id

```bash
./build/agent run "continue" --session myproj
```

### Disable persistence (ephemeral run)

```bash
./build/agent run "one shot" --no-session
```

### OpenRouter (optional headers)

```bash
export OPENROUTER_API_KEY=...
export OPENROUTER_API_BASE=https://openrouter.ai/api/v1
export OPENROUTER_HTTP_REFERER=https://example.com
export OPENROUTER_X_TITLE="agent"
./build/agent run "hi" --base-url "$OPENROUTER_API_BASE" --model "google/gemini-2.0-flash-001"
```

### Pick an OpenRouter verification model (cheap + tools + multimodal)

This repo includes a small host/dev utility that fetches OpenRouter's model catalog, filters to:
- multimodal-capable inputs (image/audio/video)
- OpenAI tools/tool_choice support
- total pricing within a range (default: $0.01–$0.50 per 1M tokens, prompt+completion)

It writes a report to `ref/openrouter/multimodal_latest.md` and prints a recommended model id:

```bash
python3 tools/openrouter_models.py --write
```

### DeepSeek tool use (calculator)

`deepseek-reasoner` can do tool calls but may reject forcing `tool_choice`, so for deterministic tool verification use `deepseek-chat`:

```bash
./build/agent run "Use calculator to compute (2+2)*10. Return exactly: 40" \
  --no-session \
  --base-url "https://api.deepseek.com" \
  --api-key "$DEEPSEEK_API_KEY" \
  --model "deepseek-chat" \
  --tools basic \
  --force-tool calculator \
  --require-tool-call
```

### Multimodal attachments (images + files)

The CLI supports attaching local files via `--attach <path>` (repeatable).

Behavior:
- Images (`.png/.jpg/.webp/...`) are sent as base64 `data:` URLs (model must support image inputs).
- Non-image files are included as **best-effort text excerpts** (binary files get a short base64 preview).
- Not every provider/model supports multimodal inputs. Even within the same provider, different models may accept/reject images.
  - If the model rejects the request, you'll typically see an HTTP `400` with a message like “image input not supported”.

Example (OpenAI-compatible vision-capable model):

```bash
./build/agent run "Describe this image." \
  --no-session \
  --tools none \
  --base-url "https://api.openai.com/v1" \
  --model "<vision-capable-model-id>" \
  --attach ./image.png
```

Example (Moonshot / Kimi K2.5, OpenAI-compatible API; base64 images only):

```bash
./build/agent run "Describe this image." \
  --no-session \
  --tools none \
  --base-url "https://api.moonshot.cn/v1" \
  --model "kimi-k2.5" \
  --api-key "$KIMI_API_KEY_CN" \
  --attach ./image.png
```

### Extensible tools (embedded-friendly)

Tool definitions and execution are host-pluggable:
- Core stores tool schemas in a portable registry: `core/include/agent/tools.h`
- Hosts implement the executor callback (GPIO, sensors, local RPC, etc.)
- CLI wires:
  - `basic` toolset (calculator) via `cli/src/toolset_basic.cpp`
  - `host` toolset (subprocess exec + diff-based file edits) via `cli/src/toolset_host.cpp`

Host toolset usage:

```bash
./build/agent run "List files, then show README.md" --tools host --tools-root .
```

### Runaway tool-call guard (repeat limit)

Some failure modes look like “capture camera → register artifact → repeat forever”. To keep runs bounded, the tool loop has a
guard that stops if the model repeats the **exact same tool call** too many times:

- CLI: `--max-repeated-tool-calls <n>` (default: `12`; set `0` to disable)
- Daemon/UI: request field `max_repeated_tool_calls` (see `docs/PROTOCOL.md`)
- For daemon runs, `max_steps` can be left blank in the UI to use the daemon’s default step cap (`/api/v1/config: daemon.max_steps_default`).
- For worst-case safety, daemon also applies a default cap on total tool calls (`/api/v1/config: daemon.max_tool_calls_total_default`).
- To guard against oversized tool call arguments, daemon can apply a default cap on tool call JSON length
  (`/api/v1/config: daemon.max_tool_call_args_chars_default`, `0` disables).
- For targeted safety (without breaking benign high-frequency tools like `fs_read`), daemon can apply explicit per-tool limits:
  - daemon defaults are visible at `/api/v1/config: daemon.tool_call_limits_default`
  - configure via `agentd --tool-call-limit proc_exec=4 --tool-call-limit shell_exec=16 --tool-call-limit artifact_register=16 --tool-call-limit ui_action=16`
  - or via env: `AGENTD_TOOL_CALL_LIMITS_DEFAULT="proc_exec=4,shell_exec=16,artifact_register=16,ui_action=16"`
  - per-run overrides: request field `tool_call_limits` (see `docs/PROTOCOL.md`)
  - defaults can be persisted via `/api/v1/config/update`:
    `max_steps_default`, `max_tool_calls_total_default`, `max_tool_calls_per_tool_default`,
    `max_tool_call_args_chars_default`, `tool_call_limits_default`

Seamless compaction for tool loops:
- In `--tools basic` / `--tools host` mode, the tool loop applies the same char-budget policy as the core runner:
  keep pinned `system` prefix + keep last `K` messages.
- When messages are dropped, a short deterministic `system` summary is inserted into the request so the model
  has some continuity (no extra model call).
- If the provider rejects a request as too large (context length / too many tokens), the tool loop retries after
  compacting more aggressively. This is effectively “spawning a new session” for stateless backends.

Session vs audit:
- Session message history:
  - `~/.agent/sessions/<id>.sess` (portable, JSON-free; primary)
  - `~/.agent/sessions/<id>.json` (optional, written when built with JSONCPP)
  stores **user/assistant conversation** only.
- Detailed tool timelines (tool calls/results + LLM request/response events) are stored in the per-session audit log
  (`~/.agent/sessions/<id>.events.jsonl`) and surfaced via CLI `--trace` output.
- Daemon (`agentd`) state is separate: sessions + audit live in SQLite (`db_path`, default: `./agentd.db`) and are exposed via
  endpoints like `GET /api/v1/session` and `GET /api/v1/session/audit`.

Host tool names:
- In `--host-policy readonly`, the host tool registry omits `shell_exec`, `proc_exec`, and `file_apply_patch` (read-only inspection only).
- In daemon restricted mode (`yolo=false`), the host tool registry omits `shell_exec` and `proc_exec` even when `host_policy=full`.
  Filesystem tools remain available; agentd no longer enforces a `tools_root` sandbox at runtime (use container-level isolation for multi-tenant safety).
- `shell_exec` (runs `/bin/sh -lc <cmd>`, returns JSON envelope with `exit_code`, `timed_out`, `truncated`, `output`)
- `proc_exec` (runs an argv array via `posix_spawnp`, no shell; returns JSON envelope with `argv`, `exit_code`, `timed_out`, `truncated`, `output`)
- `file_apply_patch` (applies a unified diff via `git apply`; returns the patch as a diff-style audit trail)
- `fs_stat` (file/dir metadata; returns structured fields + a human-readable `output`)
- `fs_list` (bounded directory listing; returns structured `entries` + `output`)
- `fs_find` (bounded file discovery; returns structured `entries` + `output`)
- `fs_read` (bounded file read with pagination by line; returns `content`/`output` + `has_more` + `next_start_line`)
- `text_search` (token-safe substring search; returns structured `matches` + `output`)

### UI actions (bidirectional UX)

In addition to `artifact_register` (explicit media artifacts), host tools include `ui_action` so the model can request
UI-side actions in a **typed** and **allowlisted** way (e.g. notifications, audio playback UI).

See `docs/CLIENT.md`.

Notes:
- For **inspection** (read/list/stat), prefer `fs_list` / `fs_read` / `fs_stat` because they provide bounded output and pagination
  (helps prevent token/context blow-ups). Use `rg/grep` first, then `fs_read` for narrow line ranges.
- For **file discovery**, prefer `fs_find` over `find`/`tree` when you need predictable output size and default excludes.
- For **search**, prefer `text_search` over `grep -R` when you need predictable output size (bounded matches + per-file size limits).
  - Optional: use `extensions` (e.g. `[".cpp",".h"]`) to restrict scanning.
- For host-side **mutating** file operations (remove/move/rename), prefer OS-native commands via `proc_exec` / `shell_exec` (e.g. `rm`, `mv`, `git`).
  If you are running the daemon in scoped mode (`yolo=false`), exec tools are omitted; use `file_apply_patch` for edits.
- For file edits, prefer `file_apply_patch` so the tool output includes a diff-style record of the change.
- Tool outputs are capped before being inserted back into the next LLM request context, to avoid overflowing the context window.

Default host system hint (CLI/daemon):
- When using `--tools host`, the CLI/daemon injects a one-time `system` message into an empty session to encourage
  **fast incremental inspection** (use `rg/grep`, `head`, `tail`, `awk`, `sed -n`) instead of reading large files wholesale.
- CLI: disable with `--no-default-system` or override with `--system "<your prompt>"`.
- CLI: select a built-in prompt profile with `--system-profile default|jules_codex`.
- Daemon: disable with `./build/agentd --no-default-system` or per-request with `no_default_system: true`;
  override per-request with `system: "<your prompt>"`.
- Daemon: select a built-in prompt profile with `./build/agentd --system-profile default|jules_codex`, env `AGENTD_SYSTEM_PROFILE`,
  or per-request with `system_profile: "default" | "jules_codex"`.

Optional LLM summaries for compaction (`--tools none`):
- In `--tools none` mode, you can optionally provide `--summary-model <name>` to generate a short LLM summary of the
  messages that are about to be dropped during compaction.
- The host inserts the summary as a `system` message starting with `AGENT_SESSION_SUMMARY_PREFIX` so the core can treat it
  as **not pinned** (allowing it to be replaced/compacted later instead of growing the pinned prefix forever).
- This is optional because it costs an extra model call; the default compaction behavior does not require it.

### Chat REPL

```bash
./build/agent chat --session default --tools host --tools-root .
```

Commands:
- `/exit` or `/quit` ends the REPL.

## Core library

- Header: `core/include/agent/agent.h`
- Scope: session model + char-budget compaction + role helpers
- No environment variable access in the core (host-only concern).

## Daemon + Web UI (day-1)

The recommended UX direction is **daemon-first**: run `agentd` locally and use the Web UI (or CLI) as a client.

### Run the daemon

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

SQLite DB (mandatory; canonical daemon state):

```bash
./build/agentd --host 127.0.0.1 --port 8123

# or pin it explicitly (recommended for production / multi-daemon):
./build/agentd --host 127.0.0.1 --port 8123 --db-path "$HOME/.agent/agentd.db"
```

The DB stores sessions, runs, events, tool records, artifacts, UI actions, client events, and audit records for debugging/replay.
See `docs/DB.md` for query examples.

Health check:

```bash
curl http://127.0.0.1:8123/api/v1/health
curl http://127.0.0.1:8123/api/v1/ready
```

YOLO vs host-scoped tools:
- Default daemon mode is YOLO (unrestricted) to match local development needs.
- `yolo` is now a *tool exposure* knob (primarily enabling/disabling process execution tools). It does not sandbox filesystem paths.
- For safer deployments, `agentd` supports `--host-policy full|readonly`:
  - `full`: enables process exec + patch application + filesystem inspection (`shell_exec`, `proc_exec`, `file_apply_patch`, `fs_*`, `text_search`)
  - `readonly`: disables process exec and patch application (keeps only `fs_*` + `text_search`)
  - Requests can also pass `host_policy: "readonly"` to *tighten* permissions for that run (cannot expand beyond daemon default).

Verbose inspection:
- Pass `verbose: true` to `POST /api/v1/run` to return a structured `events` log suitable for UIs
  (LLM request/response, tool calls, tool results).
- Pass `trace: true` to also return a plain-text transcript (`trace_text`).
- Optional: pass `max_capture_bytes` to cap large verbose event payloads for UI stability (default: 256KB; daemon clamps for tool loops).
- `agentd` ignores `SIGPIPE` so client disconnects (UI refresh, SSE close) do not terminate the daemon.
- The daemon also serves files for previews at `GET /api/v1/file?path=<...>&session_id=<sid>` (up to 10MB).
  - If `session_id` is provided and `path` is relative, the file is resolved under `<sessions_root>/<sid>/...`.
  - If `path` is absolute, it is served directly.

Assistant streaming (provider-dependent):
- Clients can set `stream_assistant: true` to request OpenAI-compatible SSE streaming (`stream: true`).
  - `tools: "none"`: the daemon emits `assistant_delta` events while the assistant message is streaming.
  - `tools: "basic"` / `"host"`: the daemon uses streaming requests for tool-loop steps and (best-effort) reconstructs tool calls
    from streamed `delta.tool_calls` (and legacy `delta.function_call`). It also emits `assistant_delta` events during the final assistant step (provider-dependent).
    Note: some providers ignore `stream: true` and return a normal JSON completion; the daemon falls back to non-stream parsing.
  - Implementation notes: `docs/STREAMING.md`

CLI streaming (stdout):
- For `agent run` / `agent chat`, pass `--stream-assistant` to stream assistant deltas to stdout (provider-dependent).
  - `--tools none`: streams the assistant message for a single request (`stream: true`).
  - `--tools basic|host`: streams assistant deltas during tool-loop steps (best-effort; depends on provider streaming + `delta.tool_calls`).

Tool schema introspection (extensible tools):
- `GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=<id>` returns the tool registry the daemon will expose:
  `name`, `description`, `parameters_json` (OpenAI-compatible JSON Schema).
- Tool exposure is also constrained by the daemon's `--host-policy` (response includes `effective_host_policy`).
- Requests cannot exceed the daemon's `--tools` setting; exceeding it returns HTTP 400.
- The response includes `daemon_tools` and (when provided) `requested_tools` for clarity.
- When `session_id` is provided (and `tools=host`), the registry may include **session-scoped tools** such as `ui_wait_event`.
- This is intended for “day-1 rich UI” features (rendering tool info, validating tool-call args) and for future clients.

OpenRouter model discovery (for verification + multimodal/tools filtering):
- `GET /api/v1/openrouter/models?min_total=0.01&max_total=0.50&require_multimodal_input=1&require_tools=1&limit=50`
  fetches OpenRouter’s `/models` catalog (using `OPENROUTER_API_KEY` or an `Authorization: Bearer ...` header),
  filters it, sorts by total price ($/1M prompt+completion), and returns a recommended cheapest model id.

Session browsing:
- `GET /api/v1/sessions` lists known sessions.
- `GET /api/v1/session?session_id=<id>` returns the message history.
- `GET /api/v1/session/audit?session_id=<id>&include_rotated=0|1` returns recent per-run audit entries (prompt + assistant + events).

Async runs (UI-friendly):
- `POST /api/v1/run_async` starts a background run and returns `{ ok, job_id }`.
- `GET /api/v1/job?job_id=<id>` returns `{ ok, status, result? }` (result shape matches `/api/v1/run`).
  - For live UI progress without SSE/WebSocket:
    - Add `include_events=1` to include the tool/LLM `events` captured so far (best-effort).
    - Use `cursor=<n>&max_events=<m>` to tail new events incrementally while the job is running.
- `GET /api/v1/job/stream?job_id=<id>&cursor=<n>` streams job progress via SSE:
  - emits `agent_event` (same objects as the `events` array)
  - ends with `job_done` containing the final `result`
- While jobs are running, the daemon may emit `heartbeat` events when no other events have been produced for a short period.
  This makes UIs resilient to slow providers and long-running tools (e.g. `sleep`, builds) without requiring token streaming.

Job cancellation (best-effort):
- `POST /api/v1/job/cancel?job_id=<id>` requests cancellation.
- Cancellation is cooperative:
  - the tool loop will stop at safe boundaries (between tool calls / LLM requests)
  - long-running host tools (`shell_exec` / `proc_exec`) will terminate their subprocess when cancellation is requested

### Run the Web UI

```bash
cd ui
NPM_CONFIG_CACHE=../build/npm-cache npm ci
npm run dev
```

Then open the dev server URL (defaults to `http://localhost:5173`) and point it at the daemon base URL.
Using a repo-local `NPM_CONFIG_CACHE` avoids permission issues with the global npm cache in some environments.

UI rendering notes:
- The UI renders a **Conversation** (message cards) derived from the daemon `events` stream:
  user prompt → assistant messages → tool calls/results.
- Markdown is rendered with GFM + syntax highlighting for code blocks.
- The settings panel is collapsible and all settings persist in the browser via `localStorage`.
- If outbound networking requires a proxy, set **HTTPS proxy** in the UI settings (it is sent as `proxy` in `POST /api/v1/run`).

Host filesystem tools (token safety):
- `fs_list` is designed for bounded output and now excludes common huge directories by default (e.g. `node_modules`, `build`, `dist`).
  - To include them, pass `use_default_excludes: false` (and/or `exclude_names` to fine-tune).
- `fs_list`, `fs_find`, and `text_search` support `exclude_globs` (fnmatch) to filter out noisy paths (generated files, vendored code, etc.).
- `fs_list`, `fs_find`, and `text_search` support `respect_gitignore: true` to skip `.gitignore`d paths (best-effort; currently reads the repo-root `.gitignore`).
- `fs_stat` supports an optional bounded line count for small text files (`count_lines: true`) so the model can decide whether a file is huge without dumping it.
- `fs_read` supports paging (`start_line`, `max_lines`, `end_line`) and a character cap (`max_chars`).
---

# Source: DESIGN.md

# Agent Project – Architecture & Design

Last updated: 2026-02-19

## Goals

- **Fast startup + small footprint** compared with heavy Python stacks.
- **OpenAI-compatible backends** (OpenAI, OpenRouter, DeepSeek, Gemini via routing, etc.) by using the OpenAI Chat Completions API shape (or compatible).
- **Seamless compaction** so long-running sessions remain usable within context limits.
- **Multi-runtime portability**
  - Desktop CLI / daemon: full features (env/config, persistence, broker connectivity).
  - Embedded (ESP32-class) and VM targets (Oren AVM): minimal assumptions (no env, limited storage).
- Optional deterministic “policy VM” layer (future): a small VM/runtime can execute routing/retry/aggregation logic without LLM calls.
  - Integration contract (draft): `docs/spec/agent_vm_port_v0.md`
  - Ecosystem leverage notes: `docs/OREN_LANG_ECOSYSTEM.md`

## Additional goals (for the first milestone)

- Full tool ecosystem (shell, filesystem, browser automation).
- Full streaming / incremental tool call protocol coverage.
- Guaranteed token-accurate budgeting for every model (tokenizers are model-specific and often heavy).

## Promoted goals (formerly non-goals)

There are no “non-goals” in this project. Anything that was previously deferred is now an explicit goal tracked in the
Roadmap section below and `TODOS.md`, including:

- Staging/rollback-friendly OTA updates with explicit auditability.
- Multi-deployment fanout controls surfaced in broker + WebUI.
- Dynamic memory: observation capture + progressive disclosure (search → timeline → get).

## Design Doc Map

- `docs/HANDBOOK.md`: unified, generated bundle of all docs (edit `docs/handbook/OVERVIEW.md` and the source docs below,
  then run `tools/build_handbook_bundle.py`).
- `DESIGN.md` (this doc): system goals, boundaries, layering, and cross-cutting policies.
- Source docs included in the handbook bundle:
  - `docs/BROKER.md`: broker relay design, trust model, and HTTP/WSS API.
  - `docs/CLIENT.md`: client collaboration model (events, UI actions, DoD, client RPC).
  - `docs/AGENTD_LIB.md`: embedding `agentd` in-process and tool-extension interfaces.
  - `docs/PROTOCOL.md`: run/artifact protocol envelopes and semantics.
  - `docs/WORKFLOWS.md`: workflow engine model and task semantics.
  - `docs/STREAMING.md`: streaming compatibility matrix and behavior notes.
  - `docs/MEMORY.md`: memory retention, salience, and recap architecture.
  - `docs/DIAGNOSTICS.md`: diagnostics endpoints and provider health checks.
  - `docs/DB.md`: daemon SQLite store, blob storage tiers, and DB query API.
  - `docs/DOD_ACK.md`: UI-visible “definition of done” handshake semantics.
  - `docs/spec/README.md`: index of versioned protocol/spec deep dives.

## Key Decisions (Facts / Constraints)

1) **Core library must not depend on environment variables**
   - Many runtimes (embedded MCU, VM) may not expose `getenv()` meaningfully or at all.
   - Therefore: env parsing is an adapter/host concern (CLI/daemon), not a core concern.

2) **Session storage is optional in core, mandatory in host apps**
   - CLI/daemon benefit from persisted sessions; embedded often cannot afford large histories.
   - Therefore: core owns the in-memory *session model*, while persistence is provided by the host.

   Practical compromise (Milestone 1.5):
   - Core provides a **JSON-free, line-based session codec** (`agent/session_codec.h`) so hosts can persist/restore sessions
     without a JSON library (embedded-friendly).
   - Hosts may additionally write an OpenAI-ish JSON view (`.json`) when JSONCPP is available, but the portable `.sess` format
     is treated as the primary persisted session file.

   Planned portability port (Milestone 2.1):
   - Core defines an optional **persistence interface** (`agent/persist.h`) so hosts/embedded targets can supply:
     - filesystem persistence (`.sess` on desktop)
     - SQLite (desktop/daemon)
     - NVS/flash key-value storage (MCU)
   - The core itself remains storage-agnostic and does not assume POSIX paths or env vars.

3) **Transport is an injected interface**
   - Embedded environments may not support libcurl/POSIX sockets.
   - Therefore: core defines a transport “port” (function pointers / vtable).
   - The CLI provides a libcurl-backed implementation; embedded provides its own.

   Milestone 1 scope: the core stays strictly network-agnostic; the desktop CLI owns the HTTP client.
   The transport “port” is a planned addition once the request/streaming surface is stabilized.

4) **Compaction is policy-driven and supports multiple budgeting strategies**
   - Token counting is model-specific and may require heavy tokenizer libraries.
   - Therefore: core supports a portable **character-budget** strategy by default, and can accept a
   host-provided token counter in the future.

5) **Provider APIs are usually stateless; “session id” is a client concern**
   - OpenAI-compatible Chat Completions APIs typically do not store conversation state on the server.
   - DeepSeek explicitly documents `/chat/completions` as “stateless”: clients must concatenate and resend prior messages.
   - Therefore our `session_id` is a **local agent/session store concept**, not a provider feature.
   - Some providers offer **context caching** (prefix-cache / KV-cache) which reduces cost/latency when request prefixes repeat,
     but this is not the same as a server-side session handle.

## Layering Overview

The stack is layered so the portable core stays env-agnostic while host services and clients evolve independently.

### 1) `agent_core` (portable core, C API)

Responsibilities:
- Session model: messages, roles, timestamps (optional), metadata.
- Compaction policy: decide when to compact and what to keep (portable char budget).
- Deterministic prompt assembly decisions (ordering, pinned messages).
- Tool abstractions: portable tool registry + host-provided tool executor callbacks.
- No assumptions about env, filesystem, argv, network availability.

Prohibitions:
- No `getenv`, no reading `.env` / `~/.config`, no CLI flag parsing.
- No mandatory libcurl / sockets dependency.
- No mandatory persistence dependency (SQLite/files).

Interfaces exported:
- `agent_session_*`: create/destroy session, append messages, compact, iterate messages.
- Optional hooks for custom allocators (useful for embedded).

### 2) `agentd` (daemon host service)

Responsibilities:
- HTTP/SSE API and run orchestration (`/api/v1/run`, `/api/v1/job`, client events).
- Session persistence, audit logs, and artifact handling.
- Tool loop execution + tool plugins / tool servers.
- Runtime config store, safety limits, and optional broker connector.

See: `docs/CLIENT.md`, `docs/PROTOCOL.md`, `docs/TOOLS.md`.

### 3) `agent_cli` (standalone desktop client)

Responsibilities:
- Parse flags + environment variables + config files (policy owned by CLI).
- Provide transport implementation (libcurl).
- Provide persistence implementation (file-based session store; optional SQLite).
- Call core APIs to manage session and compaction.
- Provide host toolsets (filesystem operations, process execution) for tool-calling models.

### 4) Broker (optional relay / control plane)

Responsibilities:
- Agent registry, membership authZ, and audit.
- mTLS agent connectivity + OIDC/JWT client auth.
- Relay HTTP/SSE between clients and agentd deployments behind NAT.

See: `docs/BROKER.md`.

### 5) Clients (WebUI + integrations)

- WebUI: primary UX for runs, artifacts, diagnostics, and ops.
- Other clients: mobile, Slack, backend services, or thin CLIs.

See: `docs/CLIENT.md`.

### 6) Embedded / AVM adapters (future)

- `agent_embed` targets ESP32 / Oren AVM with minimal storage and transport.
- Optional policy VM to run deterministic routing/retry without LLM calls.

## System Flows (Summary)

This section consolidates the most important cross-cutting flows so the system
can be understood without hopping across multiple design docs.

### Run flow (client → agentd → UI)

- Clients submit runs to `agentd` (`/api/v1/run` or `/api/v1/run_async`).
- `agentd` executes the tool loop, persists events/audit records, and emits
  structured events (SSE) defined in `docs/spec/run-events/run_events_v1.md`.
- UI-visible effects (artifacts, UI actions) use the DoD handshake so the agent
  can deterministically stop once the UI acknowledges delivery (see `docs/DOD_ACK.md`).

### Broker relay flow (client → broker → agentd)

- Agents connect **outbound** to the broker via mTLS (`/v1/agent/connect`).
  The broker verifies the client cert against `--tls-client-ca` and matches
  the cert CN to `agent_id` (see `docs/BROKER.md`).
- Clients authenticate with OIDC/JWT bearer tokens (optional cookie auth is
  supported for browser environments).
- The broker proxies requests via:
  - `/v1/agents/{agent_id}/proxy/...` for HTTP
  - `/v1/agents/{agent_id}/proxy_sse/...` for SSE streams
- Optional idempotency keys (`Idempotency-Key` / `X-Idempotency-Key`) provide
  safe retries for proxied requests.

### Persistence & observability (agentd)

- The daemon persists sessions and run/audit records to the state root.
- When `--db-path` is enabled, it mirrors runs/events/tools/artifacts into
  SQLite and exposes read-only query endpoints (see `docs/DB.md#db-query-api-troubleshooting`).
- Diagnostics endpoints (`/api/v1/diagnostics*`) provide fast health snapshots
  and provider key presence checks (see `docs/DIAGNOSTICS.md`).

## Day-1 Product Direction (Decision)

We prioritize a **daemon-first** architecture with multiple clients:

- `agentd` (daemon) is the **source of truth** for:
  - session persistence and compaction policy
  - tool loop execution and tool plugins
  - transcript/audit logging (LLM requests, responses, tool calls, tool outputs)
  - optional broker connectivity (relay)
- Clients are replaceable front-ends:
  - Local Web UI is the **primary “beautiful UX”** surface for daemon-backed workflows (rich interactions, diff views, filtering, etc.).
  - `agent` CLI remains a **standalone host adapter** for local runs and a useful debug surface.

Rationale:
- Terminal TUIs are expensive to make “feature-rich” (diff viewers, tool panes, multimodal previews, search/filter).
- A browser UI makes rich inspection and editing workflows much easier.
- A daemon aligns naturally with the future goal of outbound broker connectivity behind NAT.

## Host Tool Sandbox Policy (Daemon/CLI)

When enabling host tools (filesystem inspection, process execution, patch application), we need an explicit safety knob that
works both for local CLI runs and for daemon deployments that may be reachable over a network.

Policy modes (first milestone):

- `full`: enable all host tools (`shell_exec`, `proc_exec`, `file_apply_patch`, `fs_*`, `text_search`).
- `readonly`: disable process execution + patch application (`shell_exec`, `proc_exec`, `file_apply_patch`), keeping only
  bounded inspection tools (`fs_*`, `text_search`).

Additional sandbox knob:

- `yolo` (daemon request/query): when disabled, the daemon **disables process execution tools**
  (`shell_exec` / `proc_exec`) even if `host_policy=full`. This prevents “readonly-ish” UI runs from still having arbitrary
  host command execution.
- `agentd` no longer enforces a runtime filesystem sandbox (`tools_root`) for host tools. For multi-tenant isolation,
  run separate daemon instances (e.g. separate containers).

Implementation notes:

- The policy should be applied at the **tool registry** layer (omit disabled tool schemas) to prevent models from discovering
  tools they are not allowed to call.
- The executor should still reject disabled tool names (defense in depth) in case a provider returns tool calls that are not
  present in the tool schema set.

## HTTP CORS Policy (Daemon)

Problem:
- The daemon serves a local Web UI and is often accessed cross-origin (e.g. UI dev server `http://localhost:5173` → daemon `http://127.0.0.1:8123`).
- CORS was previously hard-coded to `Access-Control-Allow-Origin: *` and a limited header allowlist, which is:
  - too permissive by default when binding to non-loopback; and
  - too restrictive for the UI’s `X-OpenRouter-Key` header.

Goals:
- Safe defaults that work for the local UI on loopback.
- No implicit “public API” behavior when binding to non-loopback.
- Consistent behavior across JSON endpoints and SSE endpoints, including `OPTIONS` preflight.
- Easy to reason about and test (centralized policy, minimal per-handler duplication).

Additional goals (for this milestone):
- Add cookie-based auth / `Access-Control-Allow-Credentials` support (explicitly opt-in).
- Support per-route origin policies and regex matching with a clear precedence model.

Policy:
- Default:
  - If daemon binds to loopback (`127.0.0.1`, `localhost`): enable permissive CORS (`*`) for developer ergonomics.
  - If daemon binds to a non-loopback host: CORS is disabled unless explicitly configured.
- Configuration:
  - `--cors-origin <origin|*>` (repeatable):
    - `*` enables `Access-Control-Allow-Origin: *`.
    - Otherwise, only the configured exact origins are allowed; when a request has an `Origin` header matching the allowlist,
      the daemon reflects that origin and adds `Vary: Origin`.
  - `--cors-allow-headers <csv>` defaults to at least: `Content-Type, Authorization, X-OpenRouter-Key`.
  - `--cors-allow-methods <csv>` defaults to at least: `GET, POST, DELETE, OPTIONS`.

Implementation shape:
- Introduce a `CorsConfig` and a single function that applies CORS headers given `HttpRequest` + config.
- Teach the HTTP server to dispatch `OPTIONS` requests to a configurable preflight handler so preflight responses can include
  policy-derived headers (including dynamic origin reflection).

## Daemon Endpoint Structure (Hygiene)

As `agentd` grows, keep `daemon/src/main.cpp` focused on:
- process/flag initialization
- wiring configs (CORS/sandbox/provider defaults)
- HTTP route registration

Endpoint handler logic should live in dedicated modules under `daemon/src/` to keep responsibilities small and testable:
- `*_endpoint.{h,cpp}` for JSON endpoints (`/api/v1/config`, `/api/v1/tools`, `/api/v1/file`, etc.)
- `job_stream_endpoint.{h,cpp}` for SSE endpoints
- shared helpers (auth, sandbox policy) in single-purpose headers

## Data Model

### Message

- `role`: one of `system`, `user`, `assistant`, `tool` (extensible).
- `content`: UTF-8 text (first milestone).
- `name` (optional): for tool/function naming in compatible formats.

### Session

- Ordered list of messages.
- “Pinned” system messages supported by convention (first messages).

## Compaction (First Milestone)

### Trigger

- When `estimated_chars(messages) > max_chars`, compact.

### Action (default)

- Preserve:
  - All leading `system` messages (pinned) OR at minimum the first system message.
  - The last `K` messages (configurable).
- Drop older non-pinned messages until the budget is satisfied.

### Tool-loop compaction (host-side, day-1)

The tool-call loop uses the same char-budget idea, but performs compaction at the **request assembly** layer:

- Keep pinned leading `system` messages.
- Keep the last `K` messages.
- Drop the middle window when over budget.
- Optionally insert a deterministic `system` “compaction summary” message describing what was dropped (no extra LLM call).

This mirrors the approach in `ref/ds-cli` (Sophon) where compaction summaries are lightweight and do not require a second model call.

Note: `ref/` contains vendored upstream snapshots used for reference. Treat them as read-only here and follow
`docs/VENDORED.md` for the policy and guardrails.

### Per-request context hygiene (host-side)

Compaction only triggers when the history exceeds a threshold. In practice, **tool outputs** can bloat the context quickly even
before compaction triggers (e.g. repeated `rg`/`cat` output, large diffs, verbose build logs).

Therefore the host tool loop applies additional per-request guardrails:
- Tool results are **capped** before being appended into the next LLM request context (still preserving JSON envelope shape when possible).
- Full tool outputs remain available in the daemon/UI event log and audit trail, but the model only sees a bounded excerpt.

### “Session rotation” when context runs out

Because most providers are stateless, “spawning a new provider session” is implemented as **session rotation**:

- When the assembled request exceeds the configured context budget (or the provider rejects it as too large),
  the agent compacts more aggressively and retries.
- This produces a new “epoch” of context: pinned system prefix + one compaction-summary + last `K` messages.
- The tool-loop emits `compaction` events with an `epoch` counter so UIs can display when rotations happen.

Host implementation notes (day-1):
- The OpenAI-compatible host provider adapter treats “context too long” as a best-effort heuristic
  (`openai_is_context_too_long_error(...)`) based on HTTP status (e.g. `413`) and error message substrings
  (“maximum context”, “too many tokens”, etc.), and maps it to the stable core status `AGENT_ERR_CONTEXT_TOO_LONG`.
  This lets hosts (CLI/daemon) implement “session rotation” retries without duplicating provider-specific string matching.
- For `tools="none"` runs (both non-streaming and `stream_assistant=true`), the host applies the same idea:
  on a context-too-long rejection it retries up to 2 times, each time reducing `max_chars` (≈ 3/4) and compacting again.
  This is functionally equivalent to “spawning a new provider session” on stateless APIs.

Note on provider caching:
- DeepSeek supports automatic “context caching” for repeated request **prefixes** (KV-cache on disk).
- Our compaction policy preserves pinned `system` prefixes and (after a compaction event) a stable summary prefix,
  so subsequent turns can still benefit from prefix cache hits.

### Event payload size limits (UI stability)

Some event fields (especially `llm_request.request_json` and `llm_response.response_body`) can become extremely large
when verbose tracing is enabled. To keep the Web UI responsive:
- Event payloads are **bounded** (truncated with a `*_truncated` flag) for UI transport.
- Full fidelity request/response bodies remain available in the per-run `trace_text` transcript.
- The Web UI keeps the **last completed** run visible while an async job is running, and transient fetch/SSE errors
  do not clear the conversation (reduces the perception of “hangs”).

### Optional summary insertion (future-friendly)

- The host may generate a summary (possibly via an LLM call) and insert it as a `system` message:
  - `system`: "Session summary: …"
- Core supports “insert summary then prune more aggressively”.

## API Shape (C-friendly, stable ABI)

- Core exposes a C header (`core/include/agent/agent.h`) usable from C/C++.
- Core avoids C++ types across the boundary.

## Build / Packaging

- CMake-based project:
  - `agent_core` as a static library.
  - `agent` as a CLI executable.
- CLI links libcurl; JSON parsing in the CLI only (core remains JSON-agnostic).

## Milestone 1 Deliverables

- Minimal `agent` CLI:
  - One-shot prompt (`agent run "..."`)
  - Optional session (`--session <id>`) with persistence
  - Optional compaction (`--max-chars`, `--keep-last`)
  - OpenAI-compatible chat completion call (`/v1/chat/completions`)
- `agent_core` session + compaction APIs with unit/smoke tests.

## Tools (Host vs Embedded)

### Core tool contracts

- Core stores tool definitions in a portable registry (`name`, `description`, `parameters_json`).
- Tool execution is a host callback: `execute(ctx, tool_name, arguments_json) -> result_string`.
- Core does not assume tools correspond to shell commands; tools can represent:
  - CLI/daemon: filesystem, subprocess execution, network requests, code search, etc.
  - Embedded: GPIO output, I2C/SPI transactions, camera capture, audio playback, BLE scan, etc.

## Core Tool-Loop (Milestone 2)

Goal: move the **tool-call loop** (call LLM → parse tool calls → execute tools → feed tool results → repeat) into
the **portable core** so daemon/embedded targets can reuse the same control-flow and compaction policy.

### Goals

- **Core-owned loop control-flow**: retries, step limits, and compaction are consistent across hosts.
- **Core remains transport-agnostic**: no HTTP, no libcurl, no filesystem assumptions.
- **Core remains JSON-agnostic**:
  - It does not *parse* provider-specific JSON.
  - Tool arguments and tool results are treated as opaque UTF-8 strings (often JSON envelopes).
- **Host/provider adapters stay responsible for protocol shapes**:
  - OpenAI-compatible JSON request/response formatting/parsing stays in the host adapter.
  - Embedded providers can implement other formats while still using the same core loop.

### Additional goals (for the first cut)

- Full multimodal tool-loop content parts (text+image/audio/video) in the loop transcript, with consistent encoding.
- A stable long-term event schema across UI versions, with explicit versioning and migration notes.

### Architecture

Core introduces:

- `agent_chat_message_view_t`: a richer message view used by tool providers that can represent:
  - normal text messages (`role`, `content`)
  - assistant messages that include `tool_calls[]`
  - tool result messages that include `tool_call_id`
  - optional `name` (used for non-pinned compaction summary markers)
- `agent_tool_provider_t`: a host-supplied callback interface:
  - input: message transcript views + tool registry
  - output: assistant message (`content` + optional `tool_calls[]`)
  - providers can return `AGENT_ERR_CONTEXT_TOO_LONG` to request a “rotation” retry.
- `agent_tool_loop_run(...)`: the portable loop engine:
  - applies char-budget compaction (with deterministic summary insertion)
  - calls the provider
  - executes tools via `agent_tool_executor_t`
  - caps tool outputs before appending them back into the transcript
  - retries on context-too-long errors (host/provider signals via status)

Host adapters (CLI/daemon) provide:

- An OpenAI-compatible tool provider implemented with JSONCPP:
  - builds `/chat/completions` request JSON (`tools`, `tool_choice`, tool call/result message shapes)
  - parses tool calls from the response and returns them as structured tool calls
- Optional helpers for “tool output capping” and “tool output summary” that are JSON-aware (UI-friendly),
  while the core only treats tool outputs as strings.

### Host toolset (CLI/daemon)

The CLI/daemon toolset is designed around **OS-native tooling** plus **bounded filesystem inspection**:
- For **inspection** (read/list/stat), prefer bounded filesystem tools:
  - `fs_stat`: metadata (exists/type/size/mtime/ctime and binary hint; `birthtime` on platforms that support it); optional bounded line counting for small text files.
  - `fs_list`: bounded directory listing (supports recursion with depth/entry caps; supports `exclude_globs` to skip noisy paths; supports `respect_gitignore` best-effort).
  - `fs_find`: bounded file discovery (find files/dirs with depth/result caps; supports extension filters, `exclude_globs`, and `respect_gitignore` best-effort).
  - `fs_read`: bounded text reads with line-based pagination (`start_line`, `max_lines`, optional `end_line`) plus file timestamps.
  These exist primarily to control token usage and avoid “cat the world” context blow-ups.
- For **search**, use `text_search` (bounded) instead of `grep -R` when possible; optionally restrict by `extensions` to reduce scanning.
- For everything else, use `proc_exec` / `shell_exec` to run system-installed binaries for project inspection and file operations
  (`rg`, `find`, `git`, language toolchains, etc.).
- Use a dedicated diff-based editing tool (`file_apply_patch`) so file edits are auditable in the transcript
  (the tool result includes the unified diff that was applied).

Host default system hint (policy):
- When using the host toolset, host apps inject a one-time `system` message into an empty session to encourage
  **incremental inspection** (e.g. `rg/grep`, `head`, `tail`, `awk`, `sed -n`, and `fs_read` paging) instead of reading entire files.
- This is host policy only (not a core concept) and can be disabled/overridden by the host or client.

CLI-only: `--tools-root` (when set) is primarily used to control the working directory for `file_apply_patch`
(via `git -C <root> apply ...`). It is not intended as a strong security boundary (since `proc_exec`
can still invoke arbitrary commands in a YOLO host configuration).

Daemon note: `agentd` does not expose `tools_root` as an API parameter; session isolation is expected to be container/process-level.

Tool transcript persistence (host-only, day-1 pragmatic choice):
- The portable core session model is intentionally minimal and does not yet represent OpenAI tool-call metadata
  like `tool_call_id` and structured `tool` role messages.
- For host apps, the correct place to store full tool timelines is a **per-session audit log** (JSONL):
  - CLI writes per-run audit records to `~/.agent/sessions/<session>.events.jsonl`
  - `agentd` stores canonical state in SQLite (`docs/DB.md`) and exposes structured endpoints for history/replay.
  - Each record includes the prompt, final assistant text, and the structured `events` timeline (tool calls/results, LLM I/O).
- Session **messages** are kept clean (user/assistant content only), so future turns are not polluted by verbose tool output.
  This reduces token usage and avoids context blowups when switching between `tools=host` and `tools=none`.

### Tool success semantics (important)

Tool success is not solely a return/exit code:
- For host tools like `shell_exec`, a non-zero exit code can still produce useful output (or partial progress),
  while exit code 0 may still contain warnings/errors that matter (e.g. `pip install` logs).
- For embedded tools like GPIO/I2C, “success” may depend on returned sensor readings or device state.

Therefore tools should return **structured results** (recommended JSON envelope):
- `ok` (boolean) is a hint only
- `error` (string, optional) describes execution-level failure
- `data` (object) contains the raw tool output (stdout/stderr text, state fields, measurements, etc.)

The LLM (and higher-level agent policy) should judge success based on the returned `data`.

## Local RPC / API (Daemon ↔ UI)

Day-1 RPC goals:
- Simple, debuggable, OpenAPI-friendly.
- Works for local Web UI and future remote/broker bridging.

Decision:
- Use **HTTP + JSON** locally (bind to `127.0.0.1` by default).
- Version endpoints under `/api/v1/...`.

Initial endpoints (implemented in `agentd`):
- `GET /api/v1/health` → `{ ok, service, version }`
- `GET /api/v1/tools` → returns the active tool registry (`name`, `description`, `parameters_json`) for the requested toolset.
- `GET /api/v1/openrouter/models` → fetches + filters OpenRouter’s model catalog (for picking a cheap verification model).
- `POST /api/v1/run` → runs one user prompt against an LLM backend with optional tool loop.
- `POST /api/v1/run_async` → starts a background run and returns `{ ok, job_id }` (UI polls job status).
- `GET /api/v1/job?job_id=...` → returns `{ ok, status, result? }` for async runs.
  - Progress polling for UIs:
    - `include_events=1` to include tool/LLM events captured so far (best-effort).
    - `cursor=<n>&max_events=<m>` to tail events incrementally (cursor-based pagination).
- `GET /api/v1/job/stream?job_id=...&cursor=...` → streams events as Server-Sent Events (SSE) for responsive UIs.
- `GET /api/v1/file?path=...&yolo=0|1` → returns a file for UI preview (images/audio/video/text; size capped).
- `GET /api/v1/sessions` → list session ids.
- `POST /api/v1/session/new` → create a new unique session id (multi-client safe).
- `GET /api/v1/session?session_id=...` → get session messages.
- `DELETE /api/v1/session?session_id=...` → delete a session (messages + audit log).
- `GET /api/v1/session/audit?session_id=...` → fetch recent per-run audit entries (JSONL parsed to array).

Job lifecycle (daemon robustness):
- Async jobs (`run_async`) maintain an in-memory `JobState` with a bounded event ring buffer for UI progress.
- To keep the daemon safe for long-running usage, finished jobs are garbage-collected:
  - keep only a bounded number of recent jobs
  - drop jobs that have been done/error for longer than a TTL
  - cancellation flags are cooperative and checked at safe boundaries

Daemon authentication (optional, recommended when exposed beyond localhost):
- By default `agentd` binds to `127.0.0.1` and does not require auth.
- If `--auth-token` (or env `AGENTD_AUTH_TOKEN`) is set, daemon endpoints require:
  - `Authorization: Bearer <token>`
- This token is *daemon control-plane auth* and is distinct from provider API keys.
  - Provider keys are passed via request JSON (e.g. `api_key`) or via provider-specific headers (e.g. `X-OpenRouter-Key`).

`POST /api/v1/run` request (JSON):
- `prompt` (string, required)
- `session_id` (string, default `"default"`)
- `no_session` (bool, default `false`)
- `model`, `base_url`, `api_key` (optional overrides; if omitted, daemon uses env/config)
- `timeout_ms` (number; optional; provider HTTP timeout for this run)
- `stream_assistant` (bool; optional; request OpenAI-compatible SSE streaming (`stream: true`) and emit incremental `assistant_delta` events)
- `tools` (`"host"|"basic"|"none"`, default `"host"`)
- `max_steps` (number; `0` means unlimited)
- `trace` (bool; include transcript text)
- `yolo` (bool; if true, enables process execution tools; if false, exec tools are omitted)
- `verbose` (bool; if true, captures full tool outputs and raw request/response bodies into `events`)

Response (JSON):
- `ok` (bool)
- `assistant_text` (string)
- `trace_text` (string; full transcript between daemon↔LLM↔tools)
- `http_status`, `http_body` (best-effort diagnostics)
- `error` (best-effort message)
- `effective_yolo` (bool) so clients can display whether process execution tools were enabled.
- `effective_timeout_ms` (number) so clients can display the provider timeout that actually applied.
- `effective_stream_assistant` (bool) so clients can display whether assistant streaming was requested.
- `events` (array; structured event log for UIs)

Note on “thinking process”:
- The UI can display **tool calls/results and request/response transcripts**.
- We do **not** expose hidden chain-of-thought. If a model includes reasoning in the visible assistant message, it will be displayed as part of the assistant content.

Event log sizing:
- Per-event large fields are truncated (best-effort).
- The overall event log has a maximum event count and capture budget; if exceeded the final `end` event includes `truncated=true`.

Assistant streaming (provider-dependent):
- When `stream_assistant=true`, the daemon requests OpenAI-compatible SSE streaming (`stream: true`) and emits incremental
  `assistant_delta` events while the request is in-flight.
- For `tools="none"`, this provides streaming tokens for the assistant message.
- For tool-calling loops (`tools="basic"`/`"host"`), the host tool-provider reconstructs tool calls incrementally from streaming
  `delta.tool_calls` (best-effort; also supports legacy `delta.function_call`) and may also emit `assistant_delta` events on the
  final assistant step (provider-dependent).
  - Goal for the first cut: full coverage of every streaming variant across providers (with fallback when some ignore `stream: true`).
  - `assistant_delta` event payloads include:
    - `delta` (string): incremental assistant text
    - `step` (number): tool-loop step (0 for `tools="none"`)
    - `epoch` (number): context-rotation epoch / streaming attempt

Security notes (future):
- Binding to `127.0.0.1` avoids LAN exposure by default.
- Once broker/remote control is added, the daemon must implement an auth story (device provisioning, rotating tokens).

## Roadmap & Future Work (Next-gen)

This section consolidates the former `docs/NEXT_GEN.md` into this document. It captures future-proofing work for the full
stack:
- `agent_core` (portable core library)
- `agentd` (daemon + durable workflows)
- broker (secure relay / NAT traversal)
- WebUI (primary UX surface)

It is grounded in **current repo facts** (documented below) and proposes **design + implementation work** to make the
system more advanced, solid, efficient, and long-lived.

---

### Baseline facts (from this repo today)

#### agent_core
- C API with no environment-variable dependency; host handles env/flags.
- Session model + character-budget compaction (portable, tokenizer-free).
- Optional persistence interface (`agent/persist.h`) and portable `.sess` codec.
- Tool registry + host callback execution interface.

#### agentd
- Built as `agentd_lib` + thin `agentd` executable (optional embedded use).
- HTTP + SSE endpoints for runs, jobs, workflows, traces, diagnostics, etc.
- Durable workflows: DAG scheduling, retries/backoff, fairness, admission control, and restart recovery.
- Tool plugins (`--tool-plugin`) and out-of-process tool servers (`--tool-server-cmd`).
- SQLite-backed durable state (sessions, workflows, traces, memory, edge interop).
- Config store and runtime update endpoints (`/api/v1/config`, `/api/v1/config/update`).

#### broker
- Public control plane with OIDC/JWT for clients and mTLS for agent connectors.
- WebSocket connector (`/v1/agent/connect`) + HTTP proxy + SSE relay.
- Postgres-backed registry + membership audit.

#### WebUI
- React + Vite UI consuming agentd HTTP + SSE.
- Diagnostics panel, broker console, run settings, trace lookup.
- Playwright E2E tests (agentd host + broker flows).

---

### Target properties (future-proofing goals)

1) **Determinism & replayability**
   - Any run/workflow should be reproducible from its audit trail.
   - Replay should be possible without special provider state.

2) **Explicit contracts & versioning**
   - Every interop surface (agentd ↔ WebUI, broker ↔ connector, agentd ↔ agentd) should negotiate versions + capabilities.
   - Schema validation should be first-class and enforced in CI.

3) **Transport independence**
   - HTTP/SSE remains default, but agentd endpoints should be transport-agnostic by design.
   - Broker should be able to relay new transports without changing agentd handlers.

4) **Operational safety under load**
   - Strong backpressure, fairness, and admission control across the stack.
   - Clear, deterministic limits for tool loops, payload sizes, and long-running streams.

5) **Security with minimal friction**
   - Zero-trust posture for any non-local usage (auth, audit, safe defaults).
   - Non-interactive credential loading, explicit origin controls, and reliable policy enforcement.

6) **Composable intelligence**
   - Deterministic task graph + policy VM + tool servers = layered autonomy.
   - Provide a stable foundation for orchestration, verification, and collaboration.

7) **End-to-end security + identity**
   - First-class PKI/provisioning for nodes and connectors (not just hooks).
   - Signed capability manifests and run attestations with verifiable hashes.

8) **Real-time media support**
   - Audio streaming as a native workflow primitive (Opus/WebRTC transport).
   - Low-latency bidirectional media for “talk + tool” flows.

9) **Decentralized coordination**
   - Optional node consensus so clusters can negotiate locally when the platform is absent.
   - Explicit conflict resolution and safety policies for multi-node decisions.

---

### Promoted goals (explicit)

This project does **not** treat any “formerly deferred” items as non-goals. The following areas are **explicit goals**
tracked in `TODOS.md` as promoted workstreams:

- Tool loop: full multimodal transcript support + stable, versioned event schema with migrations.
- Streaming: core-layer streaming interface + provider compatibility matrix with full variant coverage.
- Audio streaming: Opus/WebRTC voice pipeline + broker relay + UI voice session controls.
- Tool plugins: sandbox/isolation, Windows loader, and embedded/MCU-compatible plugin path.
- Storage/analytics: DB query API as canonical surface, analytics layer, and binary blob storage tiers
  (see `docs/DB.md#blob-storage-tiers-design--status`).
- UI actions: stable public action API, autoplay unlock flow, and consented remote URL opens.
- Memory: observation capture + progressive disclosure retrieval flow (search → timeline → get).
- Interop/attestation: PKI provisioning + signed manifests/attestations + canonical JSON hashing + envelope confidentiality.
- AVM: scoped flag passthrough, host-effects policy, record/replay plumbing, and quorum/attestation.
- Node consensus: decentralized coordination protocol with conflict resolution + deterministic simulation tests.

---

### Architecture proposals (grounded, phased)

#### 1) agent_core (portable core)

**1.1 ABI-stable C API v1**
- Define an explicit ABI surface with versioned structs and sizes.
- Add `agent_core_version()` + `agent_core_caps()` to expose compiled features.

**1.2 Deterministic serialization boundary**
- Provide canonical JSON/CBOR encoding for session snapshots and tool call records.
- Use the existing `agent_json_c14n` and CBOR helpers to ensure stable hashing.

**1.3 Memory discipline for embedded**
- Optional arena allocator + bounded growth for session storage.
- Provide a compile-time `AGENT_CORE_LIMITS` block for MCU targets.

**1.4 Cross-runtime tool schema linting**
- Add a core helper to validate tool schema JSON (basic structural checks) before registering.

#### 2) agentd (daemon + workflows)

**2.1 Protocol capability negotiation**
- Add `GET /api/v1/caps` with:
  - protocol versions, feature flags, max limits, and enabled tool modes.
- WebUI and brokers should **fail fast** if required caps are missing.

**2.2 Replay-grade audit trail**
- Standardize a single event schema for run + workflow events.
- Add “replay bundles” (inputs + deterministic hashes) so a run can be re-executed.

**2.3 Deterministic tool-loop envelopes**
- Persist tool call inputs/outputs with stable hashing and bounded truncation metadata.
- Make replay validation part of CI for a small deterministic fixture set.

**2.4 Unified policy VM hook**
- Expose a deterministic pre/post hook interface (policy VM or rules engine) for:
  - tool allow/deny, run shaping, budget gating, retry policies.
- Keep it optional and sandboxed (out-of-process or limited VM).

**2.5 Performance & resiliency**
- Adopt a multi-queue scheduler that can separate “interactive” vs “batch” workloads.
- Add a persistent “work queue watermark” to enforce submission backpressure.

#### 3) broker (control plane + relay)

**3.1 Durable relay envelopes**
- Store in-flight relay metadata (request id, trace id, status) for audit/replay.
- Idempotency keys for proxy/orchestrate to make retries safe (implemented).

**3.2 Multi-transport relay layer**
- Keep WebSocket but add a transport-agnostic relay interface so new transports
  (e.g., gRPC or WebTransport) can be introduced without changing business logic.

**3.3 Broker-side policy hooks**
- Central policy checks (rate limits, allowlists, cost ceilings) before relay.
- Provide a policy audit stream (SSE) for operator visibility.

**3.4 Security & provisioning (broker + connector)**
- Establish a device provisioning flow: issue/rotate device certs, bind to `agent_id`, and record provenance.
- Store signed capability manifests (and hashes) so clients can verify device identity and capabilities.
- Add revocation lists and short-lived leaf cert rotation for compromised nodes.

**3.5 Media relay (broker)**
- Provide a media relay mode for Opus/WebRTC signaling that can live alongside the HTTP relay.
- Support TURN-like relay fallback for NAT traversal (initially minimal, expand later).

#### 4) WebUI (primary UX)

**4.1 Capability-aware UI**
- The UI should read `/api/v1/caps` and hide/disable unsupported features.

**4.2 Trace-first UX**
- Treat traces as the primary timeline; sessions are a lens, not the source of truth.
- Provide deterministic “replay from trace” workflows for debugging.

**4.3 Offline + state continuity**
- Persist client settings and last-known caps, and survive daemon restarts cleanly.

**4.4 Media-first UX**
- Add a minimal voice panel with streaming mic capture + playback.
- Show media session state in the trace timeline for audit/replay.

---

### Cross-cutting infrastructure

1) **Schema registry + CI checks**
   - Maintain versioned JSON Schemas for all public API payloads.
   - Enforce in CI (as already done for existing specs).

2) **Evidence & attestation**
   - Extend evidence bundles with cryptographic signatures.
   - Support “prove this run” artifacts that can be verified offline.

3) **Formal limits**
   - Standardize a single “limits” document generated from defaults (agentd + broker + core).
   - Keep docs + runtime consistent via tests.

4) **PKI + attestation**
   - Define a root CA + device cert model (mTLS + signed manifests).
   - Include signed evidence bundles and public verification tooling.

5) **Consensus protocol**
   - Specify a minimal consensus/coordination protocol for node clusters (leader election + task locks).
   - Provide a deterministic simulation harness for testing conflict scenarios.

---

### Phased roadmap (high leverage)

**Phase A: Contract foundation (short-term)**
- Implement `/api/v1/caps` and a common caps schema (agentd + WebUI + broker proxy).
- Create a single event schema for run + workflow events; add schema tests.
- Add an idempotency key to broker proxy/orchestrate requests (implemented).

**Phase B: Deterministic replay (mid-term)**
- Store replay bundles for deterministic runs (inputs + hashes + tool outputs).
- Add replay validation tests for a minimal fixture set.

**Phase C: Multi-transport readiness (mid-term)**
- Define a transport interface for broker + connector.
- Build at least one alternative transport in a feature flag (research prototype).

**Phase D: Policy VM integration (long-term)**
- Document policy VM hook interfaces + execution limits.
- Provide a stub policy runner + deterministic test fixtures.

---

### Related docs

- `docs/WORKFLOWS.md` (durable workflow semantics)
- `docs/BROKER.md` (broker protocols and auth model)
- `docs/PROTOCOL.md` (agentd ↔ WebUI protocol)
- `docs/DIAGNOSTICS.md` (health + provider smoke tests)
---

# Source: docs/AGENTD_LIB.md

# Embedding `agentd` as a Library (Sidecar Mode)

Design context: see `DESIGN.md` for the system-wide architecture; this document focuses on embedding and extension details.

This repo now builds the daemon as:

- `agentd_lib` (static library): the full agentd implementation
- `agentd` (executable): a thin CLI entrypoint that links `agentd_lib`

This enables desktop/server apps to embed agentd **in-process** and optionally inject additional tools.

## Build

`agentd_lib` is available when host components are enabled (`AGENT_BUILD_HOST=ON`).

```sh
cmake -S . -B build
cmake --build build -j
```

Targets:
- `agentd_lib`
- `agentd` (only when `-DAGENTD_ENABLE_HTTP=ON`)

### Disable the built-in HTTP server

For app embedding where you don't want to bind a local port (e.g., you use a cloud relay/broker or a custom transport),
build without the socket-based HTTP server:

```sh
cmake -S . -B build-nohttp -DAGENTD_ENABLE_HTTP=OFF
cmake --build build-nohttp -j
```

## Primary API: `agentd::AgentdService`

Header:

- `daemon/include/agentd/service.h`

Note: `AgentdService` is only available when `AGENTD_ENABLE_HTTP=ON`.

The service owns:
- SQLite DB (`AgentDb`)
- runtime config store (`DaemonConfigStore`)
- HTTP server (`HttpServer`)

### Lifecycle

- `init(out_error)`:
  - fills best-effort env defaults (base_url/api_key/model/db_path/state dirs)
  - ensures `db_path` defaults to `./agentd.db` if empty
  - opens DB and loads runtime config from DB
  - registers HTTP routes

- `serve_blocking(out_error)`:
  - blocks in the accept loop until `stop()` is called

- `start_background(out_error)`:
  - starts `serve_blocking()` on an internal thread

- `stop()`:
  - stops the HTTP server and joins the background thread (if any)

### Minimal embedding example

```cpp
#include "agentd/service.h"

int main() {
  agentd::DaemonConfig cfg;
  cfg.listen_host = "127.0.0.1";
  cfg.listen_port = 8123;
  cfg.db_path = "./agentd.db";
  cfg.cors_disabled = true; // typical for non-browser embedding

  agentd::AgentdService svc(agentd::AgentdService::Options{cfg});
  std::string err;
  if (!svc.start_background(&err)) {
    // handle error
    return 1;
  }

  // ... host app runs ...

  svc.stop();
  return 0;
}
```

## Tool extension API

`agentd` supports an optional `ToolExtension` injection point:

- `ToolExtension::register_tools(ctx, registry)` is called after the base toolset (`basic` or `host`) is created.
- Tools added by the extension are dispatched to `ToolExtension::execute_tool(...)`.

Contract:
- `register_tools` should **only append tools** to the registry.
- `execute_tool` must handle the tools added by `register_tools`.
- The extension callbacks must remain valid for the service lifetime.

### Example: add a device-specific tool

```cpp
static agent_status_t register_my_tools(void*, agent_tool_registry_t* reg) {
  return agent_tool_registry_add(
    reg,
    "device_beep",
    "Play a short beep on the host device",
    "{\"type\":\"object\",\"properties\":{\"ms\":{\"type\":\"integer\"}},\"required\":[\"ms\"]}"
  );
}

static agent_status_t exec_my_tool(void*, const char*, const char*, agent_string_t* out) {
  const char* ok = "{\"ok\":true}";
  return agent_string_set_copy(out, ok, strlen(ok));
}

agentd::ToolExtension ext;
ext.ctx = nullptr;
ext.register_tools = register_my_tools;
ext.execute_tool = exec_my_tool;

agentd::AgentdService::Options opt;
opt.cfg = cfg;
opt.enable_tool_extension = true;
opt.tool_extension = ext;

agentd::AgentdService svc(opt);
```

## Notes / caveats

- The HTTP server is intentionally minimal (no TLS, no chunked encoding). For production remote exposure, put a reverse proxy in front or extend `HttpServer`.
- `stop()` now closes the listening socket to ensure `serve()` unblocks promptly (important for embedding and tests).
- Tool execution semantics are still controlled by `tools=host|basic|none` and safety knobs (`yolo`, `host_policy`) at runtime.

## Transport-agnostic API (`AgentdApi`)

If you want to drive agentd over a non-HTTP transport (MQTT, cloud relay, custom IPC), you can avoid the socket server
and call the daemon endpoints directly through `AgentdApi`:

- `daemon/include/agentd/api.h`

It accepts/returns `HttpRequest`/`HttpResponse` objects (`daemon/include/agentd/http_types.h`), which are intentionally
“HTTP-shaped” so a transport can map its messages onto the same endpoint handlers without rewriting the daemon logic.

## Standalone tool plugins (`--tool-plugin`)

If you are using the standalone `agentd` executable (not embedding), you can still add tools without recompiling by loading
tool plugins at runtime:

```sh
./build/agentd \
  --tool-plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) \
  --tool-plugin-config '{"tag":"demo"}'
```

See: `docs/TOOLS.md`.

For process isolation, run plugins via the tool server host instead:

```sh
./build/agentd \
  --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"
```
---

# Source: docs/BROKER.md

# Cloud Broker (Secure Relay) — Design

Design context: see `DESIGN.md` for the system-wide architecture; this document focuses on broker-specific design and APIs.

This document defines a **cloud broker** for securely relaying requests between:

- multiple **agentd deployments** (desktop/server “workers” behind NAT, or in private networks)
- multiple **client types** (WebUI, mobile, backend services, CLI)

The broker provides:

1) **Mutual registry**: agents connect outbound and become addressable by `agent_id`
2) **Client authentication + authorization**: OIDC/JWT for users + DB-backed agent membership checks
3) **Management API**: list/create/delete agents, inspect status/metadata, disconnect
4) **Secure relay API**: clients send requests to a specific agentd via broker, without directly reaching the agent network
5) **SSE support**: broker can proxy streaming endpoints (SSE) without fragile “file path” hacks

## Why a broker (vs exposing agentd HTTP directly)

Directly exposing `agentd` is hard to secure and operate at scale:

- many agents are behind NAT / mobile networks
- many clients are browsers (no raw TCP, limited mTLS)
- you want centralized authZ, audit, rate limits, revocation
- you may want to coordinate **multiple agents** for one user task

The broker model reverses the connectivity:

- agents open **outbound** long-lived connections to the broker
- clients connect to the broker and request actions against agents by id

## High-level architecture

**Control plane**
- Broker HTTPS server (public)
- Client API (OIDC/JWT bearer tokens)
- Agent management API (per-user + admin)
- Postgres for durable state/audit

**Data plane**
- Agent → Broker: `wss://` outbound websocket with **mTLS**
- Client → Broker: HTTPS API calls (OIDC bearer token)
- Broker routes messages to the correct connected agent

## Identity / security model

### Agent identity

Agents authenticate with **mTLS**.

Convention (same idea as the urine-monitor hub platform):
- Agent identity is encoded in the client certificate **CN**
- Example CN: `agentd-123`

Broker verifies:
- TLS stack verifies the chain against `--tls-client-ca`
- broker extracts CN and ensures it matches the agent’s declared `agent_id`

This prevents an agent from impersonating another agent id.

### Client identity

Clients authenticate with `Authorization: Bearer <jwt>` (OIDC ID token).

Broker authorizes each request using Postgres:
- user identity is the OIDC `sub` claim
- a user may access an agent if they have a row in `broker_agent_memberships`
- admin subs can be configured with `--admin-subs` (comma-separated `sub` values)

This supports multiple users and prevents “UI says it ran” when it did not:
all authorizations and audits are checked/recorded server-side.

Optional: cookie-based auth

For browser clients that cannot attach `Authorization` headers directly, the broker can accept a bearer token from a cookie:
- configure `--auth-cookie <name>` (or env `AGENTD_BROKER_AUTH_COOKIE`)
- the cookie value should be a raw JWT (or `Bearer <jwt>`)
- enable CORS credentials (`--cors-allow-credentials`) and **explicit origins** (not `*`)

Optional: static client tokens

For non-UI service clients (or environments without OIDC), the broker can accept static bearer tokens from a JSON file:
- `--client-auth-file /path/to/client_auth.json`
- `--client-auth-fallback` to allow tokens when OIDC auth fails
- `--client-auth-reload-ms` to periodically reload the file
- `--client-auth-strict` to fail readiness if reload fails
- `--client-auth-max-age-ms` to fail readiness if last reload is too old
- `--client-auth-event-include-error` to include reload error text in events
- Send `SIGHUP` to reload immediately

Client token auth is intended for **proxy/orchestrate** calls only. Agent list/create remains OIDC-only.

Example file: `broker/client_auth.example.json`.

## Protocol between broker and agent connector

Broker and agent communicate over a single websocket connection (JSON messages).

**Agent connects:**
1) Broker accepts TLS+mTLS websocket connection at `/v1/agent/connect`
2) Agent sends:
   - `{"type":"hello","agent_id":"123","meta":{...}}`
   - Optional `meta.deployment_id` to distinguish multiple deployments for the same `agent_id`
3) Broker replies:
   - `{"type":"hello_ack","ok":true,"agent_id":"123"}`

**Broker forwards a client request to a specific agent:**
- Broker → Agent:
  - `{"type":"http_request","id":"<uuid>","req":{"method":"POST","path":"/api/v1/run","query":"","headers":{...},"body_b64":"..."}}`
- Agent → Broker:
  - `{"type":"http_response","id":"<uuid>","resp":{"status":200,"headers":{...},"body_b64":"..."}}`

Notes:
- `body_b64` allows binary payloads (e.g. wav artifacts) without fragile “file path” hacks.
- Streaming responses (SSE / long responses) are supported with:
  - `http_stream_request` / `http_stream_start` / `http_stream_chunk` / `http_stream_end`
  - broker may send `http_stream_cancel` to stop a long-running stream if the client disconnects

### Transport interface (prep for multi-transport)

The broker now uses a transport-agnostic connector interface so new transports can be added without changing relay logic:

- Interface: `broker/internal/transport/conn.go`
- WebSocket adapter: `broker/internal/transport/websocket.go`

This keeps the business logic centered on `transport.Conn` while retaining today’s WebSocket connector path.

## Broker HTTP API

All endpoints below are served by the broker (not by agents).

### Registry / management

- `GET /v1/agents`
  - lists agents the authenticated user is allowed to access (from Postgres)
  - includes connection status (connected/last_seen) from in-memory registry
  - includes connected deployment list per agent when present

- `POST /v1/agents`
  - creates a new agent record owned by the authenticated user (also inserts membership as `owner`)
  - response includes `connector_hint_cn` (the CN you should use in an mTLS client cert)

- `GET /v1/agents/{agent_id}/members`
  - lists agent members (owner/admin only)
- `POST /v1/agents/{agent_id}/members`
  - add/update an agent member (owner/admin only)
- `DELETE /v1/agents/{agent_id}/members/{user_sub}`
  - remove an agent member (owner/admin only; cannot remove owner)
- `GET /v1/agents/{agent_id}/membership_audit`
  - membership audit trail for the agent (owner/admin only)
  - query: `limit` (optional, default `200`, max `500`)

- `POST /v1/agents/{agent_id}/delete` (or `DELETE` to the same path)
  - deletes an agent record (owner or admin)

- `POST /v1/agents/{agent_id}/disconnect`
  - disconnects an agent’s websocket (admin only)
- `GET /v1/agents/{agent_id}/deployments`
  - lists connected deployments for a given `agent_id`
  - includes `default_deployment_id` (the most recent connected deployment)

### Relay

- `GET/POST/... /v1/agents/{agent_id}/proxy/<agentd_path>`
  - transparent HTTP proxy to the target agent (path and query preserved)
  - auth: OIDC user must be in `broker_agent_memberships` for that agent
  - optional header `X-Agentd-Deployment: <deployment_id>` (or query `deployment_id`) to target a specific deployment
  - if no deployment is specified, broker defaults to the most recent connected deployment

- `GET /v1/agents/{agent_id}/proxy_sse/<agentd_path>`
  - streaming proxy intended for SSE endpoints
  - broker flushes chunks as they arrive from the agent connector
  - optional header `X-Agentd-Deployment: <deployment_id>` (or query `deployment_id`) to target a specific deployment

Idempotency (optional):
- send `Idempotency-Key` (or `X-Idempotency-Key`) to safely retry proxied requests
- if the same key is reused with a different request payload, the broker returns `409` (`idempotency_key_conflict`)
- if a request is already in progress for the key, the broker returns `409` (`idempotency_key_in_progress`)
- successful replays return `X-Idempotency-Replay: true`
- large responses are not stored; the broker responds with `X-Idempotency-Disabled: response_too_large`

### Audio signaling (WebRTC relay)

The broker provides a **signaling relay** for audio sessions (media flows directly between WebUI and agentd):

- `POST /v1/audio/sessions` (create a session; returns `session_id`)
- `POST /v1/audio/sessions/{session_id}/signal` (send offer/answer/candidate/bye)
- `GET  /v1/audio/sessions/{session_id}/signal/stream` (SSE stream of signaling events)

Sessions are in-memory and expire after a TTL (default 15 minutes). Configure via:
- `--audio-session-ttl` (duration)
- `AGENTD_BROKER_AUDIO_SESSION_TTL_MS` (milliseconds)

#### Using durable workflows through the broker proxy

The broker proxy can be used as a “virtual base URL” for agentd-to-agentd collaboration tasks:

- In durable workflows, set `agentd_call.base_url` to:
  - `https://<broker>/v1/agents/<agent_id>/proxy`
- Or set `agentd_call.broker_proxy:{broker_base_url,agent_id}` and omit `base_url` (server computes/persists the proxy prefix).
- The caller still uses normal agentd endpoints under the proxy prefix:
  - `POST .../proxy/api/v1/workflow/submit`
  - `GET  .../proxy/api/v1/workflow?workflow_id=...`
- Broker auth is typically an OIDC bearer token; in workflows, use `agentd_call.bearer_env` so the token value is not persisted.

#### Operational control (OTA, maintenance)

Operational endpoints can also be routed through the proxy:

- `POST /v1/agents/{agent_id}/proxy/api/v1/ota/update`
- `GET  /v1/agents/{agent_id}/proxy/api/v1/ota/status`

Use `X-Agentd-Deployment` to target a specific deployment, or omit it to target the broker’s most recent deployment.
`/api/v1/ota/status` surfaces drain hints (`drain_active`, `drain_until_unix_ms`, `drain_reason`) during updates.

Broker bulk OTA fan-out (multi-deployment):

- `POST /v1/agents/{agent_id}/ota/update`
  - body: `{ url, sha256?, version?, reason?, trace_id?, drain_timeout_ms?, deployment_ids?, deployment_id?, deployments? }`
  - if deployment IDs are omitted, broker fans out to all connected deployments for the agent
- `GET /v1/agents/{agent_id}/ota/status`
  - query: `deployment_ids=dep1,dep2` (optional; defaults to all connected deployments)

Both return `{ ok, agent_id, total, ok_count, error_count, results[] }`, where each result is
`{ deployment_id, status, data }` with `data` mirroring the agent’s OTA endpoint JSON.

Broker bulk memory maintenance fan-out (multi-deployment):

- `POST /v1/agents/{agent_id}/memory/retention/enforce`
  - body: memory retention request + optional deployment ids
- `GET /v1/agents/{agent_id}/memory/recaps`
  - query: `limit`, `include_summary`, and optional `deployment_ids=dep1,dep2`
- `POST /v1/agents/{agent_id}/memory/recaps`
  - body: memory recap request + optional deployment ids
- `GET /v1/agents/{agent_id}/memory/salience`
  - query: salience policy params (`include_structured`, `include_daily`, `daily_days`, `max_*`, `half_life_days`, `importance_weight`)
    plus optional `deployment_ids=dep1,dep2`

These also return `{ ok, agent_id, total, ok_count, error_count, results[] }` with `{ deployment_id, status, data }`
per deployment. The `data` field mirrors the underlying agentd memory endpoint response.

### Orchestration (fan-out)

- `POST /v1/orchestrate`
  - broker-managed fan-out across multiple agents (OIDC required)
  - each task becomes an HTTP request to the target agent (defaults to `POST /api/v1/run`)
  - intended for multi-agent workflows where the client wants one aggregated response

Request (JSON):
- `tasks` (array, required): each task is:
  - `agent_id` (string, required)
  - `deployment_id` (string, optional): target a specific deployment for the agent
  - `task_id` (string, optional)
  - `request` (object, optional): agentd run request body (if omitted, remaining task keys are treated as the request body)
  - `headers` (object, optional): forwarded to agentd (broker auth headers are never forwarded)
  - `method` (string, optional, default `POST`)
  - `path` (string, optional, default `/api/v1/run`)
  - `query` (string, optional, default empty)
- `defaults` (object, optional): merged into each task request (missing keys only)
- `allow_sessions` (bool, optional, default `false`): when `false`, broker forces `no_session=true` and defaults `tools="none"` for safety
- `max_concurrency` (int, optional, default `4`, max `16`)
- `timeout_ms` (int, optional, default `60000`, max `300000`)

Response (JSON):
- `ok` (bool)
- `all_ok` (bool)
- `results` (array): list of `{ task_id, agent_id, ok, http_status?, ms, result?, error? }`

Idempotency (optional):
- send `Idempotency-Key` (or `X-Idempotency-Key`) to safely retry an orchestrate request
- `X-Idempotency-Replay: true` indicates a replayed response
- `409` responses indicate `idempotency_key_in_progress` or `idempotency_key_conflict`

### Broker events (SSE)

- `GET /v1/events`
  - server-sent events for the authenticated user
  - emits JSON `data:` payloads with types like `agent_connected`, `agent_disconnected`, `relay_audit`, `client_auth_reload`, `agent_member_updated`

### Client auth status (admin-only)

- `GET /v1/client_auth/status`
  - reports the last reload status/time for the client auth file
- `POST /v1/client_auth/reload`
  - triggers an immediate reload of the client auth file

### Trace correlation (debugging)

- `GET /v1/trace?trace_id=...`
  - returns broker relay audit rows for the trace id
  - returns persisted broker orchestrate summaries for the trace id (request/response JSON, redacted)
  - returns membership audit rows for the trace id (when membership updates include the same trace_id)
  - membership audit rows are limited to agents the caller can access (admins can see all)
  - best-effort: fans out to referenced agents to query their `GET /api/v1/trace?trace_id=...` endpoint (if supported)

## Multi-agent workflows

The broker supports multi-agent usage by design:
- multiple agents can be connected simultaneously
- client can choose which `agent_id` to send each request to

## WebUI broker console

The WebUI can operate in **broker mode** (OIDC) and now includes a broker console panel:
- list agents and select the active `agent_id`
- manage agent memberships (add/remove roles)
- view membership audit trail (per agent)

## CORS (browser clients)

The broker supports CORS for browser clients with **explicit opt-in**:

- `--cors-origin <origin>` (repeatable) or `--cors-origins <csv>`
  - accepts exact origins, `*`, or `re:<regex>`
- `--cors-allow-headers <csv>` (default includes Authorization + tracing + idempotency headers)
- `--cors-allow-methods <csv>` (default: GET, POST, PUT, PATCH, DELETE, OPTIONS)
- `--cors-allow-credentials` (enable cookie auth; requires explicit origins)
- `--cors-max-age-seconds <n>`

Per-route policies:
- `--cors-route '{"path_prefix":"/v1/agents","origins":["https://ui.example"],"allow_credentials":true}'`
- or env `AGENTD_BROKER_CORS_ROUTES='[{"path_prefix":"/v1/agents","origins":["https://ui.example"]}]'`
- precedence: **longest `path_prefix` match wins**
- origin match precedence: exact > regex > `*`

Configure in the WebUI Settings:
- Connection mode: `broker`
- Broker base URL + bearer token

This enables “complex tasks” split across specialized agents:
- one agent with full host tools on a workstation
- another agent in a GPU server environment
- another agent attached to lab equipment

Orchestration strategies (future work):
- broker-managed “task sessions” spanning multiple agents
- fan-out calls + gather results
- distributed tool call limits / budgets

## Threat model (MVP)

Assumptions:
- broker public endpoint is reachable by clients and agents
- agent networks are not directly reachable by clients
- trust anchor: broker CA for agent mTLS, plus broker client-token store

Mitigations:
- mTLS for agent channels (identity + encryption)
- DB-backed membership checks for which agents can be controlled
- audit logging in Postgres (`broker_relay_audit`)
- rate limiting (future)

## Local quickstart (dev)

1) Generate local mTLS test certs:

```sh
bash tools/gen_agentd_broker_mtls_test_certs.sh tools/_agentd_broker_mtls_test_certs 1
```

2) Ensure Postgres is reachable and set a DSN:

```sh
export AGENTD_BROKER_DB_DSN='postgres://...'
```

3) Start the broker:

```sh
cd broker
go run ./cmd/agentd-broker \
  --listen :8443 \
  --tls-cert ../tools/_agentd_broker_mtls_test_certs/server.pem \
  --tls-key  ../tools/_agentd_broker_mtls_test_certs/server.key.pem \
  --tls-client-ca ../tools/_agentd_broker_mtls_test_certs/ca.pem \
  --db-dsn "$AGENTD_BROKER_DB_DSN" \
  --oidc-issuer 'https://YOUR_ISSUER' \
  --oidc-audience 'YOUR_CLIENT_ID'
```

4) Register an agent via the broker management API (needs an OIDC bearer token):

```sh
curl -sS -H "Authorization: Bearer $OIDC_JWT" \
  -H 'Content-Type: application/json' \
  -d '{"agent_id":"1"}' \
  https://localhost:8443/v1/agents
```

5) Start `agentd` locally (bind loopback only), then start the connector:

```sh
./build/agentd --host 127.0.0.1 --port 8123
cd broker
go run ./cmd/agentd-connector \
  --broker wss://localhost:8443/v1/agent/connect \
  --local-agentd http://127.0.0.1:8123 \
  --tls-ca   ../tools/_agentd_broker_mtls_test_certs/ca.pem \
  --tls-cert ../tools/_agentd_broker_mtls_test_certs/client_1.pem \
  --tls-key  ../tools/_agentd_broker_mtls_test_certs/client_1.key.pem
```

6) Proxy a request through the broker:

```sh
curl -sS -H "Authorization: Bearer $OIDC_JWT" \
  https://localhost:8443/v1/agents/1/proxy/api/v1/health
```

7) Stream an SSE endpoint through the broker:

```sh
curl -N -H "Authorization: Bearer $OIDC_JWT" \
  https://localhost:8443/v1/agents/1/proxy_sse/api/v1/job/stream
```
---

# Source: docs/CLIENT.md

# Client Collaboration and API (Living)

Date: 2026-02-19

Design context: see `DESIGN.md` for system architecture; this document focuses on client-facing collaboration and APIs.

This document consolidates the client-facing design and API surfaces for `agentd`.
It replaces the previous split across:
- CLIENT_AGENTD_SPEC
- CLIENT_COLLAB
- CLIENT_ENTITIES
- CLIENT_PROFILES
- CLIENT_PROBE
- CLIENT_RPC
- CLIENT_STATE
- UI_ACTION
- UI_CLIENT_EVENTS
- UI_WAIT_EVENT

If you are implementing a new client (mobile app, Slack bot, CLI front-end, etc.),
this is the canonical reference.

## Scope

- Client identity and session safety
- Bidirectional collaboration (events, UI actions, client RPC)
- Durable Scene (entities) model
- Deterministic definition-of-done (DoD) handshakes
- Client-facing HTTP API endpoints

See also:
- docs/PROTOCOL.md for the broader run + artifact protocol
- docs/spec/run-events/run_events_v1.md for event schema envelopes
- docs/LIMITS.md for size and safety caps

## Concepts

- client: any UI/integration that drives the daemon (WebUI, Slack, mobile)
- session: namespace for message history, durable Scene, client events
- run: one LLM execution (sync or async)
- event: structured log record (artifacts, UI actions, client acks)
- Scene: a per-session, server-owned JSON object mapping entity_id -> entity

## Authentication and correlation

- If `agentd` starts without an auth token, requests are accepted without auth.
- If `agentd` is configured with `--auth-token`, clients must send:
  - Authorization: Bearer <token>
- Health endpoints are always unauthenticated:
  - GET /api/v1/health
  - GET /api/v1/ready
  - GET /metrics

Correlation headers:
- X-Request-Id: client-provided request id (echoed back by the daemon)
- X-Trace-Id: for /api/v1/run and /api/v1/run_async, if trace_id is omitted
  in JSON, the daemon uses the header value (must pass trace_id_is_safe)

## Session id safety

session_id is treated like a filename key and must pass session_id_is_safe:
- length 1..200
- characters: [A-Za-z0-9_.-]
- must not contain "/", "\\", or traversal segments ("." or "..")

Clients should treat invalid ids as a client-side bug and avoid retries.

## Client identity

Clients should send a client identity with events and run requests:

{
  "client": {
    "id": "webui",
    "kind": "webui",
    "instance_id": "tab-123"
  }
}

Fields:
- client.id: stable logical id (webui, slack, mobile, etc)
- client.kind: human-friendly kind (optional but recommended)
- client.instance_id: per-instance id (tab, device), used for debugging

All client identity fields are bounded (max 200 chars, no control chars).

## Event model

Client events are the append-only collaboration surface. They are used for:
- UI acknowledgements (artifact_rendered, ui_action_shown)
- client RPC results/progress
- client state snapshots

### Client event endpoint (client -> agentd)

POST /api/v1/session/client_event
POST /api/v1/session/ui_event  (legacy alias)

Request shape:

{
  "session_id": "sess-...",
  "type": "artifact_rendered",
  "ts_unix_ms": 1730000000000,
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": { "tool_call_id": "call_123", "path": "out/foo.wav" },
  "append_to_session": true
}

Notes:
- ts_unix_ms is optional (daemon will use "now")
- append_to_session default is true; when true, a synthetic user message is
  appended so the next run can see the event in session history

Persistence:
- DB mirror: client_events table (when --db-path is enabled)
- File-backed log (always): <session_id>.client_events.jsonl
  - rotated when large; canonical store is DB when enabled

Read-back helpers:
- GET /api/v1/session/client_events?session_id=...
- GET /api/v1/db/client_events?session_id=...&limit=...&offset=...
- GET /api/v1/session/clients?session_id=... (distinct clients observed)

### Suggested event types

- artifact_rendered
- ui_action_shown
- client_state
- client_capabilities
- client_rpc_result
- client_rpc_progress
- notification_shown
- notification_ack

## Agent -> client actions (ui_action)

The host tool `ui_action` lets the agent request UI-side actions.
It emits a ui_action event with structured payload.

Tool name: ui_action
Arguments:
- type (string, required)
- title (string, optional)
- message (string, optional)
- path (string, optional)
- mime (string, optional)

Allowlisted action types (initial v1):
- notify
- request_client_state
- client_rpc
- collab_rpc (alias for client_rpc)
- client_probe (legacy alias; prefer client_rpc)

### client_rpc action shape (v1)

{
  "type": "client_rpc",
  "title": "Optional short title",
  "rpc_id": "rpc_123",
  "rpc": {
    "kind": "dom_query",
    "args": { "selector": "#app", "fields": ["tag", "id"] }
  },
  "auto_run": true,
  "side_effects": false
}

Notes:
- rpc_id is the correlation id (recommended: tool_call_id)
- rpc.kind must be allowlisted by the client
- rpc.args must be bounded
- auto_run requests immediate execution when permitted

## Client RPC (collaboration surface)

Client RPC is the universal request/response mechanism:
1) agent requests a client_rpc via ui_action
2) client executes a allowlisted handler
3) client posts client_rpc_result and optional client_rpc_progress events
4) agent waits deterministically using client_wait_event/client_wait_any/client_wait_all

### Result event: client_rpc_result

{
  "session_id": "...",
  "type": "client_rpc_result",
  "client": { "id": "webui", "kind": "webui", "instance_id": "tab-123" },
  "data": {
    "rpc_id": "rpc_123",
    "request_tool_call_id": "call_abc",
    "rpc_kind": "media_observe",
    "ok": true,
    "result": { "observing": 1 }
  },
  "append_to_session": false
}

### Progress event: client_rpc_progress

{
  "session_id": "...",
  "type": "client_rpc_progress",
  "data": {
    "rpc_id": "rpc_123",
    "name": "ended",
    "payload": { "ts_unix_ms": 0 }
  }
}

### Safety model

- Clients enforce a strict allowlist of rpc kinds
- Outputs must be bounded (size and count caps)
- Side effects are opt-in (client-side consent gates)

## Entity / Scene model (client-agnostic UI)

DOM mutation is WebUI-specific. The universal abstraction is entities:

- entity_query: read-only introspection
- entity_apply: create/update/delete/action/clear

The daemon can persist a durable Scene (server-side) per session:
- GET /api/v1/session/scene
- POST /api/v1/session/scene/apply

### entity_query example

{
  "kind": "entity_query",
  "args": { "entity_kind": "canvas2d", "id_prefix": "plot-", "limit": 50 }
}

### entity_apply example

{
  "kind": "entity_apply",
  "side_effects": true,
  "args": {
    "ops": [
      {"op": "create", "id": "plot-1", "entity_kind": "canvas2d", "title": "Sine plot", "props": {"width": 640, "height": 240}},
      {"op": "action", "id": "plot-1", "action": "plot_sine", "args": {"amplitude": 1, "frequency": 2}}
    ]
  }
}

### WebUI Canvas2D scripts

The WebUI supports a canvas2d entity with a scriptable draw surface:
- ctx: CanvasRenderingContext2D
- canvas: the canvas element
- width, height: numbers
- props: entity props
- args: props.script_args (or props.args)

This allows powerful rendering without hardcoding client ops.

## Client state snapshots

A bounded snapshot event is supported for "state of the world" checks:

- client posts type="client_state" via /api/v1/session/client_event
- agent can request via ui_action type="request_client_state"
- agent waits with client_wait_event(type="client_state", data_match={query_id:...})

Example payload:

{
  "type": "client_state",
  "data": {
    "query_id": "q1",
    "media": [
      {"kind": "video", "path": "out.mp4", "paused": false, "ended": false, "current_time": 1.23, "duration": 10.0}
    ]
  }
}

Clients should keep snapshots small and bounded.

## Deterministic waits (client_wait_*)

Host tools allow the agent to wait for client acks within a single run.
These are cooperative waits (polling the client event log).

Tool: client_wait_event (preferred; ui_wait_event is the deprecated alias)
Arguments:
- type (required)
- timeout_ms (default 30000, max 300000)
- after_unix_ms (optional)
- path (optional convenience filter for data.path)
- data_match (partial object match)
- max_bytes (optional, default 262144)

Return:
- ok=true with the matched event, or ok=false with timeout/cancelled

Join waits:
- client_wait_any (preferred; ui_wait_any is deprecated)
- client_wait_all (preferred; ui_wait_all is deprecated)

## Definition of Done (DoD) handshake

For UI-visible effects, a host tool returning ok:true does not prove the
user/UI observed the result. Use a handshake:

1) agent produces an effect (artifact_register or ui_action)
2) client posts a correlated event (artifact_rendered, ui_action_shown, or client_rpc_result)
3) agent waits once using client_wait_event or client_wait_all

Recommended patterns:
- Artifact: wait for artifact_rendered with tool_call_id
- UI action: wait for ui_action_shown with tool_call_id
- Client RPC: wait for client_rpc_result with rpc_id

## Client profiles (system prompt extensions)

agentd can inject a client profile as an additional system prompt snippet
based on client.kind. This keeps user prompts clean and captures DoD guidance.

Behavior (current):
- When starting a new session and tools=host, agentd inserts a host system
  prompt (unless disabled) and then appends CLIENT_PROFILE=<kind> if available.
- Profiles live in daemon/src/client_profiles.cpp

## HTTP API quick reference (client-facing)

Service / config:
- GET /api/v1/health
- GET /api/v1/ready
- GET /metrics
- GET /api/v1/config
- POST /api/v1/config/update
- POST /api/v1/ota/update
- GET /api/v1/ota/status

Tools and files:
- GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=...
- GET /api/v1/file?path=...&session_id=...
  - If path is relative and session_id is provided, it is resolved under the
    session root; path traversal segments are rejected.
  - When --file-session-realpath-strict is enabled, symlinks that resolve
    outside the session root are rejected (realpath confinement).
  - If path is relative and session_id is omitted, it is resolved under the
    daemon working directory (no host-scope sandboxing).
  - If path is absolute, it is served directly (auth required).

Memory (durable + deterministic):
- POST /api/v1/memory/consolidate
- POST /api/v1/memory/retention/enforce
- GET /api/v1/memory/checkpoints
- GET /api/v1/memory/index
- GET /api/v1/memory/correlate
- GET /api/v1/memory/query

Sessions:
- GET /api/v1/sessions
- POST /api/v1/session/new
- GET /api/v1/session?session_id=...
- DELETE /api/v1/session?session_id=...
- POST /api/v1/session/upload
- GET /api/v1/session/audit?session_id=...&max_bytes=...
- GET /api/v1/session/artifacts?session_id=...&max_bytes=...&max_artifacts=...
- GET /api/v1/session/client_events?session_id=...
- GET /api/v1/session/scene?session_id=...
- POST /api/v1/session/scene/apply

Client events:
- POST /api/v1/session/client_event
- POST /api/v1/session/ui_event (legacy alias)

Notes:
- All /api/v1/* endpoints (except health/ready/metrics) require auth when enabled.
- The endpoint catalog mirrors daemon/src/agentd_api.cpp.
---

# Source: docs/DB.md

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
- Default: `./agentd.db` in the daemon working directory.

Note: DB persistence respects `no_session: true` requests. If a run is marked ephemeral, the daemon will not persist it to disk.

## Schema versioning

The DB includes a small `meta` table with a single key:

- `schema_version` (integer stored as text)

The daemon runs idempotent schema setup on open and will migrate older DB files forward. If the DB is newer than the current
binary (e.g. you downgrade `agentd`), `agentd` refuses to open it rather than silently corrupting the schema.

## Schema (v29)

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
---

# Source: docs/DEPLOYMENT.md

# Production Deployment Guide (agentd + broker + WebUI)

This guide focuses on **production-grade** deployment patterns for:
- `agentd` (daemon backend)
- `agentd-broker` + `agentd-connector` (secure relay + NAT traversal)
- WebUI (static site)

## Recommended topology

**Best practice**: expose **broker** publicly, keep `agentd` private.
- Clients authenticate to broker via OIDC/JWT.
- Broker proxies requests to agentd over mTLS (connector).
- Agentd can stay on loopback or private networks only.

Direct `agentd` exposure is supported, but you must harden it (see below).

---

## agentd (daemon)

### Hardening checklist
- **Auth**: use `--auth-token` (required for non-loopback binds).
- **CORS**: allowlist UI origins with `--cors-origin <https://ui.example>`.
- **State**: set `--state-dir` and `--db-path` on a persistent disk.
- **Secrets**: keep provider keys out of the UI; load via `.not_in_repo` or env. For service contexts,
  `AGENTD_DOTENV_PATH=/path/to/.env` can point the daemon at a specific dotenv file.
- **HTTP tasks**: keep `--workflow-enable-http-tasks` **off** unless needed; if enabled, set:
  - `--workflow-http-allow-host ...` and/or `--workflow-http-allow-cidr ...`
  - `--workflow-http-deny-private` (recommended)
  - `--workflow-http-dns-pin` (recommended)

### Example (systemd)
```
[Unit]
Description=agentd daemon
After=network.target

[Service]
User=agentd
Environment=AGENTD_AUTH_TOKEN=REPLACE_WITH_RANDOM_TOKEN
Environment=AGENTD_DOTENV_PATH=/etc/agentd/agentd.env
WorkingDirectory=/var/lib/agentd
ExecStart=/usr/local/bin/agentd \
  --host 127.0.0.1 \
  --port 8123 \
  --auth-token ${AGENTD_AUTH_TOKEN} \
  --state-dir /var/lib/agentd \
  --db-path /var/lib/agentd/agentd.db
Restart=always
RestartSec=3
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

### Example (macOS launchd)
```
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.agentd.daemon</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>AGENTD_DOTENV_PATH</key><string>/Users/you/Library/Application Support/agentd/agentd.env</string>
  </dict>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/agentd</string>
    <string>--host</string><string>127.0.0.1</string>
    <string>--port</string><string>8123</string>
    <string>--auth-token</string><string>REPLACE_WITH_RANDOM_TOKEN</string>
    <string>--state-dir</string><string>/Users/you/Library/Application Support/agentd</string>
    <string>--db-path</string><string>/Users/you/Library/Application Support/agentd/agentd.db</string>
  </array>
  <key>WorkingDirectory</key><string>/Users/you/Library/Application Support/agentd</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/Users/you/Library/Logs/agentd.out.log</string>
  <key>StandardErrorPath</key><string>/Users/you/Library/Logs/agentd.err.log</string>
</dict>
</plist>
```

### Helper scripts (macOS)

For local bring-up on macOS, you can install/remove launchd services with:
```
AGENTD_AUTH_TOKEN="REPLACE_WITH_RANDOM_TOKEN" tools/install_agentd_launchd.sh
tools/uninstall_agentd_launchd.sh
```
You can also point the daemon at a specific dotenv file when installing the service:
```
AGENTD_DOTENV_PATH="/path/to/.env" tools/install_agentd_launchd.sh
```

Environment overrides:
- `AGENTD_BIN` (default `./build/agentd`)
- `AGENTD_HOST`, `AGENTD_PORT`
- `AGENTD_STATE_DIR`, `AGENTD_DB_PATH`
- `AGENTD_DOTENV_PATH` (optional override for provider key discovery)
- `AGENTD_AUTH_TOKEN`, `AGENTD_AUTH_COOKIE`
- `AGENTD_TOOLS`, `AGENTD_YOLO`
- `AGENTD_HOST_SCOPE`, `AGENTD_TOOLS_ROOT`
- `AGENTD_CORS_ORIGINS` (comma-separated)
- `AGENTD_CORS_ALLOW_HEADERS`, `AGENTD_CORS_ALLOW_METHODS`
- `AGENTD_CORS_ALLOW_CREDENTIALS`, `AGENTD_CORS_MAX_AGE_SECONDS`
- `AGENTD_CORS_ROUTES` (JSON array of `{path_prefix, origins}`)
- `AGENTD_UPLOAD_MAX_BYTES` (per-file session upload cap)
- `AGENTD_EXTRA_ARGS` (space-delimited)

### OTA updates (agentd)

OTA is **disabled by default**. Enable it explicitly and provide an update command:

- `--ota-enable` (or env `AGENTD_OTA_ENABLE=1`)
- `--ota-command /path/to/tools/agentd_ota_apply.sh` (or env `AGENTD_OTA_COMMAND`)

The reference script reads a plan file from `AGENTD_OTA_PLAN_PATH` and uses optional env hints:

- `AGENTD_OTA_TARGET_BIN` — target binary path to replace (recommended)
- `AGENTD_OTA_RESTART` — `systemd` | `launchd` | `signal` (default: `signal`)
- `AGENTD_OTA_SERVICE` — service name/label for restart (systemd/launchd)

Example (systemd):
```
Environment=AGENTD_OTA_ENABLE=1
Environment=AGENTD_OTA_COMMAND=/opt/agentd/tools/agentd_ota_apply.sh
Environment=AGENTD_OTA_TARGET_BIN=/usr/local/bin/agentd
Environment=AGENTD_OTA_RESTART=systemd
Environment=AGENTD_OTA_SERVICE=agentd
```

Trigger via API:

- `POST /api/v1/ota/update` with `{ url, sha256?, version?, reason?, drain_timeout_ms? }`
- `GET /api/v1/ota/status` for current state

OTA enters **drain mode** while the update runs:
- new run/workflow submissions return `HTTP 503` + `drain_*` hints
- job/workflow schedulers pause claiming new tasks
- `GET /api/v1/ota/status` surfaces `drain_active`, `drain_until_unix_ms`, `drain_reason`
- Status also reports best-effort queue pressure (`jobs_running`, `jobs_queued`, `workflow_tasks_running`,
  `workflow_tasks_queued`, `workflows_running`) when the DB is available.
- agentd waits (best-effort) for **running jobs/workflow tasks** to finish until `drain_timeout_ms` elapses; any remaining
  work is resumed after restart.

WebUI + broker fanout:
- In broker mode, the WebUI lists connected deployments and can send OTA requests to **all selected deployments** using broker bulk OTA endpoints:
  - `POST /v1/agents/{agent_id}/ota/update` (body may include `deployment_ids`).
  - `GET  /v1/agents/{agent_id}/ota/status` (query `deployment_ids=...` optional).
- Status checks also fan out per deployment, so operators can verify drain/rollout progress without logging into each host.

Local verification:
- `tools/verify_ota_continuity.sh` runs a delay workflow, triggers an OTA update, restarts agentd, and confirms the workflow resumes.

### Example (Windows service)

#### Option A: `sc.exe` (built-in)
Run from an **elevated** Command Prompt:
```
setx AGENTD_DOTENV_PATH "C:\\ProgramData\\agentd\\agentd.env" /M
sc.exe create agentd binPath= "\"C:\\Program Files\\agentd\\agentd.exe\" --host 127.0.0.1 --port 8123 --auth-token REPLACE_WITH_RANDOM_TOKEN --state-dir \"C:\\ProgramData\\agentd\" --db-path \"C:\\ProgramData\\agentd\\agentd.db\"" start= auto
sc.exe config agentd start= delayed-auto
sc.exe failure agentd reset= 86400 actions= restart/5000/restart/5000/restart/5000
```

#### Option B: PowerShell `New-Service`
Run from an **elevated** PowerShell prompt:
```
[Environment]::SetEnvironmentVariable("AGENTD_DOTENV_PATH", "C:\\ProgramData\\agentd\\agentd.env", "Machine")
$exe = "C:\\Program Files\\agentd\\agentd.exe"
$args = "--host 127.0.0.1 --port 8123 --auth-token REPLACE_WITH_RANDOM_TOKEN --state-dir `\"C:\\ProgramData\\agentd`\" --db-path `\"C:\\ProgramData\\agentd\\agentd.db`\""
New-Service -Name "agentd" -BinaryPathName "`"$exe`" $args" -StartupType Automatic
```

### If exposing `agentd` directly
- Put it behind a reverse proxy with TLS (nginx/Caddy/Envoy).
- **Do not** bind directly to `0.0.0.0` without `--auth-token`.
- Use strict CORS allowlists.
- If using cookie auth, enable CORS credentials (`--cors-allow-credentials`).
- Set HTTP safety limits via env:
  - `AGENTD_HTTP_MAX_BODY_BYTES` (request body cap)
  - `AGENTD_HTTP_MAX_HEADER_BYTES` (request header cap)
  - `AGENTD_HTTP_READ_TIMEOUT_MS` (read timeout for slow clients)

---

## Broker + Connector

### Broker requirements
- TLS enabled (`--tls-cert`, `--tls-key`)
- mTLS enabled for connectors (`--tls-client-ca`)
- OIDC enabled for users (`--oidc-issuer`, `--oidc-audience`)
- Durable DB (Postgres) with backups
- HTTP tunables:
  - `--max-body-bytes`, `--max-header-bytes`
  - `--read-timeout`, `--write-timeout`, `--idle-timeout`, `--read-header-timeout`
  - Env overrides: `AGENTD_BROKER_MAX_BODY_BYTES`, `AGENTD_BROKER_MAX_HEADER_BYTES`,
    `AGENTD_BROKER_READ_TIMEOUT_MS`, `AGENTD_BROKER_WRITE_TIMEOUT_MS`,
    `AGENTD_BROKER_IDLE_TIMEOUT_MS`, `AGENTD_BROKER_READ_HEADER_TIMEOUT_MS`
- Browser clients:
  - CORS allowlist with `--cors-origin` / `--cors-origins`
  - Cookie auth (optional): `--auth-cookie <name>` + `--cors-allow-credentials`

### Connector requirements
- Unique `--agent-id` per agentd instance
- mTLS client certs issued by broker CA
- Runs close to the agentd instance (same host or VPC)

### Example (broker flags)
```
agentd-broker \
  --listen :8443 \
  --tls-cert /etc/agentd/tls/server.pem \
  --tls-key /etc/agentd/tls/server.key.pem \
  --tls-client-ca /etc/agentd/tls/ca.pem \
  --db-dsn postgres://... \
  --oidc-issuer https://id.example.com/realms/agentd \
  --oidc-audience agentd-broker \
  --cors-origin https://ui.example.com
```

Optional (non-UI service clients): static client token file:
```
agentd-broker \
  --client-auth-file /etc/agentd/client_auth.json \
  --client-auth-fallback \
  --client-auth-reload-ms 60000 \
  --client-auth-strict \
  --client-auth-max-age-ms 120000
```

Env equivalents:
- `AGENTD_BROKER_CLIENT_AUTH_FILE=/etc/agentd/client_auth.json`
- `AGENTD_BROKER_CLIENT_AUTH_FALLBACK=1`
- `AGENTD_BROKER_CLIENT_AUTH_RELOAD_MS=60000`
- `AGENTD_BROKER_CLIENT_AUTH_STRICT=1`
- `AGENTD_BROKER_CLIENT_AUTH_MAX_AGE_MS=120000`

You can also send `SIGHUP` to reload the client auth file immediately.

### Example (connector flags)
```
agentd-connector \
  --broker wss://broker.example.com/v1/agent/connect \
  --local-agentd http://127.0.0.1:8123 \
  --tls-ca /etc/agentd/tls/ca.pem \
  --tls-cert /etc/agentd/tls/client.pem \
  --tls-key /etc/agentd/tls/client.key.pem \
  --agent-id agent-123
```

---

## macOS (MacBook M2) — broker + WebUI

Recommended on macOS: run **broker + connector + WebUI** via Docker Compose, and keep `agentd` as a local launchd service.

### Prereqs
- Docker Desktop or Colima
- `docker compose` available

### Local prod-like stack (broker + connector + WebUI)
```
cp project.local.md.example project.local.md
export WEBUI_PUBLISHED_PORT=8100
export AGENTD_PUBLISHED_PORT=8123
export BROKER_PUBLISHED_PORT=8443
export KEYCLOAK_PUBLISHED_PORT=8081
docker compose up -d postgres keycloak broker connector webui
```

Then run `agentd` locally (launchd or foreground):
```
./build/agentd --host 127.0.0.1 --port 8123 --auth-token "REPLACE_WITH_RANDOM_TOKEN" --state-dir "$HOME/Library/Application Support/agentd" --db-path "$HOME/Library/Application Support/agentd/agentd.db"
```

### Manual verification (macOS)
1) Broker health:
```
curl -k https://127.0.0.1:8443/healthz
```
2) Agentd health:
```
curl http://127.0.0.1:8123/api/v1/health
```
3) WebUI bring-up:
- Open `http://127.0.0.1:8100`
- Set **Base URL** to `http://127.0.0.1:8123`
- Set **Daemon Auth Token** to the `--auth-token` value above
4) Functional check:
- Run a short prompt that emits an audio artifact (or scene audio).
- If autoplay is blocked by the browser, click once in the UI to unlock media playback.

### Notes
- Broker TLS in compose uses test certificates under `tools/_compose_mtls` (local only).
- WebUI expects to talk to broker proxy or direct agentd; align CORS + auth tokens accordingly.
- Compose mounts `tools/agentui-config.compose.js` into the WebUI container to default to broker mode.
  - Broker mode exposes a broker console panel for agent selection + membership management + audit.

### WebUI Playwright smoke (optional)

From `ui/`:
- `npm run e2e:agentd` (agentd host UI smoke)
- `npm run e2e:broker` (broker console UI smoke)

From repo root (headless capture + traces/videos/screenshots):
- `tools/run_ui_playwright_smoke.sh`

### Evidence bundles (optional)

Capture a lightweight snapshot for debugging or attachable evidence:
- `tools/capture_agent_evidence_bundle.sh --agentd-base http://127.0.0.1:8123`

Validate a captured bundle:
- `python3 tools/check_agent_evidence_bundle.py --dir docs/artifacts/evidence/<bundle_dir>`

### Scenario runner (optional)

Run a JSON scenario that captures logs + evidence:
- `python3 tools/scenario_runner.py --file tools/scenarios/agentd_smoke.json`

Run all scenarios and validate evidence bundles:
- `python3 tools/scenario_pack.py --dir tools/scenarios --validate`

### One-command devstack (optional)

Bring up agentd + broker + connector + WebUI on the host (Postgres + Keycloak via Docker), then run smoke checks + capture evidence:
- `tools/devstack_agent.sh`

Stop the stack:
- `tools/devstack_agent_down.sh`
  - Edit that file to adjust broker URL, agent id, or pass-through daemon token for local dev.
- For a one-command macOS verification run, use: `tools/verify_mac_full_stack.sh`.
- If Docker is unavailable or resource-constrained, you can verify the local stack without compose:
  - `tools/verify_mac_local_stack.sh` (agentd + WebUI only; no broker)
  - Optional env: `MAC_LOCAL_SKIP_UI=1` to skip WebUI build/serve, `MAC_LOCAL_UI_INSTALL=0` to skip `npm ci` when deps already exist
- If Docker build is blocked but Docker itself runs, you can verify a host-mode full stack:
  - `tools/verify_mac_full_stack_host.sh` (runs Postgres + Keycloak via Docker, and runs agentd/broker/connector/WebUI on the host)
- If `tools/verify_mac_full_stack.sh` skips due to Docker build resource errors (e.g. `unpigz`/`runc`),
  restart Docker Desktop or increase CPU/RAM (Docker Desktop → Settings → Resources; also ensure disk image size).
  The script already retries builds, can fall back to the
  legacy builder, and throttles pigz threads. You can also tweak:
  - `COMPOSE_BUILD_SERIAL=1` (default) to reduce concurrency
  - `COMPOSE_BUILD_RETRIES=3` (default) to raise retry attempts
  - `PIGZ=-p1 GZIP=-p1` to reduce decompression thread pressure
  - `COMPOSE_BUILD=0` to skip image rebuilds when you already have fresh images (requires images present; otherwise it skips)
  - `COMPOSE_PULL=1` to auto-pull missing images when `COMPOSE_BUILD=0`
- Prebuilt images (optional):
  - set `BROKER_IMAGE`, `AGENTD_IMAGE`, `CONNECTOR_IMAGE`, `WEBUI_IMAGE` to registry tags
  - run `COMPOSE_BUILD=0 COMPOSE_PULL=1 ./tools/verify_compose_stack.sh`

---

## WebUI (static)

- Build once and host on a static server:
  - `cd ui && npm ci && npm run build`
- Serve `ui/dist/` via nginx/Caddy/S3.
- Runtime defaults (no rebuild): edit `ui/dist/agentui-config.js` to set:
  - `connectionMode` (`direct` or `broker`)
  - `daemonBaseUrl`, `brokerBaseUrl`, `brokerAgentId`
  - `brokerDeploymentId` (optional; target a specific agentd deployment)
  - `daemonAuthToken`, `brokerAuthToken` (if you accept putting tokens in a static file)
  - `model`, `baseUrl`, `proxyUrl`, `timeoutMs`
  - `tools`, `yolo`, `hostPolicy`, `verbose`
  - `allowClientRpcs`, `allowClientEffects`, `allowUnsafePageEval`
- Build-time overrides (optional): `VITE_AGENTD_BASE_URL`, `VITE_BROKER_BASE_URL`, and `VITE_AGENTUI_*` variants above.
- Configure UI to talk to:
  - Broker proxy (recommended), or
  - Direct agentd base URL (ensure CORS + auth token).
- The WebUI supports **multiple connection profiles** stored in browser localStorage; use Settings to add/switch
  between multiple agentd deployments (direct or broker-backed).
- Each profile can optionally use **profile-specific run settings** (model/provider, tool flags, run limits, etc.);
  toggle "Profile-specific run settings" in Settings → Model / Provider.
- Session selection, history UI state, job resume, and scene cache are scoped per profile (and base URL) to avoid
  collisions when switching between deployments.

---

## Operational checklist

- **Backups**: SQLite (`agentd.db`) + Postgres (broker).
- **Logs**: rotate daemon + broker logs.
- **Monitoring**: scrape `/healthz`, `/readyz`, `/metrics` (broker) + `/api/v1/health`, `/api/v1/ready`, `/metrics` (agentd).
- **Access logs**: set `AGENTD_ACCESS_LOG=1` (text) or `AGENTD_ACCESS_LOG=json` for machine-parsable access logs.
- **Resource limits**: set process limits and container quotas.
- **Upgrades**: keep `agentd` and WebUI in lockstep (OpenAPI + protocol changes).

---

## Docker stack hardening checklist

- **Secrets**: keep provider keys out of images; inject via env or Docker/Swarm secrets.
- **TLS**: terminate TLS at broker; use valid certs in production (replace test CA).
- **mTLS**: ensure connector certs are per-agent and rotated.
- **Least exposure**: only publish broker/WebUI ports; keep agentd private.
- **Backups**: snapshot Postgres + agentd SQLite volume.
- **Observability**: log rotation + metrics scraping.
- **Image pinning**: pin broker/agentd build images by digest for deterministic rollouts.

---

## Local prod-like stack

Use `docker-compose.yml` + `tools/verify_compose_stack.sh` for a local, production-like integration test of:
Postgres + Keycloak + broker + connector + agentd + WebUI.
---

# Source: docs/DIAGNOSTICS.md

# Diagnostics Endpoints

Date: 2026-02-19

This document describes the lightweight diagnostics endpoints exposed by `agentd`.

Goals:
- Give operators a **fast health snapshot** beyond `/api/v1/health`.
- Surface provider key presence **without leaking secrets**.
- Provide a small provider smoke test endpoint for quick verification.

Additional goals:
- Deliver full observability by unifying traces, audit logs, and DB query endpoints.
- Provide long-running benchmark and soak runs via dedicated load-test harnesses.

## Auth

All diagnostics endpoints require Bearer auth when `agentd` is started with `--auth-token`.

## Endpoint: `/api/v1/diagnostics`

Returns a compact snapshot with:
- readiness (`ready`)
- DB metadata (`db.path`, `db.size_bytes`)
- DB table counts (`db.tables.*`)
- job status counts (`jobs.by_status`)
- workflow scheduler stats (`workflows.*`)
- current provider selection (`active_provider`)

Example:

```json
{
  "ok": true,
  "service": "agentd",
  "version": "0.1",
  "now_unix_ms": 1739490000000,
  "uptime_ms": 123456,
  "active_provider": "openai",
  "active_provider_key_present": false,
  "active_provider_base_url": "https://api.openai.com/v1",
  "active_provider_base_url_source": "default",
  "ready": true,
  "checks": { "db_open": true },
  "db": {
    "path": "/path/to/agentd.db",
    "size_bytes": 1234567,
    "tables": {
      "sessions": 12,
      "messages": 345,
      "runs": 78,
      "events": 910,
      "artifacts": 5,
      "ui_actions": 2,
      "client_events": 0,
      "audit_records": 80,
      "jobs": 3,
      "workflows": 7,
      "workflow_tasks": 22,
      "workflow_events": 41,
      "edge_nodes": 0,
      "edge_tasks": 0,
      "edge_workflows": 0,
      "blob_manifest": 2,
      "artifact_blobs": 1
    }
  },
  "jobs": {
    "total": 3,
    "by_status": { "queued": 1, "running": 0, "done": 2 }
  },
  "workflows": {
    "workflows_by_status": { "queued": 1, "running": 0, "done": 6 },
    "tasks_by_status": { "queued": 2, "running": 0, "done": 20 },
    "tasks_queued_ready": 1,
    "tasks_queued_not_ready": 1
  }
}
```

Notes:
- `active_provider` is derived from `daemon.base_url` (defaults to OpenAI-compatible).
- `active_provider_key_present` is a boolean quick check for whether the active provider has any key configured.
- `active_provider_key_source` is present when a key is found (`kind` + `label` mirror the providers endpoint).
- `active_provider_base_url` / `active_provider_base_url_source` report where the active base URL came from (`config`, `env`, `default`).
- If any counters fail to load, the response includes a `warnings[]` array.

## Endpoint: `/api/v1/diagnostics/providers`

Returns **provider key presence** and best-effort source (config/env/file), plus base URL and model defaults.

Example:

```json
{
  "ok": true,
  "service": "agentd",
  "version": "0.1",
  "now_unix_ms": 1739490000000,
  "uptime_ms": 123456,
  "providers": {
    "deepseek": {
      "active": false,
      "key_present": true,
      "source": { "kind": "env", "label": "DEEPSEEK_API_KEY" },
      "base_url": "https://api.deepseek.com",
      "base_url_source": "default",
      "model_default": "deepseek-reasoner"
    },
    "moonshot": {
      "active": true,
      "key_present": true,
      "source": { "kind": "file", "label": "AGENTD_DOTENV_PATH" },
      "base_url": "https://api.moonshot.cn/v1",
      "base_url_source": "config",
      "model_default": "kimi-k2.5"
    }
  }
}
```

Notes:
- `active` indicates the daemon’s current provider selection (derived from `daemon.base_url`).
- `key_present` is **boolean only**. Secret values are never returned.
- `source.kind` is one of: `config`, `env`, or `file`.
- `source.label` is a best-effort descriptor (e.g., `provider_keys`, `api_key`, `.not_in_repo`, `project.local.md`, `~/.env`,
  or `AGENTD_DOTENV_PATH` when explicitly configured).
- `warning` is an optional provider-specific hint (e.g., active provider key missing, key detected but base_url not configured, or Moonshot key detected but OpenRouter key missing).
- `~/.env` is only consulted for provider **keys**; base URLs still come from flags/env/config. You can override the dotenv
  path with `AGENTD_DOTENV_PATH=/path/to/.env`, which is useful for services running under a different user. If no base URL
  is set, the provider defaults to OpenAI-compatible.
- `base_url_source` indicates where the provider base URL came from: `config`, `env`, or `default`.

## Endpoint: `/api/v1/diagnostics/provider_test`

Runs a small provider test without creating a session.

Request fields:
- `provider` (required): `deepseek`, `moonshot`, `openrouter`, `openai`
- `base_url` (optional): override provider base URL
- `model` (optional): override model
- `prompt` / `expect` (optional): basic expectation matcher (`assistant_text` must match `expect`)
- `tools` (optional): `none|basic|host`
- `require_tool_call` (optional): require at least one tool call
- `timeout_ms` (optional)
- Tool-loop limits (optional): `max_steps`, `max_tool_calls_total`, `max_tool_calls_per_tool`,
  `max_tool_call_args_chars`, `max_tool_result_chars`, `max_repeated_tool_calls`
- `include_run` (optional): echo the full run response in `run`

Example (DeepSeek reasoner + tool call):

```bash
curl -sS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{
    "provider": "deepseek",
    "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
    "expect": "40",
    "tools": "basic",
    "require_tool_call": true,
    "max_steps": 6
  }' \
  http://127.0.0.1:8123/api/v1/diagnostics/provider_test
```

Example (Moonshot/Kimi tool call):

```bash
curl -sS -H "Authorization: Bearer ${AGENTD_AUTH_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{
    "provider": "moonshot",
    "prompt": "Use the calculator tool to compute (2+2)*10. Return exactly: 40",
    "expect": "40",
    "tools": "basic",
    "require_tool_call": true,
    "max_steps": 6
  }' \
  http://127.0.0.1:8123/api/v1/diagnostics/provider_test
```

Response fields:
- `ok` (bool)
- `provider`, `base_url`, `model`
- `duration_ms`
- `assistant_text` (truncated at 2048 chars)
- `error` (when non-OK)
- `http_status` (best-effort provider HTTP status)
- `run` (optional, when `include_run=true`)

Notes:
- Provider tests use the same API key resolution as normal runs (config, env, repo-local secrets).
- No session is created (`no_session=true`).
- Tests respect daemon limits and defaults.
---

# Source: docs/DOD_ACK.md

# Definition of Done (DoD) for UI-visible Effects

Date: 2026-02-19

This document defines a **fundamental stop condition** for “agent did something for the user” workflows in the Web UI.

The core issue:
- A host tool returning `ok:true` proves the host-side action succeeded (file created, tool ran).
- It does **not** prove the user/UI actually **received/observed** the effect (rendered in browser, played, acknowledged).

Therefore, for UI-visible effects (artifacts, notifications, playback), a reliable DoD requires a **handshake**:

1) Agent produces an effect (artifact registered / UI action requested).
2) Client confirms it actually rendered/performed it (client event).
3) Agent waits for that confirmation once (no retry loop).

This is complementary to hard bounds (`max_steps`, tool call limits). Bounds are guardrails; the handshake is the root-cause fix.

## Goals

- Make “done” observable for UI-visible effects so the agent stops repeating work.
- Support deterministic waiting for:
  - one acknowledgement (`ui_wait_event`)
  - multiple acknowledgements (“join”) (`ui_wait_any`, `ui_wait_all`)
- Keep behavior robust when DB is disabled (file-backed client events are canonical).
- Keep everything bounded (time, bytes, recursion depth).

## Scope notes

This document is narrowly about a **deterministic stop condition** (“did the UI/client actually observe the effect?”).

This project explicitly **demands power capabilities** (automation, entity creation/editing/actions, DOM mutation, media control).
The DoD handshake is what makes those capabilities usable in practice: powerful systems that cannot tell when they are done will
retry forever and appear “broken”.

This document does not try to “bypass” browser permission policies (e.g. autoplay gesture requirements). Instead, it specifies the
handshake so agents can act powerfully *and still stop* based on observable facts.

## Protocol primitives

### Effect emission (agent → UI)

- `artifact_register` host tool → derived `artifact` event
- `ui_action` host tool → derived `ui_action` event

### Effect acknowledgement (UI → agentd)

The client posts client events via:
- `POST /api/v1/session/client_event` (preferred; `ui_event` is a legacy alias)

Canonical storage:
- Legacy (file-backed): `<sessions_root>/<session_id>.client_events.jsonl` (current canonical store is the DB)

Recommended event types:
- `artifact_rendered`: UI mounted the artifact renderer for a specific artifact/tool call.
  - payload: `{ path, kind, title?, tool_call_id? }`
- `ui_action_shown`: UI rendered a UI-action card.
  - payload: `{ action_type, title?, tool_call_id? }`
- `notification_ack`: user explicitly acknowledged a notification (manual DoD).

### Waiting / joining acks (agent tool)

Agents should use host tools (session-scoped):
- `ui_wait_event`: wait for one matching event
- `ui_wait_any`: wait until any predicate matches (OR join)
- `ui_wait_all`: wait until all predicates match (AND join)

## Recommended DoD patterns

### “Capture screenshot and present to UI”

1) Produce image file
2) `artifact_register(path=..., kind=image, title=...)`
3) Wait once:
   - `ui_wait_event(type="artifact_rendered", data_match={ tool_call_id: "<tool_call_id>" })`
4) Respond with a short “done” assistant message and stop

### “Present an artifact and stop (generic)”

1) Produce a host file (any format) under the daemon tools root
2) `artifact_register(path=..., kind=..., title=...)`
3) Wait once:
   - `client_wait_event(type="artifact_rendered", data_match={ tool_call_id: "<tool_call_id>" })`

If the task requires active client-side interaction (e.g. playback, DOM edits), request it as a `client_rpc`
(`dom_apply` / `page_eval` / `script_eval` / `media_play`) and wait for `client_rpc_result` instead of assuming it happened.

### Join multiple UI acknowledgements

Example: show notification + play audio, continue after both:
- `ui_wait_all(predicates=[ notification_ack(tool_call_id=...), client_rpc_result(rpc_id=...) ])`

### “Wait for a client-side condition (generic)”

When an agent must decide based on client state (DOM/media/location), use a client RPC:

1) Request RPC:
   - `ui_action(type="client_rpc", rpc_id="<tool_call_id>", rpc={kind:"media_snapshot", args:{...}}, auto_run=true)`
2) Wait for the result:
   - `client_wait_event(type="client_rpc_result", data_match={rpc_id:"<tool_call_id>"})`

For long-running conditions (like “wait until media ended”), prefer observation:
- `ui_action(type="client_rpc", rpc_id="<id>", rpc={kind:"media_observe", side_effects:true, args:{tool_call_id:"<call>"}}, auto_run=true)`
- `client_wait_event(type="client_rpc_progress", data_match={rpc_id:"<id>", name:"ended"})`
- `ui_action(type="client_rpc", rpc_id="<id>", rpc={kind:"media_unobserve", args:{rpc_id:"<id>"}}, auto_run=true)` to detach observers

### “Do side-effecting client automation and stop”

When an agent uses the collaboration surface to cause side effects (DOM edits, entity operations, playback, navigation), the DoD is
still a handshake:

1) Request the side effect as a client RPC:
   - `ui_action(type="client_rpc", rpc_id="<tool_call_id>", rpc={kind:"dom_apply"|"...", side_effects:true, args:{...}}, auto_run=true)`
2) Wait once for the correlated outcome:
   - `client_wait_event(type="client_rpc_result", data_match={rpc_id:"<tool_call_id>", ok:true})`

If the action is long-running, prefer progress events and wait for a phase:
- `client_wait_event(type="client_rpc_progress", data_match={rpc_id:"<tool_call_id>", name:"finished"})`

## Future: concurrent jobs (design sketch)

The daemon already supports concurrent **runs** via `run_async` + job manager.

What we do not yet support is concurrent host tool tasks *within a single tool-loop decision* (e.g. “build + tests in parallel”).

A future-safe design would introduce “tool jobs”:
- `tool_job_start(tool_name, args)` → returns `{ job_handle }`
- `tool_job_poll(job_handle)` → returns `{ status, partial_output? }`
- `tool_job_wait(job_handle, timeout_ms)` → returns `{ final_output }`
- cancellation wired to daemon job cancellation

This enables the agent to reason about multiple ongoing tasks (peek status) and join them deterministically, but is intentionally
deferred until the tool/job API surface is stabilized.
---

# Source: docs/EDGE_INTEROP.md

# Edge Agent Interop (UM‑EAIS v0.1) — Platform/Broker Support in `agentd`

Date: 2026-02-04

This repo implements the **platform/broker** side of the UM‑EAIS v0.1 draft spec (transport-agnostic payload semantics).

Canonical spec (copied from `../urine_monitor`):
- `docs/spec/um-eais/um-eais-v0.1.md` (copied from `../urine_monitor` commit `278ad9e5`)
- `docs/spec/um-eais/edge_agent_interop_handoff_to_agent_repo.md` (node-side handoff checklist, copied from the same source)

This document describes the **HTTP transport mapping** implemented by `agentd` for that payload-level spec.

Executable contract artifacts (this repo):
- Schemas: `docs/spec/um-eais/schema/` (envelope + core + platform extensions)
- Golden transcript fixtures: `docs/spec/um-eais/fixtures/` (JSONL)
- Proof: `ctest` includes `um_eais_spec_sanity_tests`, `agentd_edge_interop_transcript_replay_smoke`, and
  `agentd_edge_interop_task_loop_replay_smoke`.

## Design

UM‑BMP defines:
- the **message envelope** (`msg_id`, `ts_utc_ms`, `type`, `from`, `to`, `body`)
- the **semantics** for discovery + tasking + events

UM‑BMP does *not* mandate a transport. `agentd` provides a simple, robust mapping:

- **Ingress**: `POST /api/v1/edge/message` (any UM‑BMP envelope; idempotent by `msg_id`)
- **Egress**: `GET /api/v1/edge/outbox?node_id=...&cursor=0&limit=256` (per-node outbox cursor)

This makes it easy to layer:
- MQTT gateway (topic → HTTP bridge)
- WebSocket gateway
- LoRa hub backhaul

…without changing payload semantics.

## Node-initiated collaboration (platform extensions)

In addition to the canonical UM‑EAIS v0.1 message types, `agentd` implements a small set of platform-side extensions that
enable **node-initiated orchestration** (handoff to the coordinator) over the same ingress pipe:

- `WORKFLOW_SUBMIT` (node → platform): persist a new edge workflow (`edge_workflows`, `edge_workflow_steps`)
- `WORKFLOW_CANCEL` (node → platform): cancel an edge workflow (`CANCELED`)
- `DURABLE_WORKFLOW_SUBMIT` (node → platform): submit a **durable workflow** to the platform workflow engine
  (same semantics as `POST /api/v1/workflow/submit`, but transported over UM‑BMP via `POST /api/v1/edge/message`)
- `DURABLE_WORKFLOW_CANCEL` (node → platform): cancel a durable workflow
  (same semantics as `POST /api/v1/workflow/cancel`, but transported over UM‑BMP via `POST /api/v1/edge/message`)

Details: `docs/spec/um-eais/um-eais-platform-extensions-v0.1.md`

ACK note:
- For `WORKFLOW_SUBMIT` and `WORKFLOW_CANCEL`, the platform enqueues a best-effort outbox `WORKFLOW_ACK` so non-HTTP transports can observe
  submit/cancel outcomes.

Security note:
- For node-submitted durable workflows, the platform forces `allow_inline_api_keys=false` (nodes should not ship provider keys).
  Use daemon defaults (`--base-url`, `--api-key`, or provider keys) or a trusted gateway that authenticates and sets defaults.

## Endpoints

All endpoints require daemon auth when `agentd` is started with `--auth-token`.

### Ingest UM‑BMP envelopes

`POST /api/v1/edge/message`

Wire encoding:
- Default: `Content-Type: application/json` (envelope is JSON object).
- Optional MCU/gateway profile: `Content-Type: application/cbor` (envelope is CBOR map with the same keys/shape).
  - This is intended for constrained transports (LoRa/MQTT bridges) where JSON overhead is material.
  - The platform requires definite-length CBOR items and text-string map keys (no indefinite-length streaming items).
  - MCU encoder notes: `docs/spec/um-eais/mcu_cbor_encoder_notes.md`

This stores the envelope durably (`edge_inbox_messages`) and updates platform state:
- `NODE_HELLO`, `NODE_HEARTBEAT` update `edge_nodes`
  - `NODE_HEARTBEAT` best-effort persists `body.health` (and optional `battery_pct` / `rssi`) into `edge_nodes.health_json`,
    surfaced via `GET /api/v1/edge/node`.
- `NODE_CAPS_RSP` stores manifest + extracts tags/tools/presence
- `TASK_*` messages update `edge_tasks` + append `edge_task_events`
- `SENSOR_EVENT` appends `edge_sensor_events`

If the platform sees a new/unknown `caps_sha256`, it queues a `PLATFORM_CAPS_REQ` to the node outbox.

Envelope authenticity (optional, UM‑BMP auth v0.4):
- Envelopes MAY include an `auth` object:
  - `auth.alg` (string):
    - `"hmac-sha256"`: HMAC over canonical JSON bytes (`agent_json_c14n_v1`)
    - `"hmac-sha256-cbor"`: HMAC over deterministic CBOR bytes (`daemon/src/cbor_encode.*`)
    - `"ed25519"`: Ed25519 signature over canonical JSON bytes (`agent_json_c14n_v1`)
    - `"ed25519-cbor"`: Ed25519 signature over deterministic CBOR bytes (`daemon/src/cbor_encode.*`)
  - `auth.kid`: key id selecting an operator-provisioned shared secret (HMAC) or public key (Ed25519)
  - `auth.seq`: optional monotonic sequence number (anti-replay; when enabled by the platform)
  - `auth.sig`: base64 of the signature bytes:
    - 32 bytes for HMAC-SHA256
    - 64 bytes for Ed25519
  - Signing input (all algs):
    - signing input is the envelope with `auth.sig` removed (auth metadata like `kid`/`seq` stays in the signed bytes)
    - for `*-cbor` algs: deterministic CBOR bytes with:
      - definite lengths only
      - text-string map keys ordered by UTF‑8 byte length, then lexicographically by UTF‑8 bytes
      - integers encoded in minimal CBOR form
      - floats encoded as float64 when present
        - IMPORTANT: preserve numeric types for signing (a CBOR float must stay a CBOR float in the signing input,
          even if numerically integral like `87.0`)
        - Recommendation: avoid floats in signed envelopes when possible; prefer integers/fixed-point on MCUs
      - embedded helper: `agent_core` provides a minimal deterministic CBOR writer (`agent/cbor_det.h`) for MCU bring-up
- Operator controls:
  - `edge_auth.required` is surfaced via `GET /api/v1/config`.
  - `POST /api/v1/config/update` supports:
    - `edge_auth_required: true|false`
    - `edge_auth_require_ts: true|false` (when true, requires `ts_utc_ms > 0` on authenticated envelopes)
    - `edge_auth_max_skew_ms: <int>` (when > 0, rejects authenticated envelopes if `abs(now-ts_utc_ms)` exceeds this window)
    - `edge_auth_require_seq: true|false` (when true, requires monotonic `auth.seq` on authenticated envelopes)
    - `edge_auth_kid_policy: "any"|"match_node"|"node_prefix"` (best-effort binding between `from:"node:<id>"` and `auth.kid`)
    - `edge_auth_hmac_keys: { "<kid>": "<secret>", "<kid2>": null }` (null clears)
    - `edge_auth_ed25519_pubkeys: { "<kid>": "<base64(pubkey32)>", "<kid2>": null }` (null clears)
    - `edge_attest_required: true|false` (when true, invoke-mode `TASK_DONE` must include `result.attest` with a matching `result_sha256`)
    - `edge_attest_require_sig: true|false` (when true, invoke-mode `TASK_DONE` attestation signatures must verify)
  - Behavior:
    - If `edge_auth_required=true`: missing/invalid `auth` is rejected with HTTP 401 (no inbox persistence).
    - If `edge_auth_required=false`: unsigned envelopes are accepted, but if `auth` is present it must verify.

Task-loop correctness note (recommended, enforced by platform for known `TASK_*` types):
- Nodes SHOULD echo `idempotency_key` for all task lifecycle messages (`TASK_ACK/TASK_EVENT/TASK_DONE/TASK_FAILED`),
  matching the `TASK_ASSIGN.body.idempotency_key` they received. The platform rejects missing/invalid `idempotency_key`
  to prevent cross-attempt/cross-task corruption under retries.

Reliability note (important):
- The platform *persists* all inbound envelopes and dedupes persistence by `msg_id`.
- If a duplicate `msg_id` is received:
  - if the prior message was already processed, the platform returns early (dedupe) and does not re-apply side effects.
  - if the daemon crashed after persisting the inbox row but before applying side effects, the platform may reprocess the message
    (at-least-once) to avoid permanent drops.
- replay safety depends on message type: node-initiated handoffs are designed to be idempotent (`workflow_id` / `idempotency_key`),
    while event-style messages may produce duplicate logs if a crash occurred after side effects but before the processed marker was written.

Attestation note (v0.2/v0.3; enforceable via policy):
- If a node includes `body.result.attest`, the platform persists that blob under `edge_tasks.attest_json` and **excludes**
  `attest` from the `edge_tasks.result_sha256` hash surface to avoid self-referential hashing.
- Best-effort: if `attest` includes `{kid,alg,sig,ts_utc_ms,result_sha256}`, the platform verifies the signature when possible and
  emits evidence under task events (visible via `GET /api/v1/trace?trace_id=...`).
- When `edge_attest_required=true`, invoke-mode `TASK_DONE` must include `result.attest` with a valid `result_sha256` (matching
  the platform-computed hash). When `edge_attest_require_sig=true`, the attestation signature must also verify using the configured keys.

### Poll node outbox

`GET /api/v1/edge/outbox?node_id=...&cursor=0&limit=256`

Wire encoding:
- Default: JSON (`application/json`).
- Optional: CBOR (`Accept: application/cbor`) returns a CBOR-encoded response body with the same JSON-shaped fields.

Returns messages in ascending `outbox_id` order. The node should:
- persist its last `cursor_next`
- re-poll with `cursor=<cursor_next>`

### Debug registry helpers

- `GET /api/v1/edge/nodes`
- `GET /api/v1/edge/node?node_id=...`
- `GET /api/v1/edge/node/caps?node_id=...`

### Platform helper: enqueue TASK_ASSIGN

`POST /api/v1/edge/task/assign`

Creates (or dedupes) an `edge_tasks` row and enqueues a UM‑BMP `TASK_ASSIGN` to the target node outbox.

Targeting:
- explicit: `node_id`
- capability routing: `match_any { requires_tools, tags_all, tags_any, tags_none }`

Trace correlation (best-effort):
- The platform accepts an optional `trace` object on `POST /api/v1/edge/task/assign` and forwards it to the node as
  `TASK_ASSIGN.trace` (envelope-level field). Recommended key: `trace.trace_id`.

Safety/rate gates (best-effort, platform-side):
- For `mode:"invoke"`, the platform requires a stored node manifest (`NODE_CAPS_RSP`) so it can inspect tool metadata.
- For `mode:"invoke"`, the platform requires `payload.args` to be an object, and validates it (best-effort) against the
  tool argument schema from the stored manifest when present:
  - preferred: `manifest.tools[].parameters_schema`
  - fallback: `manifest.tools[].parameters`
  The platform enforces a small, deterministic subset of JSON Schema keywords (`type`, `enum`, `required`,
  `properties`, `additionalProperties:false`, `items`) to catch shape mismatches early for MCU/actuator tools.
- For `mode:"invoke"`, if the tool definition includes a `resource_lock` key, the platform blocks parallel dispatch
  of another `TASK_ASSIGN` to the same node using the same lock while an existing task is `QUEUED` or `RUNNING`.
  The platform returns HTTP 429 (`resource_locked`) so orchestrators can backoff/retry.
- For `mode:"invoke"`, if the stored manifest tool definition includes a `result_schema`, the platform validates
  `TASK_DONE.body.result.data` against it (best-effort subset, fail-closed). This prevents malformed tool outputs from
  flowing into workflows and memory.
- Denies tools tagged with hazard `privacy_camera` by default (unless explicitly allowed via request).
- Denies `side_effect_level:"high"` by default (unless explicitly allowed via request).
- Enforces per-tool `rate_limit` from the manifest (`max_per_minute`, `cooldown_ms`) using platform-side state.

### Task status

`GET /api/v1/edge/task?task_id=...&step_id=...`

### Automation rules (SENSOR_EVENT → actions)

- `POST /api/v1/edge/rule/upsert`
- `GET /api/v1/edge/rules`
- `DELETE /api/v1/edge/rule?rule_id=...`

Rules are evaluated during `SENSOR_EVENT` ingestion. Supported actions:
- `type:"task_assign"`: enqueue a `TASK_ASSIGN` (invoke or agent) to a node (same behavior as `POST /api/v1/edge/task/assign`)
- `type:"durable_workflow_submit"`: submit a durable workflow to the platform workflow engine

For both action types, the platform runs the same cooldown gate: it fires when:
- `event_type` matches
- `confidence >= min_confidence`
- `cooldown_ms` has elapsed since `last_fired_utc_ms`

Durable workflow action notes:
- If `action.workflow.inputs.sensor_event` is missing, the platform injects it (best-effort) so workflow tasks can template against
  `${input.sensor_event.json:/...}`.

### Durable edge workflows (UM‑WF)

- `POST /api/v1/edge/workflow/submit`
- `POST /api/v1/edge/workflow/cancel`
- `GET /api/v1/edge/workflow?workflow_id=...&include_steps=1`
- `GET /api/v1/edge/workflows?status=...&limit=...`
- `GET /api/v1/edge/workflow/events?workflow_id=...&cursor=0&limit=256`
- `GET /api/v1/edge/workflow/stream?workflow_id=...&cursor=0` (SSE)

Workflows are executed by a background runner in `agentd`:
- dispatches `invoke_tool`/`run_agent` steps via `TASK_ASSIGN`
- supports `depends_on` sequencing, parallel fan-out, and `join` (`all|any`)

## Quick smoke flow (single node)

1) Node sends `NODE_HELLO` → platform queues `PLATFORM_CAPS_REQ`
2) Node polls outbox, receives caps req, responds with `NODE_CAPS_RSP`
3) Platform enqueues `TASK_ASSIGN` (`mode:"invoke"`) to call a device tool
4) Node replies with `TASK_ACK` + `TASK_EVENT` + `TASK_DONE`

Proof:
- `ctest` includes `agentd_edge_interop_smoke` and `agentd_edge_workflow_submit_message_smoke`.
- `ctest` includes `agentd_edge_auth_hmac_smoke`.
- `ctest` includes `agentd_edge_auth_ed25519_smoke`.

## Storage

DB tables are documented in `docs/DB.md`:
- `edge_nodes`
- `edge_inbox_messages`
- `edge_outbox_messages`
- `edge_tasks`
- `edge_task_events`
- `edge_sensor_events`
- `edge_tool_rate_state`
- `edge_rules`
- `edge_workflows`
- `edge_workflow_steps`
- `edge_workflow_events`
---

# Source: docs/EMBEDDED_C_API.md

# Embedded C Integration (ESP32-S3)

This repo contains a **portable, pure-C core library** (`agent_core`) intended to run on embedded targets (including ESP32-class MCUs) as long as you provide:

- a **tool-capable LLM provider** (`agent_tool_provider_t`) that can produce tool calls and assistant text
- a **tool executor** (`agent_tool_executor_t`) that runs your device tools (GPIO, sensors, display, etc.)
- (optionally) a custom allocator via `agent_set_allocator()` to control memory

The core library **does not** perform HTTP/JSON parsing for you on embedded. That is deliberate: networking and JSON stacks vary wildly across embedded environments.

For a maturity/feasibility assessment and architecture options (remote agent vs on-device), see:
- `docs/ESP32S3_AGENT_CORE_MATURITY.md`

## Build: core-only (no host deps)

To build only the embedded-safe core (no libcurl / jsoncpp / daemon / UI):

```sh
cmake -S . -B build-core -DAGENT_BUILD_HOST=OFF
cmake --build build-core -j
```

This produces the `agent_core` static library.

## Memory model

All heap allocations inside the core go through `agent_malloc()` / `agent_free()`.
You can override these globally at process startup:

```c
#include "agent/agent.h"

static void* my_malloc(size_t n) { /* arena/heap */ }
static void my_free(void* p) { /* arena/heap */ }

void app_init_allocator(void) {
  agent_allocator_t a = { .malloc_fn = my_malloc, .free_fn = my_free };
  (void)agent_set_allocator(&a);
}
```

On ESP32-S3, a common pattern is:
- one arena allocator for the agent task (fast reset between runs)
- a small fallback heap allocator for long-lived objects you keep across runs

## Core data types (what you need to implement)

### 1) Tool registry

Tools are declared as OpenAI-compatible JSON schema blobs, but the core treats them as opaque strings.

```c
#include "agent/tools.h"

agent_tool_registry_t* tools = NULL;
agent_tool_registry_create(&tools);
agent_tool_registry_add(tools, "gpio_write", "Set a GPIO pin high/low",
  "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"value\"]}"
);
```

### 2) Tool executor (runs on-device tools)

The executor must return an `agent_string_t` result. The simplest way is to build the output using `agent_string_set_copy()` (which allocates via the active allocator):

```c
#include "agent/tools.h"

static agent_status_t exec_tool(
  void* ctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  (void)ctx;
  // Parse arguments_json (your JSON lib) and do work...
  // Return a compact result; smaller is cheaper in tokens.
  const char* ok = "{\"ok\":true}";
  return agent_string_set_copy(out_result, ok, strlen(ok));
}

static agent_tool_executor_t make_executor(void) {
  agent_tool_executor_t ex = {0};
  ex.ctx = NULL;
  ex.execute = exec_tool;
  return ex;
}
```

### 2b) Embedded plugin list helper (compile-time tools)

If you prefer a static, compile-time tool list, use the embedded plugin helper:

```c
#include "agent/tool_plugin_embedded.h"

static agent_status_t exec_gpio(
  void* ctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  (void)ctx;
  (void)tool_name;
  // Parse arguments_json and run GPIO...
  return agent_string_set_copy(out_result, "{\"ok\":true}", strlen("{\"ok\":true}"));
}

static const agent_tool_plugin_v0_def_t defs[] = {
  {
    .name = "gpio_write",
    .description = "Set a GPIO pin high/low",
    .parameters_json = "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"value\"]}",
    .execute = exec_gpio,
    .ctx = NULL,
  },
};

static const agent_tool_plugin_v0_t plugin = {
  .defs = defs,
  .def_count = sizeof(defs) / sizeof(defs[0]),
};

// Register tool schemas and initialize the executor:
agent_tool_registry_add_plugin(tools, &plugin);
agent_tool_plugin_executor_ctx_t exec_ctx = {0};
agent_tool_executor_t exec = {0};
agent_tool_plugin_executor_init(&exec_ctx, &plugin, 1, &exec);
```

### 3) Tool-capable LLM provider (your network / model adapter)

The provider is responsible for calling your LLM and returning:
- assistant text (`assistant_content`)
- optional tool calls (`tool_calls[]`)

Core does not ship an embedded HTTP client. Implement `agent_tool_provider_generate_fn` using whatever stack you have:

```c
#include "agent/tool_provider.h"

static agent_status_t my_generate(
  void* provider_ctx,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  (void)provider_ctx;
  // req->messages: includes tool_call_id/tool_calls metadata (OpenAI-compatible shape)
  // req->tools: tool registry
  // req->model: model hint (you may ignore)
  //
  // 1) Serialize req into your LLM request
  // 2) Send request to your LLM
  // 3) Parse response:
  //    - assistant content string
  //    - optional tool call list (name + arguments_json + optional id)
  //
  // Fill out_resp using agent_string_set_copy and agent_malloc allocations.
  //
  // Placeholder:
  const char* txt = "Hello from embedded provider";
  (void)agent_string_set_copy(&out_resp->assistant_content, txt, strlen(txt));
  out_resp->tool_calls = NULL;
  out_resp->tool_call_count = 0;
  return AGENT_OK;
}

static agent_tool_provider_t make_provider(void) {
  agent_tool_provider_t p = {0};
  p.ctx = NULL;
  p.generate = my_generate;
  return p;
}
```

## Multimodal inputs (image + text)

The core can store multimodal message “parts” (text + image URL / binary image bytes) via `agent/parts.h`.

Important constraints:
- Parts are currently **in-memory only** (the portable session codec v1 stores role+text only).
- The **tool loop** (`agent_tool_loop_run`) passes messages to `agent_tool_provider_t` as text strings; it does not currently
  surface parts to the tool-provider interface.
- For **non-tool runs** (pure chat), `agent_run_once` can pass the session pointer into the provider via an extended request:
  implement `agent_provider_t.generate_ex` (see `agent/provider.h`). The core prefers `generate_ex` when present.

Minimal example (non-tool run, with an image URL part):

```c
#include "agent/agent.h"
#include "agent/parts.h"
#include "agent/provider.h"
#include "agent/runner.h"

agent_session_t* s = NULL;
agent_session_create(&s);

agent_content_part_t parts[2] = {0};
parts[0].type = AGENT_PART_TEXT;
parts[0].text_or_null = "Describe this image.";
parts[1].type = AGENT_PART_IMAGE_URL;
parts[1].url_or_null = "https://example.com/image.png";
agent_session_add_message_parts(s, AGENT_ROLE_USER, parts, 2);

// provider.generate_ex must translate session parts into the provider's multimodal request format.
```

## Running the core tool loop

The `agent_tool_loop_run()` function orchestrates:
- transcript compaction
- provider calls
- tool execution
- tool result injection back into the transcript

Minimal invocation:

```c
#include "agent/tool_loop.h"

agent_tool_loop_result_t r = {0};
agent_tool_loop_options_t opt = {0};
opt.model = "your-model-id";
opt.max_steps = 8;
opt.max_chars = 8000;
opt.keep_last_messages = 12;
opt.max_tool_result_chars = 2000;
opt.max_tool_call_args_chars = 0; // default (0 disables)
opt.verbose_events = 0;
opt.max_capture_chars = 0;        // default (only used when events enabled)
opt.disable_tool_records = 1;     // recommended for embedded

agent_tool_loop_hooks_t hooks = {0}; // leave all NULL for minimal allocations

agent_status_t st = agent_tool_loop_run(
  &provider,
  tools,
  &executor,
  seed_session,
  user_prompt,
  &opt,
  &hooks,
  &r
);

// Use r.final_assistant_text; then free:
agent_tool_loop_result_free(&r);
```

Notes:
- For embedded, **keep hooks NULL** unless you truly need event streaming; otherwise you pay for JSON event construction.
- `disable_tool_records=1` avoids allocating duplicated tool transcript copies in the final result.
- Always zero-init `agent_tool_loop_options_t` / `agent_tool_loop_result_t` (`{0}`) so new fields have safe defaults.

## Minimizing token footprint (practical knobs)

These settings directly reduce prompt size and tool-loop amplification:

- `opt.max_tool_result_chars`: keep tool outputs compact before re-inserting into the LLM transcript
- `opt.max_chars` + `opt.keep_last_messages`: bounds the transcript size and compaction work
- `opt.max_steps` / `opt.max_tool_calls_total` / `opt.max_tool_calls_per_tool` / `opt.max_tool_call_args_chars`: hard limits to prevent runaway loops
- Tool schemas: keep JSON schema small; avoid large descriptions; prefer enums over long free-form fields

If you need richer tool outputs for debugging, return structured *but compact* JSON, e.g.:
`{"ok":true,"v":123}` not multi-kilobyte pretty-printed blobs.

## ESP32-S3 architecture recommendation

On ESP32-S3, treat the core tool loop as a **single task**:

- Inputs: prompt + current session snapshot + tool registry
- Outputs: final assistant text (+ optionally a short event stream)
- Side effects: tools run via `agent_tool_executor_t` only

Keep all persistence (flash/SD/network) and UI outside the core library. This keeps the C API portable and minimizes binary size.
---

# Source: docs/ESP32S3_AGENT_CORE_MATURITY.md

# ESP32-S3 Readiness: `agent_core` vs `agentd` (Maturity + Design)

Date: 2026-02-01

This document assesses whether the project can run “agent + tool use” on an **ESP32-S3-class MCU**,
and what to build next so the device can control attached peripherals from user prompts.

## Executive summary (fact-based)

### What is realistic on ESP32-S3 today

**Use `agent_core` (pure C library), not `agentd`.**

Facts from the repo:
- `agent_core` is **C11**, portable, and is explicitly documented for ESP32-S3 in `docs/EMBEDDED_C_API.md`.
- `agentd` / `agentd_lib` is **C++17**, depends on **libcurl + jsoncpp**, and typically **sqlite3** + filesystem.
  - Those are desktop/server dependencies and are not a practical fit for ESP-IDF builds.

So the maturity assessment is split into:
- **On-device agent (ESP32-S3)**: `agent_core` + device tools + an embedded provider adapter.
- **Desktop/server daemon**: `agentd` (sidecar mode, broker mode) for richer environments.

### “Maturity” rating (what you can build confidently)

| Layer | Target | Current maturity | Why |
|---|---|---:|---|
| `agent_core` (tool loop + session) | ESP32-S3 | **Good** | Pure C, custom allocator, hard safety limits; no HTTP/JSON assumptions. Core-only build + tests pass. |
| Provider adapter (HTTP/TLS + JSON parsing) | ESP32-S3 | **Missing (host responsibility)** | Not shipped by design; you must implement with ESP-IDF networking + a JSON library. |
| Tool executor (peripherals) | ESP32-S3 | **Project-specific** | Framework exists (`agent_tool_executor_t`), but you must implement tools + validation. |
| Persistence (NVS/flash/FS) | ESP32-S3 | **Moderate** | Core provides session codec + persistor interface; host must implement NVS/FS. |
| `agentd` / `agentd_lib` | ESP32-S3 | **Not a target** | C++17 + curl/jsoncpp/sqlite/filesystem/threading; intended for desktop/server. |

## Constraints you must design for (ESP32-S3-class MCU)

These are not repo-specific; they are practical MCU constraints that shape the integration:

- **RAM is limited** (especially if you don’t have PSRAM). TLS + JSON parsing can be memory hungry.
- **Flash is limited**. Pulling in heavy formatting / JSON libs increases binary size.
- **Network is lossy**. Provider calls need retries + timeouts and should support cancellation.
- **Real-time peripherals** must not be blocked by the LLM loop. Run the agent in its own FreeRTOS task.

To make this concrete for your board, we should pin:
- whether your ESP32-S3 build has PSRAM enabled and how much
- how you plan to connect to the LLM (Wi‑Fi direct, gateway, cellular modem, etc.)
- whether the device must work offline (local model) or can be cloud-dependent

## Dependency mapping: `agentd` vs `agent_core`

### `agent_core` (ESP32-S3 friendly)

Observed dependencies (from `core/src` + headers):
- Language: C11
- libc headers: `string.h`, `ctype.h`, `stdint.h`, `stddef.h`
- heap: `agent_malloc/agent_free` (overridable via `agent_set_allocator`)
- no filesystem, no sockets, no threads required by the core

Notes:
- `agent_core` includes an embedded-friendly persistence codec: `core/include/agent/session_codec.h`
- multimodal “parts” exist, but persistence codec v1 stores **role + content only** (no parts yet).
  - For non-tool runs, providers can access parts via `agent_provider_t.generate_ex` (see `core/include/agent/provider.h` and `core/include/agent/parts.h`).

### `agentd` / `agentd_lib` (desktop/server)

Observed characteristics:
- Language: C++17
- Depends on `agent_host`, which requires:
  - libcurl
  - jsoncpp
- Daemon layer adds:
  - sqlite3 (when enabled)
  - filesystem usage
  - optional HTTP server thread (when `AGENTD_ENABLE_HTTP=ON`)

This is appropriate for a workstation/edge gateway, not for an MCU build.

## Architectures for “prompt → tool use → peripherals”

There are two viable patterns. Pick based on your network/security requirements.

### Option A (recommended): Remote agent, MCU is a tool endpoint

Run `agentd` on a gateway (desktop/server/RPi). The ESP32-S3 exposes a **small device RPC** interface:
- gateway sends “execute tool” requests
- MCU executes peripheral operation
- MCU replies with result JSON

Pros:
- MCU does not need to implement tool-calling LLM protocol, TLS quirks, or big JSON parsing.
- You keep secrets (LLM API keys) off the device.
- You can ship updates faster on the gateway.

Cons:
- Requires gateway connectivity.
- Requires an additional “device RPC” protocol (MQTT/HTTP/serial/custom).

This repo already has a broker concept (`docs/BROKER.md`) for routing requests to agents behind NAT; a similar pattern
works for routing tool calls to devices.

### Option B: On-device agent (`agent_core` runs on ESP32-S3)

The ESP32-S3 runs:
- `agent_core` tool loop
- an embedded provider adapter (HTTP/TLS + JSON parsing) that returns assistant text + tool calls
- a tool executor that controls peripherals
- optional persistor (NVS) for sessions

Pros:
- Device can accept prompts directly and act autonomously.
- No gateway required (beyond internet access to the LLM).

Cons:
- Harder engineering on MCU: TLS + JSON parsing + retries + time sync.
- Higher risk surface: device holds more “decision power”.

## On-device design (Option B): exact API boundaries

### 1) Tool registry (compile-time)

You register tools using:
- `agent_tool_registry_add(name, description, parameters_json)`

On MCU, keep schemas **small** and **strict**:
- prefer enums, bounded ints, required fields
- avoid large free-form strings where possible

### 2) Tool executor (peripherals)

You implement:
- `agent_tool_executor_t.execute(ctx, tool_name, arguments_json, out_result)`

Hard requirements for safety:
- validate `tool_name` is in an allowlist
- validate arguments: type, range, units, and *side effects*
- make tool operations idempotent when possible
- keep tool results compact JSON (token + RAM friendly)

### 3) Tool-capable provider (LLM adapter)

You implement:
- `agent_tool_provider_t.generate(ctx, req, out_resp)`

Provider responsibilities:
- serialize `req->messages` + tool schemas into your LLM API request
- call LLM (with timeouts + retries)
- parse response into:
  - `out_resp->assistant_content`
  - `out_resp->tool_calls[]` (name + arguments_json [+ id])

Core requirement:
- any allocations stored into `out_resp` must be compatible with `agent_free` (use `agent_malloc` / `agent_string_set_copy`).

### 4) Persistence (optional but usually needed)

For persistence, implement `agent_persistor_t` against:
- NVS (key-value): store `agent_session_codec_encode_v1` output per session id
- or a filesystem (SPIFFS/LittleFS) storing the same codec text

## Safety limits you should enable on MCU

The core already provides hard stops; on-device you should turn them on by default:

- `max_steps` (small default like 4–8)
- `max_tool_calls_total` (caps “multi tool calls in one step” runaway)
- `max_repeated_tool_calls` (prevents “repeat the same call forever”)
- `max_tool_result_chars` (prevents huge tool outputs from exploding tokens)
- `disable_tool_records=1` (reduces allocations)

Additionally, implement **tool-level** safety:
- per-tool call limits (`tool_call_limits`) for dangerous/expensive tools (motors, relays, OTA, etc.)
- “arming” or confirmation flow for irreversible actions

## Work items (what to implement next)

### Phase 0: Packaging + build proof

- Create an ESP-IDF component wrapper for `agent_core` (no provider included).
- Build a “hello tool” example that runs without Wi‑Fi (fake provider that returns a fixed tool call).
- Add a **host-side simulator** that runs `agent_core` with the same tool registry + executor, but logs everything for rapid iteration.
  - In this repo: the `esp32sim` harness (`tools/esp32sim.cpp`) builds on desktop and writes a JSONL log for inspection.

Exit criteria:
- `agent_core` compiles under ESP-IDF toolchain
- tool executor controls at least one peripheral (GPIO toggle) from a tool call

### Phase 1: Provider adapter

- Implement an ESP-IDF provider adapter (HTTP client + TLS + JSON parsing).
- Support tool calling response parsing for your chosen backend.

Exit criteria:
- end-to-end: prompt → LLM → tool call → GPIO result → assistant conclusion
- robust timeouts + retry + cancellation

### Phase 2: Session + Scene strategy

Decide:
- whether the MCU needs a “Scene” concept locally (often unnecessary; Scene is mainly for rich clients)
- how sessions are persisted (NVS vs FS vs remote)

Exit criteria:
- device can resume last session after reboot

## Notes on “Scene” for MCUs

The durable Scene described in `docs/CLIENT.md` is a **daemon-side** persistence feature (DB-backed).
On MCU, it is usually better to:
- represent UI state in the client (mobile/web)
- keep the device focused on **actuation + sensing tools**

If you do need a local scene, implement it as a thin mapping of entity ids to props in RAM, not as a DB feature.
---

# Source: docs/LIMITS.md

# Run Limits & Runaway-Loop Guards

Date: 2026-02-19

This document specifies how the agent tool loop should behave when configured safety limits are reached.

It exists because:
- In tool mode, the model may keep producing tool calls indefinitely (e.g. repeated camera captures).
- Without explicit limits, the daemon can appear “stuck” and can spam host resources (camera, disk, CPU).
- Limits must fail **loudly** (structured error event + non-OK status) so UIs/operators can diagnose the stop reason.

## Goals

- Provide a bounded, operator-configurable safety envelope for tool loops.
- Ensure hitting a limit:
  - returns a non-OK status (`AGENT_ERR_LIMIT`)
  - includes a human-readable error message
  - emits a structured `error` event with the stop reason and relevant counters
- Preserve explicit opt-out for advanced users (e.g. `max_steps=0` means unlimited).

## Additional goals

- Improve task-complete inference with explicit done criteria and optional semantic checks.
- Support provider-specific policy controls surfaced through hosts and UIs.

## Core semantics

### `max_steps`

- Definition: maximum number of tool-loop *steps* (provider calls), where each step can contain 0+ tool calls.
- `max_steps = 0` means unlimited.
- If the loop would continue producing tool calls beyond the limit, the core must:
  - emit an `error` event with:
    - `reason = "max_steps_exceeded"`
    - `steps_executed`
    - `max_steps`
  - set error message like: `"max steps exceeded"`
  - return `AGENT_ERR_LIMIT`

### `max_repeated_tool_calls`

- Definition: abort if the exact same tool call (same tool name + exact `arguments_json`) repeats too many times consecutively.
- `max_repeated_tool_calls = 0` disables the guard.
- When triggered, the core must:
  - emit an `error` event with:
    - `reason = "repeated_tool_call_guard"`
    - `tool_name`
    - `repeats`
    - `max_repeats`
  - return `AGENT_ERR_LIMIT`

### `max_tool_calls_total`

- Definition: maximum number of tool calls executed in total (across all steps).
  - This is distinct from `max_steps`: a single step can contain **multiple** tool calls in a provider response.
- `max_tool_calls_total = 0` means unlimited.
- When triggered, the core must:
  - emit an `error` event with:
    - `reason = "max_tool_calls_exceeded"`
    - `tool_calls_executed`
    - `max_tool_calls_total`
  - return `AGENT_ERR_LIMIT`

### `max_tool_calls_per_tool`

- Definition: maximum number of tool calls executed for a given tool name across the entire run.
  - This catches “same tool, varying args” loops that bypass the exact-call repetition guard.
- `max_tool_calls_per_tool = 0` means unlimited.
- When triggered, the core must:
  - emit an `error` event with:
    - `reason = "max_tool_calls_for_tool_exceeded"`
    - `tool_name`
    - `tool_calls_for_tool`
    - `max_tool_calls_for_tool` (same numeric value as `max_tool_calls_per_tool`)
    - `limit_source = "max_tool_calls_per_tool"`
  - return `AGENT_ERR_LIMIT`

### `max_tool_call_args_chars`

- Definition: maximum number of characters in a single tool call `arguments_json` (best-effort).
- `max_tool_call_args_chars = 0` means unlimited.
- When triggered, the core must:
  - emit an `error` event with:
    - `reason = "max_tool_call_args_chars_exceeded"`
    - `max_tool_call_args_chars`
    - `tool_name` (best-effort)
    - `tool_call_args_chars` (best-effort length)
  - return `AGENT_ERR_LIMIT`

### `max_tool_result_chars`

- Definition: cap tool outputs before they are re-inserted into the prompt context (best-effort).
- `max_tool_result_chars = 0` means unlimited.
- When triggered, the core truncates the tool output for prompt use and sets `result_truncated_for_prompt=true` in tool records
  (and may emit truncated content in verbose events). No hard error is returned.

### `tool_call_limits` (per-tool map)

- Definition: an explicit per-tool call limit map applied across the entire run.
  - Each entry is `(tool_name, max_calls)`.
  - This is more precise than `max_tool_calls_per_tool` and avoids breaking common workflows that legitimately call
    low-risk tools many times (e.g. `fs_read`) while still bounding high-risk tools (`proc_exec`, `shell_exec`).
- Semantics:
  - If a tool name has an explicit entry in `tool_call_limits`:
    - `max_calls = 0` means unlimited for that tool (explicit disable).
    - Otherwise `max_calls` is enforced for that tool name.
  - If a tool name does not have an explicit entry:
    - fall back to `max_tool_calls_per_tool` (if non-zero)
    - otherwise unlimited for that tool name.
- When triggered, the core must:
  - emit an `error` event with:
    - `reason = "max_tool_calls_for_tool_exceeded"`
    - `tool_name`
    - `tool_calls_for_tool`
    - `max_tool_calls_for_tool`
    - `limit_source = "tool_call_limits" | "max_tool_calls_per_tool"`
  - return `AGENT_ERR_LIMIT`

## Host/daemon defaults

### Daemon default `max_steps`

To protect long-running `agentd` instances, the daemon should apply a default `max_steps` for requests that omit it.

- Proposed default: `32` steps.
- The UI should allow:
  - blank / unset `max_steps` → daemon default applies
  - explicit `0` → unlimited (operator accepts risk)

This keeps new users safe while still allowing intentional long runs.

### Daemon default `max_tool_calls_total`

To protect against a single model response that requests many tool calls at once, `agentd` should apply a default cap on total
tool calls when requests omit it.

- Proposed default: `128` tool calls.
- The UI should allow blank/unset to use daemon defaults and explicit `0` to disable.

### Daemon default `max_tool_result_chars`

To keep prompts from being flooded by large tool outputs, the daemon should apply a default cap on tool results
before they are re-inserted into the LLM context.

- Suggested default: `12000` chars.
- The UI should allow blank/unset to use daemon defaults and explicit `0` to disable.

### Daemon default `tool_call_limits`

For long-running daemons, it is often better to bound the most dangerous/high-noise tools directly, rather than applying
a global per-tool cap that can break benign workflows.

Recommended defaults (can be tuned per operator):
- `proc_exec=4` (prevents runaway capture attempts or repeated subprocess loops)
- `shell_exec=16` (still allows normal build/test flows; encourages batching multiple commands in one call)
- `artifact_register=16` (prevents runaway artifact spamming)
- `ui_action=16` (prevents runaway UI action spamming)

## Related docs

- `docs/PROTOCOL.md` (run request fields)
- `README.md` (operator-facing summary)
---

# Source: docs/MACOS_PACKAGING.md

# macOS Packaging, Codesign, and Notarization (agentd/agent)

Date: 2026-02-14

This guide covers a **production-grade** packaging pipeline for macOS (Apple Silicon, M2). It produces a signed `.pkg`
containing `agentd` (and optionally `agent`) installed under `/usr/local/bin/`.

## Goals

- Ship a signed, notarized, and stapled installer (`.pkg`).
- Keep a repeatable, scriptable workflow for CI or local release builds.
- Separate build, signing, packaging, and notarization steps for clarity and auditability.

## Preconditions

- Xcode Command Line Tools installed.
- Apple Developer ID certificates:
  - **Developer ID Application** (for signing binaries)
  - **Developer ID Installer** (for signing the `.pkg`)
- Keychain profile configured for `notarytool` (recommended):
  - `xcrun notarytool store-credentials --apple-id <id> --team-id <team> --password <app-specific-password> <profile_name>`

## Required Identifiers

You need stable identifiers for notarization and update tracking:

- **Bundle/Package identifier** (e.g. `com.agentd.pkg`)
- **Version** (e.g. `0.1.0` or `0.1.0+<git>`) — keep stable across artifacts

## Quick start (scripted)

Use the helper script (requires signing identities):

```bash
CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
PKG_SIGN_IDENTITY="Developer ID Installer: Your Name (TEAMID)" \
NOTARY_PROFILE="your-notary-profile" \
tools/macos_package.sh
```

Artifacts are written under `out/macos_pkg_<timestamp>/` (including `SHA256SUMS.txt`).

## Manual steps (reference)

### 1) Build

```bash
cmake -S . -B build
cmake --build build -j
```

### 2) Stage binaries

```bash
mkdir -p stage/usr/local/bin
install -m 0755 build/agentd stage/usr/local/bin/agentd
install -m 0755 build/agent stage/usr/local/bin/agent
```

### 3) Codesign binaries (required for notarization)

```bash
codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" \
  stage/usr/local/bin/agentd

codesign --force --options runtime --timestamp \
  --sign "Developer ID Application: Your Name (TEAMID)" \
  stage/usr/local/bin/agent
```

Verify:

```bash
codesign --verify --strict --deep stage/usr/local/bin/agentd
codesign --verify --strict --deep stage/usr/local/bin/agent
```

### 4) Build and sign the `.pkg`

```bash
pkgbuild \
  --root stage \
  --identifier com.agentd.pkg \
  --version 0.1.0 \
  --install-location / \
  unsigned.pkg

productsign \
  --sign "Developer ID Installer: Your Name (TEAMID)" \
  unsigned.pkg signed.pkg
```

### 5) Notarize and staple

```bash
xcrun notarytool submit signed.pkg --keychain-profile your-notary-profile --wait
xcrun stapler staple signed.pkg
```

Verify notarization:

```bash
xcrun stapler validate signed.pkg
```

## Operational notes

- `agentd` is not a GUI app; packaging as a `.pkg` is the most reliable distribution path.
- Use launchd for service management (see `tools/install_agentd_launchd.sh`).
- Keep the installer payload minimal: binaries + optional docs. Do not embed secrets.
- If you ship `agentd` to non-localhost targets, use `--auth-token` and CORS allowlists.

## Recommended environment variables

The helper script understands:

- `CODESIGN_IDENTITY` — Developer ID Application identity (required for notarization).
- `PKG_SIGN_IDENTITY` — Developer ID Installer identity (required to sign `.pkg`).
- `NOTARY_PROFILE` — notarytool keychain profile name (optional; enables notarization).
- `AGENTD_BIN`, `AGENT_BIN` — override binary paths.
- `MACOS_PKG_ID` — package identifier (default `com.agentd.pkg`).
- `MACOS_PKG_VERSION` — package version (default based on git + date).
- `MACOS_PKG_NAME` — output base name (default `agentd`).
- `MACOS_PKG_OUT_DIR` — output directory (default `out/macos_pkg_<timestamp>`).
- `MACOS_PKG_INSTALL_PREFIX` — install prefix (default `/usr/local/bin`).

## Security and auditability

- Never store credentials in the repo.
- Keep `NOTARY_PROFILE` in the login keychain or CI secrets store.
- Record the package checksum (SHA256) at release time.
- Retain signing logs for traceability.
---

# Source: docs/MEMORY.md

# Memory (Durable + Searchable)

`agentd` (and the `agent` CLI when connected to `agentd` state) uses a **durable memory directory** under the daemon `state_dir`:

- `state_dir/memory/MEMORY.md` — core “always true” memory (facts/preferences/tasks)
- `state_dir/memory/YYYY-MM-DD.md` — daily append-only log (bounded scan by days)
- `state_dir/memory/sessions/<session_id>.md` — optional session layer
- `state_dir/memory/STRUCTURED.md` — machine-maintained structured memory (via `memory_put(entries)`)
- `state_dir/memory/checkpoints/structured_<ts>.json` — time-stamped structured snapshots (best-effort, rolling)
  - each checkpoint has a deterministic `sha256` surface (exposed by APIs/tools)
- `state_dir/memory/recaps/recap_<ts>.json` — optional LLM-generated recap snapshots

## Tools

These are host tools exposed to the model when a daemon session context is available:

- `memory_write` — append a note (default: `layer="daily"`)
- `memory_observe` — append a structured observation (daily log; citations + tags)
- `memory_get` — read a memory file by path (paged by lines)
- `memory_search` — retrieve relevant snippets (prefer ranked search when available)
- `memory_timeline` — retrieve bounded context around a citation (`path:line`)
- `memory_put` — consolidate/overwrite memory files (legacy mode), or structured upsert (`entries`)

## Memory context injection (runs)

When `tools="host"`, agentd can inject a **durable memory context** as a system message before the run:

- File mode (default): reads Markdown memory files and injects a concise snapshot.
- Search mode: injects **ranked snippets** (claude-mem style) for the current prompt.

Run request knobs (file mode + search mode share the same memory root):

- `memory_context_mode`: `"files"` (default), `"search"`, `"index"` (progressive disclosure; `"progressive"` alias supported), or `"salience"`.
- `memory_include_structured`, `memory_include_core`, `memory_include_daily`, `memory_include_session`
- `memory_daily_days` (clamped), `memory_total_cap` (clamped)
- `memory_search_query` (defaults to prompt when omitted)
- `memory_search_use_index`, `memory_search_case_sensitive`
- `memory_search_order` (`ranked` | `newest` | `oldest`; default `ranked`)
- `memory_search_max_results`, `memory_search_max_snippet_chars`, `memory_search_context_lines`
- `memory_search_fallback_to_files` (if search yields no hits)

Search mode injects a system message that starts with `DURABLE_MEMORY_SEARCH_CONTEXT` and includes:
- total durable memory bytes + approximate tokens,
- the latest recap timestamp/path (if available),
- a recap hint line (if recaps exist),
- the latest assistant timestamp + assistant hint (if available),
- citations in the form `[tier path:line]`.

This makes search results cost-aware and keeps the model aligned with the latest recap summary.

### Progressive disclosure index mode

`memory_context_mode="index"` injects a lightweight index instead of full content:

```
DURABLE_MEMORY_INDEX
- This is a lightweight index of durable memory files on disk.
- Token estimates are approximate (bytes/4). Use memory_search and memory_get for details.
- Total memory bytes: 25521 (~tokens=6380)
- Latest recap: 2026-02-17T12:10:44Z (recaps/recap_2026-02-17T12-10-44Z.json)
- Recap hint: Ongoing goals + most recent decisions...
- Latest assistant: 2026-02-18T19:23:11Z
- Assistant hint: Confirmed deployment changes and pending follow-ups...

[structured STRUCTURED.md] lines=42 bytes=9101 ~tokens=2276
[core MEMORY.md] lines=18 bytes=2100 ~tokens=525
[daily 2026-02-14.md] lines=120 bytes=14320 ~tokens=3580
```

This mirrors the claude-mem style of “show what exists + cost first,” keeping context lean while still
giving the agent enough signals to fetch the right details on demand.

### Salience mode (dynamic)

`memory_context_mode="salience"` injects a **ranked, compact** memory summary based on
recency + importance:

- Structured memory: latest checkpoint (`memory/checkpoints/structured_*.json`)
- Daily observations: `@obs` blocks (importance-tagged)

The daemon computes a deterministic salience score:

```
score = base_weight * exp(-age_days / half_life_days)
```

Use `/api/v1/memory/salience` to inspect the ranked items and tuning parameters.

Tuning knobs (daemon config / `/api/v1/config/update`):

- `memory.salience_daily_days`
- `memory.salience_max_items`
- `memory.salience_structured_max_items`
- `memory.salience_daily_max_items`
- `memory.salience_half_life_days`
- `memory.salience_importance_weight`

### `memory_search` tiers + citations

`memory_search` results now include:

- `tier`: `core` | `structured` | `session` | `daily` (or `other`)
- `citation`: `${path}:${line}` (stable, human-readable pointer)

Ordering:
- `ranked` (default): preserves FTS5 relevance ordering (or scan order for substring mode).
- `newest` / `oldest`: reorders **daily** results by date/line while leaving other tiers stable.

### 3-step retrieval (claude-mem style)

For large memory stores, use the progressive workflow:

1) `memory_search` to find relevant citations (`path:line`)
2) `memory_timeline` with the citation to get a bounded context window
3) `memory_get` if you need the full file or a larger slice

This keeps context lean while still allowing precise follow-up reads.

For progressive disclosure, set `tiered=true` to group results by tier and get rough token
estimates per tier (`tiers.<tier>.token_estimate`).

### Privacy filtering (`<private>` blocks)

To keep sensitive content out of durable storage, wrap it in `<private>` tags:

```text
User’s card number is <private>4242 4242 4242 4242</private>
```

Behavior:
- `memory_write` strips `<private>...</private>` blocks before writing.
- `memory_observe` strips `<private>...</private>` blocks before writing.
- `memory_put` strips private blocks from legacy text and from structured `entries[].value`.
- If all content is private, the write is skipped and the tool reports `skipped_private=true`.

## Ranked retrieval (Memory v2)

`memory_search` supports a ranked mode backed by an on-disk SQLite index under the memory root:

- index path: `state_dir/memory/.memory_index.sqlite3`
- default: `use_index=true`
- scope: results are restricted to the same file set the tool would otherwise scan (`daily_days`, core/session/structured)

When SQLite or FTS5 is unavailable at runtime, `memory_search` automatically falls back to a bounded substring scan.

## Structured consolidation + checkpoints

For durable “facts” that should survive long-running evolution, prefer:

- write daily/raw observations via `memory_observe` (preferred) or `memory_write(layer="daily")`
- periodically upsert stable facts into `STRUCTURED.md` via `memory_put(path="STRUCTURED.md", entries=[...])`

### Structured memory schema (v2)

`STRUCTURED.md` contains a machine block (JSON) delimited by:

- `<!-- AGENT_MEMORY_V1_BEGIN -->`
- `<!-- AGENT_MEMORY_V1_END -->`

The delimiter names are historical; the **payload schema** evolves. Current schema:

- `schema: "agent_memory_v2"`
- `items: { <key>: <record> }`

Each record keeps both:

- **current** value (`kind`, `value`, `status`, `updated_utc`, `observed_utc`, `sources[]`)
- **history** (`versions[]`) for superseded facts

Deterministic semantics:

- Same `kind/value/status` + same `source` → no-op (idempotent).
- Same `kind/value/status` + new `source` → evidence-only update:
  - appends to `sources[]` (deduped)
  - refreshes `observed_utc`
  - does **not** change `updated_utc` and does **not** add a new version.
- Different `kind/value/status` → supersede:
  - previous current is pushed into `versions[]` (newest-first) with `superseded_utc`
  - current becomes the new value and its `sources[]` starts from the incoming source.

Bounds (to keep files small):

- `sources[]` capped (oldest dropped)
- `versions[]` capped (oldest dropped)

### Deterministic promotion via `@mem` markers (rolling consolidation v2.1)

To enable **deterministic** rolling consolidation (no LLM required), you can write explicit markers into daily memory.
The daemon can then promote them into `STRUCTURED.md`:

Marker syntax (one per line, optional bullet prefix):

```text
@mem fact <key> = <value>
@mem pref <key> = <value>
@mem task <key> = <value>
@mem deprecated <key> = <value>
```

Example:

```text
- @mem fact ui.rendering = Scene rendering must survive refresh + restart
- @mem deprecated feature.a = Feature set A
```

On demand, call:

- `POST /api/v1/memory/consolidate` (auth required when daemon auth is enabled)

To run periodically, start `agentd` with:

- `--memory-consolidate-interval-ms <n>` (0 disables; default)
- `--memory-consolidate-daily-days <n>` (default: 14)
- `--memory-consolidate-keep-checkpoints <n>` (default: 100)

Structured updates produce a time-stamped checkpoint JSON snapshot under `memory/checkpoints/` by default:

- `checkpoint=true|false` (default: true)
- `keep_checkpoints=<N>` (default: 100)

When a checkpoint is written, `memory_put` also reports:
- `checkpoint_path` (relative to memory root)
- `checkpoint_ts_utc`
- `checkpoint_sha256` (sha256 of checkpoint JSON bytes)
- `checkpoint_bytes`

These fields exist so workflows can attach a stable “memory evidence hash” to their event logs without needing to
re-open files during execution.

## Memory retention (v1)

This is a deterministic, operator-controlled policy to keep durable memory growth bounded.

Goals:
- Bound disk usage for daily logs and structured checkpoints.
- Provide dry-run previews.
- Support both on-demand and background enforcement.
- Allow structured memory facts to be deprecated when they age out of policy.

Policy surfaces:
1) Daily logs (state_dir/memory/YYYY-MM-DD.md)
   - Age bound: keep at most daily_max_days files (oldest deleted first)
   - Size bound: keep total daily bytes under daily_max_bytes
2) Structured checkpoints (state_dir/memory/checkpoints/structured_*.json)
   - Age bound: delete checkpoints older than checkpoint_max_days
   - Count bound: keep at most checkpoint_max_count (oldest deleted first)
3) Structured deprecation (STRUCTURED.md)
   - Entries older than structured_deprecate_days are upserted as status="deprecated"
   - Bound by structured_deprecate_max_entries per enforcement pass

API:
- POST /api/v1/memory/retention/enforce
  - supports dry_run plus per-call overrides
  - returns deleted files and counts

Example request:

{
  "dry_run": true,
  "daily_max_days": 30,
  "daily_max_bytes": 104857600,
  "checkpoint_max_days": 30,
  "checkpoint_max_count": 200,
  "structured_deprecate_days": 90,
  "structured_deprecate_max_entries": 50
}

Config knobs (all optional; 0 disables each bound):
- memory_retention_interval_ms
- memory_retention_daily_max_days
- memory_retention_daily_max_bytes
- memory_retention_checkpoint_max_days
- memory_retention_checkpoint_max_count
- memory_retention_structured_deprecate_days
- memory_retention_structured_deprecate_max_entries

Broker fan-out (multi-deployment):
- POST /v1/agents/{agent_id}/memory/retention/enforce

Background enforcement:
- when memory_retention_interval_ms > 0, the daemon applies policy periodically

## Memory recaps (LLM summaries)

For operator-triggered summaries, agentd can generate recap snapshots from salience-ranked memory:

- `POST /api/v1/memory/recaps` — generate a recap (LLM) or `dry_run` to preview
- `GET /api/v1/memory/recaps` — list recap snapshots under `memory/recaps/`

Recaps use `summary_model` by default and can override with `model` or `summary_model` per request.
Each recap JSON includes the prompt inputs, the recap summary (JSON + text), and the ranked items used.

## API helpers (correlation)

In addition to the tool surface, `agentd` exposes correlation helpers:

- `GET /api/v1/memory/checkpoints` — list checkpoint snapshots + sha256 (bounded by time window)
- `GET /api/v1/memory/checkpoints?structured_path=STRUCTURED.md` — optional filter by structured file path
- `GET /api/v1/memory/correlate?trace_id=...` — find structured keys whose evidence sources mention `trace:<trace_id>`
  - optional filters: `structured_path=...` and `key_prefix=...`
- `GET /api/v1/memory/query?...&key_prefix=...` — bounded query over the **current view** of structured memory
  (reads the newest checkpoint in the requested time window)
- `GET /api/v1/memory/index` — lightweight index of memory files (paths + size/line/token estimates)
- `GET /api/v1/memory/salience` — ranked items by recency + importance (structured + @obs daily)

## WebUI Memory Explorer

The WebUI exposes a **Memory explorer** panel (collapsible) that directly calls the memory endpoints:

- Structured query (`/api/v1/memory/query`)
- Trace correlation (`/api/v1/memory/correlate`)
- Checkpoint listing (`/api/v1/memory/checkpoints`)

This panel is intended for operator/debug use and returns raw JSON so you can inspect structured memory state
and evidence hashes without leaving the UI.

## Deterministic workflow tasks

Durable workflows can query memory without invoking an LLM:

- `kind:"memory_correlate"` — bounded correlation by `trace_id` evidence against structured checkpoints
- `kind:"memory_query"` — bounded query of the structured **current view** by `key_prefix` (reads newest checkpoint)
---

# Source: docs/OREN_LANG_ECOSYSTEM.md

# Oren-Lang Ecosystem Leverage (Agent Framework Integration Map)

Date: 2026-02-05

This document answers: “Is there **multiply-style** ecosystem gain from bringing up `oren-lang` alongside this repo?”
and “How to **maximize leverage** if `oren-lang` is not prioritized near-term?”

## 0) Hard findings (facts from this workspace)

1) `oren-lang` is present at:
   - `/Users/zongbaolu/work/oren-lang`

   It is a language + compiler + VM repo that explicitly targets an “agentic execution substrate”:
   - deterministic, capability-governed execution via AVM (`.obc`)
   - record/replay and budgets
   - result/state/trace hashing for k-of-n validation

   Primary docs in that repo (as of 2026-02-05 inspection):
   - `docs/AGENTIC_REQUIREMENTS.md`
   - `docs/AVM_SPEC.md`, `docs/AVM_SPEC_V1.md`
   - `docs/AVM_SWARM_CONSENSUS.md`, `docs/AVM_MULTIVERSE.md`
   - `docs/AVM_PLUGINS_AND_NESTING.md`

2) This repo already anticipates a “VM target” concept:
   - `DESIGN.md` explicitly mentions “VM targets (Oren AVM)” as a portability goal.

3) This repo’s current high-power surfaces (already implemented) that a VM/DSL can plug into later:
   - Durable workflows (server-side) with an event log + SSE.
   - Durable edge workflows (platform coordinating MCU nodes) with retries/backoff and workflow events.
   - Tool execution surface (tool plugins, basic tools, host tools) and strict schemas.
   - Durable memory + consolidation paths.
   - Broker component for multi-agent relay/orchestration.

## 1) Where the “multiply gain” actually comes from (not from adding “another repo”)

The multiplier effect comes from a **deterministic programmable layer** that can sit between:
- a stateless LLM call surface, and
- real-world tool execution + scheduling + memory consolidation.

If `oren-lang` is “a language/VM project”, the *ecosystem gain* is not “we have a language”, but that we get a
portable artifact (`.obc`) and runtime model that is designed for:
- capability-gated effects (`CALL_NATIVE2(domain, op, nargs)`)
- deterministic modes (TIME/RNG virtualization)
- job scanning without execution (`avm --print-job[-json]`)
- stable hashes for governance (`RESULT_HASH`, `STATE_HASH`, `TRACE_HASH`)

### 1.1 Deterministic glue logic (replace LLM tokens with code)

Use-cases that should *not* require an LLM call every time:
- routing/selecting nodes by capability tags/tools
- safety policy enforcement (“deny privacy_camera unless explicitly allowed”)
- retry/backoff scheduling and escalation policy
- workflow aggregation (first-ok / all-ok / quorum)
- memory consolidation and conflict resolution

These are stable algorithms where determinism is a feature.

### 1.2 Replayability + correctness checking

This repo already values correctness and replay (workflow expectations, durable event logs).
A VM layer can make “re-run the same logic” cheap and verifiable.

Concrete leverage from `oren-lang` AVM:
- **Deterministic hashing:** compare `RESULT_HASH` / `TRACE_HASH` across runs/nodes to detect divergence.
- **Preflight governance:** `avm --print-job-json` binds program hash + policy/budgets + deterministic knobs *without executing bytecode*.
- **Record/replay:** effectful calls can be captured and replayed (designed as a first-class requirement).

### 1.3 Cross-runtime portability of policy

Today you effectively have multiple runtimes:
- desktop daemon/CLI (rich)
- MCU nodes (constrained)
- broker (cloud relay)

If policy is expressed once (in a small deterministic DSL/VM), you can run:
- a “full” policy engine on server, and
- a “subset” on device (or vice versa),
without rewriting logic N times.

That’s the *ecosystem multiplier*: **one policy artifact, many runtimes**.

## 2) Why you can still maximize leverage without bringing up `oren-lang` soon

If resources are constrained and `oren-lang` has low short-term priority, the highest leverage is:

### 2.1 Define the integration boundary now (cheap), not the implementation (expensive)

Bring-up cost explodes when the boundary is implicit.
If we make the boundary explicit as a small “port” contract, the future integration becomes a bounded task.

Concrete output: `docs/spec/agent_vm_port_v0.md`

### 2.2 Keep “policy artifacts” transport-agnostic and storage-agnostic

Don’t bake in:
- filesystem assumptions
- environment variable assumptions
- host shell availability

This aligns with the existing core design stance in `DESIGN.md`.

### 2.3 Use the platform as the coordinator (already chosen in UM‑EAIS)

For IoT and multi-agent:
- the platform (agentd/broker) should remain the system-of-record
- nodes can request coordination (“handoff”), but should not become distributed orchestrators

This reduces integration complexity and keeps correctness enforceable.

## 3.5 What `oren-lang` already gives you that is unusually high leverage for agents

These are “multipliers” because they directly compensate for stateless/context-bounded LLMs.

1) **Capsules as data**: `.obc` is explicitly positioned as a universal artifact (portable across platforms) with a governance story.
   - See `oren-lang/docs/OBC_PORTABILITY.md`.
2) **Budgets + deterministic time**: deterministic time derived from gas + sleep + virtual origin (useful for reproducible backoff/deadline testing).
   - See `oren-lang/docs/AVM_TIME_CALIBRATION.md`.
3) **Nested universes (plugin model A)**: a plugin can be executed as a child universe with explicit `allowed_domains` + budgets + VirtualFS/VirtualNET fixtures.
   - See `oren-lang/docs/AVM_PLUGINS_AND_NESTING.md` and `oren-lang/docs/AVM_MULTIVERSE.md` (cfg keys + return shape).

## 3) Recommended “max leverage” path (when you *eventually* bring up `oren-lang`)

These are ordered by leverage/cost ratio.

### 3.1 Make `oren-lang` a policy module runner (NOT a full agent)

Goal: reduce LLM calls and make system behavior deterministic under load.

Policy module responsibilities:
- evaluate routing selection for edge steps
- decide retry/backoff adjustments
- compute aggregation for workflow nodes
- perform memory consolidation transforms

This keeps `oren-lang` small and bounded.

### 3.2 Run it out-of-process first (tool-server style)

First integration should be:
- a subprocess with a strict stdin/stdout protocol (or JSON-RPC),
so failures are isolated and don’t destabilize `agentd`.

Only after that is stable should it be embedded.

### 3.3 Embed later for MCU/VM targets

Once the language runtime is stable and resource-bounded, embedding becomes realistic.
Before that, embedding usually slows everything down.

## 4) Minimal “next action” that stays low-cost (no full bring-up)

If you want to maximize leverage *now* with minimal time:

1) Treat AVM as a **future deterministic policy engine**, and only implement a “runner boundary” in `agentd` first
   (out-of-process, deny-by-default), not a full language integration.
2) Align the boundary with AVM’s native concepts:
   - `allowed_domains` + budgets
   - record/replay logs as bytes
   - job scanning (`--print-job-json`) for caching and governance

The draft for this boundary is `docs/spec/agent_vm_port_v0.md`.

## 5) Fast bring-up path (today; minimal effort)

If you want immediate leverage without “bringing up the whole language project”:

1) Build/locate AVM (prints absolute path):
   - `tools/oren_avm_bringup.sh --verify`
2) Point `agentd` at the AVM binary:
   - `export AGENTD_AVM_BIN="$(tools/oren_avm_bringup.sh)"`
3) Use **non-exec** governance endpoints (scan/inspect/verify/trace hash) as cheap correctness gates:
   - `POST /api/v1/avm/job_scan`, `.../policy_scan`, `.../inspect`, `.../verify_strict`, `.../trace_hash`
4) When you explicitly want execution, gate it (operator choice):
   - set `AGENTD_AVM_EXEC=1` and use `POST /api/v1/avm/capsule_run` or workflow `kind:"avm_capsule"`
5) Compile a `.oren` source → `.obc` and emit a ready workflow task JSON:
   - `tools/oren_capsule_task.sh --src /abs/path/to/prog.oren --task-id AVM`
---

# Source: docs/PLATFORM_SUPPORT.md

# Platform Support Matrix

This repo targets **desktop/server** environments. `agentd` is intended for Windows, Linux, and macOS. `agent_core` is intended for embedded/MCU.

## Agentd (daemon)

| Feature | Linux | macOS | Windows |
|---|---|---|---|
| Core HTTP API + WebUI (daemon-only) | ✅ | ✅ | ✅ |
| SQLite state (`--db-path`) | ✅ | ✅ | ✅ |
| Tool plugins (`--tool-plugin`) | ✅ | ✅ | ✅ |
| Tool servers (`--tool-server-cmd`) | ✅ | ✅ | ❌ |
| AVM endpoints (`/api/v1/avm/*`) | ✅ | ✅ | ❌ (501) |

**Windows notes**
- The HTTP server is Winsock-based (no POSIX socket calls).
- Tool plugins use `LoadLibrary` on Windows; tool servers remain disabled (no POSIX process/poll).
- AVM endpoints return 501 (unsupported) on Windows.
- Validation: run `tools/verify_windows_build.ps1` on a Windows host (optionally with `VCPKG_ROOT` set; `-InstallDeps` can bootstrap vcpkg + deps).
- CI check (host-independent): `tools/check_ci_windows_build.sh` prints the latest workflow status (uses GitHub API; set `GITHUB_TOKEN` or `GH_TOKEN` for private repos).
- CI trigger: `tools/trigger_ci_windows_build.sh [ref]` dispatches the workflow (requires token or logged-in `gh`).

## Broker + Connector

The broker and connector are Go binaries and are portable across Windows/Linux/macOS. Production deployments typically run them in Linux containers.

## WebUI

The WebUI is a static site (Vite build). It can be hosted by any static web server (nginx, Caddy, S3/CloudFront, etc.).
---

# Source: docs/PROTOCOL.md

# Agentd ↔ Web UI Protocol

Date: 2026-02-19

This document defines the **bidirectional** (UI → agentd → agent/tools → UI) protocol surface for rich interactions,
with a focus on:

- multi-agent-safe session handling (UI can create/list/resume sessions without collisions)
- explicit “artifact” signaling (agent can register host files like images/audio/video and provide playback hints)
- optional DB-backed troubleshooting queries (agentd can expose read-only DB endpoints when `--db-path` is enabled)

This project is rolling; schema evolves with versioned event types, but **typed events** are preferred over UI heuristics.

Event envelope reference:
- `docs/spec/run-events/run_events_v1.md`

## Goals

- Make troubleshooting possible by having a **machine-readable event log** instead of relying on regex parsing of tool output.
- Support **multiple agents/daemons** safely:
  - default UI behavior should not collide on a shared `"default"` session id
  - each daemon instance can use its own state/session root
- Support **broker deployments** cleanly when multiple agentd instances register under the same `agent_id`
  - UI may target a specific deployment via broker header `X-Agentd-Deployment`
  - UI scopes local session selection by deployment id to avoid collisions
- Allow agents to present created host files (image/audio/video) and request UI actions like “play audio once/N times”.

## Additional goals

- Provide stable, long-term, versioned public API guarantees with a clear deprecation policy.
- Deliver reliable audio/video playback with explicit gesture-based unlock flows that respect browser autoplay rules.

## Sessions

### Why a new-session endpoint is required

The UI historically defaulted to a fixed session id (`"default"`). With multiple browser tabs or multiple clients,
this causes collisions and makes debugging confusing.

### Endpoint: create a new session id

- `POST /api/v1/session/new`

Request (JSON, optional fields):
- `session_id` (string, optional): request a specific id (must be safe filename)
- `create_files` (bool, optional, default `true`): legacy name; currently means “eagerly create the session row in the DB” (by writing empty messages)

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `created` (bool) whether a new session was created (false means it already existed)
- `error` (string, optional)
- `err` (string, optional; alias of `error`)
- `code` (string, optional; stable-ish error code derived from the message)

Notes:
- The daemon still supports `GET /api/v1/sessions` and `GET /api/v1/session?session_id=...` for listing/resuming.
- Multi-agent safety is achieved by:
  - generating **unique** session ids by default (UUID-ish)
  - allowing daemon operators to set separate state/session roots per daemon instance
  - when using the broker, the UI also includes `brokerDeploymentId` in its session scope key so switching
    deployments does not reuse the same session id unintentionally

### Endpoint: upload session files (for multimodal prompts)

The Web UI can upload local files into a session folder, and then reference them in subsequent run requests via `input_files`.

- `POST /api/v1/session/upload`

Request (JSON):
- `session_id` (string, required)
- `files` (array, required): list of:
  - `name` (string, required): safe filename (ASCII, no path separators)
  - `mime` (string, optional): MIME type hint (used for UI preview + inlining decisions)
  - `data_base64` (string, required): file bytes (base64)

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `files` (array): list of accepted uploads:
  - `name` (string)
  - `mime` (string, optional)
  - `kind` (string, optional): `"image" | "audio" | "video" | "text" | "file"`
  - `path` (string): session-relative path (e.g. `uploads/<ts>_<idx>_<name>`)
  - `bytes` (number): decoded size
- `errors` (array, optional): rejected uploads:
  - `index` (number)
  - `name` (string, optional)
  - `code` (string, optional): `invalid_entry | invalid_name | missing_data | invalid_base64 | file_too_large | invalid_path | write_failed`
  - `error` (string, optional)
  - `bytes` (number, optional)
  - `max_bytes` (number, optional)

Notes:
- This endpoint requires daemon DB enabled/open (same as most session endpoints).
- Uploads are stored under `<sessions_root>/session_<session_id>/uploads/`.
- Upload size is capped (best-effort) to keep the daemon memory-bounded.
  - Config: `--upload-max-bytes <n>` or `AGENTD_UPLOAD_MAX_BYTES` (decoded bytes; default 32 MiB; `0` disables per-file cap).
- If no files are accepted, the endpoint returns `ok=false` and HTTP 400 with an `errors[]` array.
- The daemon also enforces a global HTTP body limit via `AGENTD_HTTP_MAX_BODY_BYTES` (default 64 MiB).

### Endpoint: list recent artifacts for a session (from audit)

- `GET /api/v1/session/artifacts?session_id=...&max_bytes=...&max_artifacts=...&include_rotated=0|1&max_files=...`

Response (JSON):
- `ok` (bool)
- `session_id` (string)
- `count` (number)
- `artifacts` (array): flattened list of `artifact` events extracted from per-run audit JSONL

Notes:
- This endpoint is intended for UI indexing/browsing (cross-run).
- Artifact payload schema matches the `artifact` event described below.

### Endpoint: read session audit tail (file-backed)

UIs can inspect recent per-run audit entries (prompt + events) from the session-scoped audit JSONL:

- `GET /api/v1/session/audit?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

The audit file may be rotated when it grows large:
- current: `<session_id>.events.jsonl`
- backups: `<session_id>.events.jsonl.1`, `.2`, ...

## Durable Scene (server-owned, DB-backed)

For UIs that render a “Scene” panel, agentd supports a **durable, server-owned Scene snapshot** per session.
This is intended to make Scene rendering refresh-proof without relying on browser storage.

Endpoints:
- `GET /api/v1/session/scene?session_id=...` → `{ ok, session_id, updated_unix_ms, scene }`
- `POST /api/v1/session/scene/apply` with `{ session_id, ops:[...] }` → `{ ok, apply, scene }`

Notes:
- These endpoints require the daemon DB to be enabled/open.
- The `clear` op exists server-side, but clients should treat it as destructive.
  - WebUI disables one-click “Clear Scene” and does not send `clear` ops from the browser.

Quick reference (client implementers): `docs/CLIENT.md`.

## Runs (UI → agentd)

The Web UI drives the daemon by posting run requests (sync or async). In addition to standard settings (provider config,
tools mode, sandbox knobs), the daemon supports an explicit runaway-loop safety knob:

- `max_repeated_tool_calls` (int, optional): stops a run if the model repeats the **exact same tool call** too many times.
  - default: `12`
  - `0` disables the guard (not recommended)

Additionally, tool loops can be bounded by step count:

- `max_steps` (int, optional): max number of tool-loop steps (provider calls).
  - omitted → daemon default applies (see `/api/v1/config: daemon.max_steps_default`)
  - `0` means unlimited (not recommended for long-running daemons)

For robust runaway protection (especially when a model requests many tool calls in a single step), runs can also be bounded by:

- `max_tool_calls_total` (int, optional): max number of tool calls executed in total.
  - omitted → daemon default applies
  - `0` means unlimited
- `max_tool_calls_per_tool` (int, optional): max number of tool calls executed per tool name.
  - omitted → daemon default applies (often `0`/disabled)
  - `0` means unlimited
- `max_tool_call_args_chars` (int, optional): max length of a single tool call `arguments_json` (best-effort).
  - omitted → daemon default applies
  - `0` means unlimited
- `max_tool_result_chars` (int, optional): cap tool outputs before re-inserting into the prompt (best-effort).
  - omitted → daemon default applies
  - `0` means unlimited

For targeted safety (and to avoid breaking benign high-frequency tools like `fs_read`), runs may also specify an explicit
per-tool map:

- `tool_call_limits` (array, optional): list of `{ tool: string, max_calls: int }`.
  - If an entry exists for a tool name:
    - `max_calls = 0` means unlimited for that tool (explicit disable).
    - otherwise the tool is capped at `max_calls`.
  - If no entry exists for a tool name, the tool falls back to `max_tool_calls_per_tool` (if set) or unlimited.
  - omitted → daemon default applies (see `/api/v1/config: daemon.tool_call_limits_default`)

This exists to prevent pathological “capture camera → send to UI → repeat” loops when the model does not infer a natural stop
condition from the conversation alone.

Tools ceiling:
- Run requests cannot exceed the daemon’s `--tools` setting (e.g., a daemon started with `--tools basic` will reject `tools:"host"`).
  Such requests return HTTP 400.

### Run replay bundles (deterministic audit)

`agentd` persists a **redacted replay bundle** for session-backed runs (best-effort) and exposes it via:

- `GET /api/v1/run/replay?run_id=...`

The replay bundle includes a redacted request/response snapshot plus tool records, and a deterministic hash token
(`agent_json_c14n_v1`) for offline verification.

See: `docs/WORKFLOWS.md` (Run replay bundles section).

### Multimodal inputs (`input_files`)

Run requests may optionally include `input_files` to reference session-uploaded files (via `POST /api/v1/session/upload`):

- `input_files` (array, optional): each entry is either:
  - a string `path` (session-relative), or
  - an object `{ path, name?, mime?, kind? }`

Behavior:
- `input_files` requires session persistence (`no_session=false`) because files live under the session folder.
- For OpenAI-compatible providers/models that support multimodal messages, agentd translates these into a `messages[].content` array
  containing text + image parts (and text blocks for non-image attachments).
- Not all models support image input. Some models (and some providers) will reject image parts with an HTTP `400`.
  - In that case, use a vision-capable model, or remove `input_files`.

## Tool registry introspection (UI → agentd)

The UI can query the daemon’s effective tool registry (schemas) to render tool docs and validate tool-call arguments:

- `GET /api/v1/tools?tools=host|basic|none&yolo=0|1&host_policy=full|readonly&session_id=<id>`

Notes:
- When `session_id` is provided (and `tools=host`), the returned registry may include **session-scoped tools**
  such as `ui_wait_event` (which depends on a per-session client event log).
- Requests cannot exceed the daemon’s `--tools` ceiling; exceeding it returns HTTP 400.
- The response includes `daemon_tools` and (when provided) `requested_tools` for clarity.
- Requires auth when `--auth-token` is enabled (same as other control-plane endpoints).

## UI Actions (agent → UI)

Artifacts cover “render this file”; some workflows need explicit UI intent (e.g. show a notification, request an audio play).

Protocol:
- Host tool `ui_action` returns a JSON envelope with `data.tool="ui_action"` and `data.action={...}`.
- The host tool loop emits a derived `ui_action` event to the UI (similar to derived `artifact` events).

See: `docs/CLIENT.md` (UI actions and client RPC).

## Client events (UI → agentd)

In addition to prompts/runs, the UI can send structured client events back to the daemon (e.g. “audio finished playing”).
These are optionally appended into the session message history and (when `--db-path` is enabled) mirrored into the troubleshooting DB.

See: `docs/CLIENT.md` (client events, state snapshots, and RPC).

### Endpoint: list client events (file-backed)

For debugging without enabling SQLite, UIs can read the tail of the session-scoped client event log:

- `GET /api/v1/session/client_events?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

Legacy note: older builds read from `<sessions_root>/<session_id>.client_events.jsonl`. Current agentd builds store client events in the DB (canonical).
When `include_rotated=1`, the daemon may also include data from rotated backups (`.client_events.jsonl.1`, `.2`, …)
to fill the requested `max_bytes` budget.

### Endpoint: list observed clients (file-backed)

For multi-client debugging, the daemon can list distinct clients that have posted events recently:

- `GET /api/v1/session/clients?session_id=...&max_bytes=...&include_rotated=0|1&max_files=...`

Clients are reported based on the `payload.client` identity fields in the client event log.

## Waiting for UI acknowledgements (host tool)

When using tool loops, an agent may need to wait for a UI/user acknowledgement within a single run (e.g. audio playback finished).
For this, the host toolset exposes a cooperative polling tool:
- `ui_wait_event`
- `ui_wait_any` (OR join)
- `ui_wait_all` (AND join)

See: `docs/CLIENT.md` (client_wait_* tools).
Preferred (client-agnostic) names for the same tools:
- `client_wait_event`
- `client_wait_any`
- `client_wait_all`

## Artifacts

### Problem

The UI can preview files by heuristically extracting `*.png|*.mp3|*.mp4` tokens from tool output, but:
- it is fragile (false positives, missing files, no metadata)
- it cannot express UI intent (e.g. “this file is an audio clip; please play it twice”)
- it makes repeated tool calls more likely (agent can’t reliably know the UI “received” the artifact)

### Design

Add an explicit artifact signaling path:

1. Agent (via a host tool) “registers” a file it created/produced.
2. The host tool returns a structured JSON envelope that includes `data.tool="artifact_register"` and `data.artifact={...}`.
3. The tool loop emits a derived `artifact` event to UIs.

### Host tool: `artifact_register` (tools=host)

Purpose:
- Tell the UI “this host file is an artifact; please render it as media and apply playback hints”.

Arguments (JSON):
- `path` (string, required): filesystem path to the file (absolute or relative)
  - In agentd sessions, `artifact_register` will **copy** the file into the session’s `out/` directory (best-effort) and return a session-relative `artifact.path` like `out/<name>`.
  - UIs should fetch it via `GET /api/v1/file?session_id=<sid>&path=<artifact.path>`.
- `kind` (string, optional): `"image" | "audio" | "video" | "text" | "file"`
- `mime` (string, optional): explicit MIME type (else best-effort from extension)
- `title` (string, optional): UI title/label
- `autoplay` (bool, optional): request UI to attempt autoplay (UI may require user consent)
- `repeat` (int, optional): for audio/video, request play N times (default 1)
- `blob_id` (string, optional): content-addressed blob id (`sha256:<hex>`) when stored in the blob tier

Return (tool output string; JSON envelope):
- `ok` (bool)
- `error` (string, optional)
- `data` (object)
  - `tool`: `"artifact_register"`
  - `artifact`: object containing resolved metadata:
    - `path` (string)
    - `resolved_path` (string, best-effort)
    - `kind` (string)
    - `mime` (string)
    - `size_bytes` (number, optional)
    - `mtime_unix_ms` (number, optional)
    - `title` (string, optional)
    - `autoplay` (bool, optional)
    - `repeat` (number, optional)
    - `blob_id` (string, optional)

### Event: `artifact`

Event object (same envelope shape as other `events` entries):
- `type`: `"artifact"`
- `data`: object
  - `step` (number, optional): tool-loop step
  - `tool_call_id` (string, optional)
  - `tool_name` (string, optional; `"artifact_register"`)
  - `artifact` (object): same as returned from tool output

UI behavior:
- Render images/videos/audio using `GET /api/v1/file?session_id=<sid>&path=<artifact.path>`
- If `artifact.blob_id` is present, UIs may also fetch bytes via `GET /api/v1/blob?blob_id=...` (range-friendly)
- For audio:
  - show a normal audio player
  - if `autoplay=true`, the UI may attempt playback only after explicit user opt-in
  - if `repeat>1`, provide a “Play xN” option (and implement best-effort loop)

### Backwards compatibility

The UI may keep heuristic media extraction as a fallback, but should prefer explicit `artifact` events.
---

# Source: docs/STREAMING.md

# Streaming (OpenAI-compatible) – Design Notes

Date: 2026-01-30

This document describes how this repo handles **OpenAI-compatible Chat Completions streaming** (`stream: true`, SSE),
and the shared implementation used by:

- CLI `agent` (`--stream-assistant`)
- daemon `agentd` (`stream_assistant: true`)
- the host tool provider (`cli/src/openai_tool_provider.cpp`) for tool-loop steps

## Goals

- **Single source of truth** for parsing SSE `data: { ... }` JSON chunks into:
  - assistant text deltas (`choices[0].delta.content`)
  - tool-call deltas (`choices[0].delta.tool_calls`)
  - `finish_reason` (best-effort)
- **Best-effort compatibility** across OpenAI-ish providers:
  - missing `index` in `delta.tool_calls`
  - tool-call arguments fragmented across many chunks
  - legacy `delta.function_call` streaming shape (treated as a single tool call at index 0)
  - providers that ignore `stream: true` and return a normal JSON completion body
- **Minimal coupling**: keep streaming decode logic independent from daemon job/event plumbing.

## Additional goals (current)

- Full coverage for every streaming variant across providers, backed by a compatibility test matrix.
- Streaming support for the **core** layer via a transport-agnostic interface.
- Token-accurate streaming budgets for *all* providers with reconciliation fallbacks.

## Usage accounting (best-effort)

For OpenAI-compatible Chat Completions streaming, this repo attempts to preserve token accounting so workflow-level
`max_total_tokens` budgets remain enforceable even when `stream_assistant:true` is used.

- When streaming is enabled, the tool provider requests `stream_options.include_usage=true`.
- If a provider rejects `stream_options` (commonly as an HTTP 400 "unknown/unrecognized field"), the client retries once
  without `stream_options` to preserve streaming functionality (but token usage will not be available).
- When a streamed chunk includes a top-level `usage` object, the tool provider emits a `llm_usage` event, allowing the daemon
  to aggregate tokens across tool-loop steps and charge durable workflow budgets.

## Interfaces

Core SSE framing (portable, C API):

- `core/include/agent/sse_parser.h`
  - `agent_sse_parser_feed(...)`: incremental SSE framing; emits complete events
  - `agent_sse_event_t`: `{event,id,data}` fields (data lines joined with `\n`)

Shared decoder (host library, used by both CLI + daemon):

- `cli/src/openai_stream_decoder.h`
  - `OpenAIStreamChunk`: decoded fields from a single streamed JSON chunk
  - `openai_stream_parse_chunk_json(...)`: parse `data: { ... }` JSON payload
  - `OpenAIToolCallStreamAccumulator`: reconstruct complete tool calls from `delta.tool_calls` fragments
- `cli/src/openai_stream_adapter.h`
  - `OpenAIStreamCoreAdapter`: bridges OpenAI stream JSON chunks into the core stream decoder
  - CLI/daemon/tools now share the same core delta assembly + usage handling

Core streaming interface (design draft):

- `docs/spec/streaming/core_stream_v1.md`
- Core reference implementation (agent-core):
  - `core/include/agent/stream_decoder.h`
  - `core/src/agent_stream_decoder.c`

## Event schema notes

Host streaming emits `assistant_delta` events (daemon SSE and tool-loop events array) using a consistent payload shape:

- `delta` (string): incremental assistant text
- `step` (number): tool-loop step (`0` for `tools="none"`)
- `epoch` (number): retry/rotation epoch (daemon uses attempt index for `tools="none"`)

The CLI streaming mode prints deltas to stdout (not an event stream), but uses the same decoded content deltas.

## Compatibility matrix (variants)

Decoder variants and how they are covered in tests:

| Variant | Coverage | Evidence |
| --- | --- | --- |
| `delta.content` assistant text | Covered | `tests/test_openai_stream_decoder.cpp`, `tests/agent_local_stream_assistant_smoke.sh` |
| `delta.tool_calls` fragmented args | Covered | `tests/test_openai_stream_decoder.cpp`, `tests/agent_local_stream_tool_loop_smoke.sh` |
| Legacy `delta.function_call` | Covered | `tests/test_openai_stream_decoder.cpp`, `tests/test_openai_tool_provider_stream.cpp` |
| `stream_options.include_usage` usage chunks | Covered | `tests/agentd_workflow_budget_tokens_stream_smoke.sh` |
| Provider retry during stream (429) | Covered | `tests/agentd_stream_provider_retry_smoke.sh` |

## Compatibility matrix (providers)

Provider-level coverage is tracked by runnable smoke tests. These are **evidence of coverage**, not a blanket guarantee:

| Provider | Streaming tested | Tool-call delta | Usage in stream | Evidence |
| --- | --- | --- | --- | --- |
| Local OpenAI-compatible stub | Yes | Yes | Yes | `tests/agent_local_stream_assistant_smoke.sh`, `tests/agent_local_stream_tool_loop_smoke.sh`, `tests/agentd_workflow_budget_tokens_stream_smoke.sh` |
| DeepSeek (live) | Yes (assistant text) | Yes (tool calls) | Unknown | `tests/agentd_stream_assistant_smoke.sh`, `tests/agentd_deepseek_stream_tool_call_smoke.sh` (requires `DEEPSEEK_API_KEY`) |
| OpenRouter (live) | Yes (assistant text) | Yes (tool calls) | Unknown | `tests/agentd_openrouter_stream_assistant_smoke.sh`, `tests/agentd_openrouter_stream_tool_call_smoke.sh` (requires `OPENROUTER_API_KEY`) |
| Moonshot (live) | Yes (assistant text) | Yes (tool calls) | Unknown | `tests/agentd_moonshot_stream_assistant_smoke.sh`, `tests/agentd_moonshot_stream_tool_call_smoke.sh` (requires `KIMI_API_KEY_CN` / `MOONSHOT_API_KEY`) |

## Provider model pinning (OpenRouter)

OpenRouter models can change behavior or deprecate streaming. To identify **known-good** models for streaming
assistant deltas and tool-call deltas, use the probe script:

```bash
tools/probe_openrouter_stream_models.sh build/agentd
```

If you need a quick key sanity check before probing, run:

```bash
tools/check_openrouter_auth.sh
```

If your OpenRouter account enforces header requirements, set:

- `OPENROUTER_HTTP_REFERER`
- `OPENROUTER_X_TITLE`

The auth check, debug helper, and probe scripts will pass these headers through when set.
You can store them as env-style entries in `.not_in_repo` or `project.local.md` (same format as keys),
or export them in your shell.

If that still fails, use the debug helper to inspect model selection and the chat error payload:

```bash
tools/openrouter_auth_debug.sh
```

The script produces a JSON summary under `build/openrouter_probe/` and prints lists of models that pass
assistant streaming, tool-call streaming, and both. Pin those in:

- `AGENT_TEST_OPENROUTER_STREAM_MODEL`
- `AGENT_TEST_OPENROUTER_STREAM_TOOL_MODEL`

OpenRouter auth preflight prefers `AGENT_TEST_OPENROUTER_MODEL` when set. Otherwise, if `/models` does not
provide a `recommended_model`, it picks the first text-capable model (preferring ones that advertise tool
support). If no candidate is found, it falls back to `bytedance-seed/seed-1.6-flash`. Set the env var to a
model your key can access to avoid false auth failures.

To force a specific candidate list (bypass `/openrouter/models` discovery), set:

- `OPENROUTER_STREAM_PROBE_MODELS=modelA,modelB,...`

For a repo-tracked pin file, rerun the probe with `OPENROUTER_STREAM_PROBE_WRITE_PINS=1` to write:

- `ref/openrouter/streaming_pins.json`

Override the pin path with:

- `OPENROUTER_STREAM_PINS_PATH` (probe script output path)
- `AGENT_OPENROUTER_STREAM_PINS` (smoke test override)

The pin file can set distinct `assistant_model` and `tool_model` when no single model passes both checks.
The OpenRouter smoke tests will prefer this pin file when present, falling back to the env vars and defaults above.
If every candidate fails with OpenRouter auth errors, the probe exits with code 77 and prints a skip message.
---

# Source: docs/TOOLS.md

# Tools: Plugins and Tool Servers (Living)

Date: 2026-02-19

This document consolidates the tool extension mechanisms for `agentd`.
It replaces:
- docs/TOOL_SERVERS.md
- docs/TOOL_PLUGINS.md

## Overview

There are two extension paths:

1) Tool plugins (in-process, shared library ABI)
2) Tool servers (out-of-process, JSON-lines over stdio)

Use plugins when you need low-latency calls and can trust the plugin process.
Use tool servers when you want isolation, independent dependencies, or a
separate runtime (Playwright, device bridges, AVM runners, etc.).

Platform support:
- Tool plugins: Linux, macOS, Windows
- Tool servers: Linux and macOS only (POSIX process + poll)

## Tool servers (out-of-process)

Enable one or more tool servers:

./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tests/tool_server_echo.py" \
  --tool-server-timeout-ms 30000 \
  --tool-server-max-line-bytes $((4*1024*1024)) \
  --tool-server-ping-interval-ms 0

Notes:
- tool-server flags are per-server and apply to the most recently declared
  --tool-server-cmd
- max_line_bytes bounds both the JSON response line and buffered partial lines
- optional ping (op:"ping") can be enabled to detect dead servers

### Protocol (JSON-lines)

All messages are single-line JSON objects terminated by \n.

Manifest request:

{"id":1,"op":"manifest"}

Manifest response:

{"id":1,"ok":true,"tools":[{"name":"server_echo","description":"...","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}]}

Execute request:

{"id":2,"op":"execute","tool_name":"server_echo","arguments":{"text":"hello"}}

Execute response:

{"id":2,"ok":true,"tool_result":{"ok":true,"data":{"echo":"hello"}}}

Protocol hardening:
- response id must match request id
- malformed JSON or oversized lines are treated as protocol violations
- stderr is reserved for logs (stdout is JSON-lines only)

### Deterministic testing

ctest includes agentd_tool_server_smoke, which routes a forced tool call to a
server and validates the response.

## Tool plugins (in-process ABI)

The tool plugin ABI allows `agentd` to load shared libraries at runtime.

Enable:
- agentd --tool-plugin /path/to/plugin.so (repeatable)
- agentd --tool-plugin-config '{"key":"value"}' (applies to most recent plugin)

### Required symbols

Manifest:

const char* agentd_tool_plugin_manifest_json(void);

Optional config-aware manifest:

const char* agentd_tool_plugin_manifest_json_ex(const char* config_json);

Execution:

char* agentd_tool_plugin_execute_json(const char* tool_name, const char* arguments_json);

Optional config-aware execution:

char* agentd_tool_plugin_execute_json_ex(const char* tool_name, const char* arguments_json, const char* config_json);

Free:

void agentd_tool_plugin_free(char* p);

### Manifest shapes

Accepted manifest shapes:
- {"ok":true,"tools":[ ... ]}
- [ ... ] (raw array)

Each tool entry:

{
  "name": "my_tool",
  "description": "optional",
  "parameters_json": "{\"type\":\"object\",...}"
}

You may also provide parameters as a JSON object under "parameters".

### Execution return shape

Recommended:
- {"ok": true, "result": { ... }}
- {"ok": false, "error": "reason"}

### Size limits

- manifest capped at 1 MiB
- tool result capped at 4 MiB

## Embedded/MCU tool plugins (compile-time)

Embedded targets typically cannot load shared libraries. For `agent_core` builds, use a
compile-time plugin list that registers tool schemas into `agent_tool_registry_t` and
dispatches via `agent_tool_executor_t`. See:

- `docs/spec/tool_plugins_embedded_v0.md` (draft ABI + constraints)

## Sandboxed execution (plugin host)

Use the tool server protocol to run plugins out-of-process:

./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"

This provides process isolation plus tool-server timeouts and line limits.

## References

- tests/tool_server_echo.py (example tool server)
- tools/agentd_tool_plugin_echo.c (example plugin)
- docs/spec/tool_plugins_sandbox_v0.md (sandbox policy details)
---

# Source: docs/VENDORED.md

# Vendored Subtrees (Read-Only Policy)

This repo includes upstream snapshots under `ref/` for reference and parity.
These directories are **read-only** in this repo to avoid divergence from their
upstream sources. If changes are required, prefer updating upstream or copying
the needed logic into first-party code under `tools/`, `scripts/`, or another
project-owned location.

Guard: `tools/vendored_guard.py` fails CI if files under `ref/` change. Override
with `ALLOW_VENDORED_CHANGES=1` only when intentionally updating a vendored
snapshot.
The guard also checks staged and unstaged working-tree changes, so local hooks
catch edits before commit.
Optional local hook install/uninstall:
`tools/install_git_hooks.sh`, `tools/uninstall_git_hooks.sh`.
Hook status: `tools/hooks_status.sh` (use `--json` for scriptable output,
`--check` to exit non-zero when the guard isn't installed, or combine both).
These helpers respect `core.hooksPath` if you override the default hooks
directory.

Example (CI/local script):

```bash
tools/hooks_status.sh --json --check
```

Hook modes:

- Debugging hook output:

```bash
VENDORED_GUARD_VERBOSE=1 git commit
```

- Install a permanently verbose hook:

```bash
tools/install_git_hooks.sh --verbose
```

- Quiet hook output (only lists changed paths on failure):

```bash
VENDORED_GUARD_QUIET=1 git commit
```

- Precedence: `VENDORED_GUARD_QUIET` overrides `VENDORED_GUARD_VERBOSE`.

- Install a permanently quiet hook:

```bash
tools/install_git_hooks.sh --quiet
```

## Current vendored subtrees

- `ref/claude-mem/` (upstream: thedotmack/claude-mem)
  - `ref/claude-mem/openclaw/test-install.sh` is a 2.3k+ line upstream test
    suite; it remains read-only here. If we need to adjust behavior, create a
    smaller first-party test harness instead of modifying the vendored file.
- `ref/ds-cli/` (upstream: ds-cli)
- `ref/openrouter/` (upstream: OpenRouter references)
---

# Source: docs/WORKFLOWS.md

# Durable Workflows (agentd)

Date: 2026-02-05

`agentd` includes a durable workflow scheduler: a workflow is a persisted DAG of tasks (typically normal `/api/v1/run` requests)
that can continue running across daemon restarts.

The workflow engine is intended to be the framework’s “power source” for:

- task continuity across time (durable + resumable)
- retries + backoff
- deterministic correctness checks
- budgets and higher-level scheduling policies (fairness/backpressure)

## Key semantics

### Workflow deadline (v1.2)

Workflows may optionally carry a scheduler-level deadline:

- `deadline_unix_ms` (submit request top-level; Unix time in milliseconds)

Semantics:
- Once `now_unix_ms > deadline_unix_ms`, the workflow engine:
  - cancels any `queued` tasks (marks them `cancelled` with error `"deadline exceeded"`)
  - requests cancellation for the workflow (best-effort)
  - does **not** forcibly interrupt `running` tasks (v1)
- When all tasks become terminal, the workflow becomes terminal (typically `cancelled`) with error `"deadline exceeded"`.

This deadline is enforced by the **scheduler**, so it helps bound long fan-out workflows even if tasks are misconfigured.

### Scheduling priority (v1)

Workflows and tasks support a simple integer priority hint:

- `workflow.priority` (submit request top-level `priority`)
- `task.priority` (task entry `priority`, or `request.priority`)

Higher priorities are scheduled sooner when multiple runnable tasks exist.

Notes:
- The daemon clamps priorities to `[-1000, 1000]` (default `0`).
- Priority is a scheduling hint, not a correctness guarantee.

### Fairness budgets (v1.1)

Under load, “priority only” is not enough: a single large fan-out workflow can monopolize all workers.
`agentd` therefore supports **simple scheduler-level fairness caps**:

- `--workflow-max-inflight-per-workflow <n>` (default: `2`)
  - Limits how many tasks from the same workflow may be `running` concurrently.
  - Prevents “fan-out storms” from starving other workflows.
- `--workflow-max-inflight-per-session <n>` (default: `0` = disabled)
  - Optional multi-tenant cap across all workflows sharing the same `session_id`.
  - Useful when multiple clients share one daemon and you want predictable fairness.

Env equivalents:
- `AGENTD_WORKFLOW_MAX_INFLIGHT_PER_WORKFLOW`
- `AGENTD_WORKFLOW_MAX_INFLIGHT_PER_SESSION`

### Admission control / backpressure at submit time (v1.5)

Fairness limits how many tasks may be running concurrently, but it does not prevent a client from submitting an
unbounded backlog (which can grow the DB and delay other work). `agentd` therefore supports **admission control**
caps applied at `POST /api/v1/workflow/submit`:

- `--workflow-admit-max-inflight-tasks-per-session <n>` (default: `0` = disabled)
  - Caps total workflow tasks with status `queued|running` across all workflows that share the same `session_id`.
- `--workflow-admit-max-inflight-tasks-total <n>` (default: `0` = disabled)
  - Caps total workflow tasks with status `queued|running` across the whole daemon.

If a submit would exceed a configured cap, the endpoint responds with HTTP `429` and a small `retry_after_ms` hint.

Env equivalents:
- `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_PER_SESSION`
- `AGENTD_WORKFLOW_ADMIT_MAX_INFLIGHT_TASKS_TOTAL`

Notes:
- The per-session cap only applies when `allow_sessions=true` and a non-empty `session_id` is provided.
- `agentd` will auto-create/upsert the referenced session row on first submit to satisfy the DB foreign key constraint.

### Durability and restart recovery

- Workflows and tasks are persisted in the SQLite DB (`agentd.db`).
- If the daemon restarts while tasks are running, those tasks are recovered back to `queued`.
  - This implies **at-least-once** execution semantics for inflight tasks.
  - If your tools or external side-effects are not idempotent, design tasks with explicit idempotency keys.

### Run replay bundles (deterministic audit)

Each run can persist a redacted, deterministic replay bundle for offline verification.

Contents:
- request (redacted)
- response (redacted)
- tool_records (per-tool inputs/outputs)
- replay_sha256 (canonical JSON hash via agent_json_c14n_v1)

Redaction:
- request: remove api_key, Authorization, auth_token, trace_text, http_body, input_files[].data_base64
- response: remove http_body, trace_text

Bounds:
- request JSON max: 512 KiB (redacted)
- response JSON max: 1 MiB (redacted)

Endpoint:
- GET /api/v1/run/replay?run_id=<id>

Notes:
- Runs with no_session=true do not persist replay bundles.
- Oversized payloads are omitted and replay_error is recorded.

### Workflow submit idempotency (v1.3)

Workflow submission supports an optional **idempotency key** to make client retries safe:

- `idempotency_key` (submit request top-level; `id`-safe string, max 128 chars)

Semantics:
- If `idempotency_key` is provided and a workflow already exists with the same:
  - `idempotency_key`, and
  - `COALESCE(session_id,'')` scope (session-less workflows share one global scope),
  then `POST /api/v1/workflow/submit` returns the existing workflow and sets `deduped=true`.
- The second submit does **not** overwrite the existing workflow’s spec; the first accepted submit “wins”.

This is primarily for robustness against:
- client-side retry loops (HTTP timeouts, disconnections)
- load balancers that retry POSTs
- “at-least-once” delivery semantics from upstream systems

### Task dependency scheduling (DAG)

Each task may declare:

- `depends_on: ["A", "B", ...]`

A task is runnable when:

- its status is `queued`
- `now >= ready_unix_ms` (retry/backoff gate)
- all dependencies are satisfied:
  - default: dependency status is `done`
  - if the dependency is `error` and `allow_error=true`, it is also considered satisfied (soft-fail)
  - for aggregation/join tasks (`kind:"aggregate"`), dependencies may be any terminal state (`done|error|cancelled`)
    so the join can compute over failures/timeouts without blocking forever

#### Optional dependency inference (`infer_depends_on`)

Template-based dataflow is safest when it is paired with explicit dependencies. To reduce human error, the workflow submit
endpoint can optionally infer dependencies by scanning each task request JSON for:
- `${task.<id>...}` template references
- `{"$ref":"task.<id>..."}` JSON-native embedding references

When enabled, inferred IDs are merged into `depends_on` for that task before the DAG is validated/persisted.

Enable it per-submit:
- `POST /api/v1/workflow/submit` with top-level `infer_depends_on: true`

Notes:
- This is a correctness convenience; it does not change runtime template semantics.
- If inference introduces a cycle, submission fails with “invalid workflow DAG”.

### Retries and backoff

Each task has:

- `max_attempts` (default: `1`)
- `attempt` (incremented each time the engine claims the task)
- `ready_unix_ms` (used by the engine to defer retries)

On failure, if `attempt < max_attempts`, the engine re-queues the task and sets a bounded quadratic backoff.

Some task kinds can also request a custom retry schedule:

- If a task returns `retryable=true` with `retry_in_ms`, the engine re-queues it using that delay (instead of the default backoff).
  This is intended for polling patterns like edge task completion waits.

### Soft-fail tasks (`allow_error`)

If a task is submitted with:

- `allow_error: true`

Then if that task ends `status="error"`, the workflow may still complete `done` (so long as there are no remaining “hard”
errors from tasks where `allow_error=false`).

### Deterministic correctness checks (`expect`)

`expect` is a deterministic assertion object evaluated by the workflow engine after the task’s run completes.

Supported checks (v1):

- `ok: true|false` (checks the run response `ok` field)
- `assistant_text_contains: "<substring>"` (or an array of substrings)
- `json_pointer_equals`:
  - object `{ pointer: "/some/path", value: <any-json> }`, or
  - array of such objects
- `json_pointer_exists: "/ptr"` (or an array of pointers)
- `json_pointer_regex`:
  - object `{ pointer: "/some/path", regex: "..." }`, or
  - array of such objects
  - semantics: `regex_search` over the stringified value (if the value is not a string, it is JSON-stringified first)
- `json_pointer_number_between`:
  - object `{ pointer: "/some/path", min?: <number>, max?: <number> }`, or
  - array of such objects
  - values may be JSON numbers or numeric strings
- Tool-call constraints (v1.1):
  - `tool_called: "tool_name"` (or array of names)
    - requires that at least one `tool_call` event exists for each named tool
  - `tool_not_called: "tool_name"` (or array of names)
    - requires that no `tool_call` event exists for each named tool
  - `tool_calls_total_between: { min?: <int>, max?: <int> }`
    - bounds total number of tool calls executed (counted from `tool_call` events)
  - `tool_calls_for_tool_between`:
    - object `{ tool: "name", min?: <int>, max?: <int> }`, or
    - array of such objects

Pointers follow JSON Pointer (`""` for root, otherwise `/key/0/subkey`).

If expectations fail, the task is treated as failed (and may retry if configured).

### Prompt templating (v1)

For simple dataflow, the engine expands placeholders in `request.prompt`:

- `${task.<id>.assistant_text}`
- `${task.<id>.json:<json_pointer>}` (e.g. `${task.A.json:/assistant_text}`)

This is resolved from the **completed** dependency’s `assistant_text` (from its persisted `result_json`).

As of v2, template expansion is applied recursively to the task’s full request JSON (not only `prompt`), so you can:
- feed prior task outputs into `edge_invoke.args` for MCU actuation
- wire prior outputs into non-prompt request fields (e.g., structured messages payloads)

### Workflow inputs (`inputs`) (v2.1)

For safer, more maintainable dataflow (vs repeating `${task.X...}` everywhere), tasks may carry an `inputs` object
(`map<string, any-json>`) which is expanded and then made available to later template expansion.

Where `inputs` can be defined:
- Workflow-level: `POST /api/v1/workflow/submit` top-level `inputs` (copied into every task request)
- Per-task: `task.inputs` (or `task.request.inputs`) overrides workflow-level keys for that task
- Runtime: tasks may include `inputs` directly in their persisted request JSON (advanced use / internal tooling)

Inputs are expanded in **two phases**:
1) Expand `task.*` templates across the full task request JSON (so inputs can reference prior task results).
2) Build `inputs_by_name` from the resolved `inputs` object and expand `input.*` templates across the request JSON.
   This is repeated in bounded rounds so `input -> input` chains converge.

Input template forms:
- `${input.<name>}` (stringifies the whole JSON value if it is not a string)
- `${input.<name>.json:<json_pointer>}` (extract a JSON Pointer from the input value)

Constraints:
- Input names must be “id-safe”: `[A-Za-z0-9_-]` (length 1..128).
- `inputs` is persisted in the workflow DB; do not store secrets unless you intend them to be durable.

### JSON-native embedding (`$ref`)

String templating is convenient but always produces strings. For structured dataflow, tasks may embed JSON from prior task
results using a special object form:

```json
{ "$ref": "task.A.json:/some/pointer" }
```

Supported references:
- `task.<id>.assistant_text`
- `task.<id>.json:<json_pointer>`
- `input.<name>`
- `input.<name>.json:<json_pointer>`

If the reference cannot be resolved, the task fails deterministically with `error="template expansion failed"`.

### Deterministic delay task (`kind:"delay"`)

For deterministic scheduling tests and “wait gates” that do not involve an LLM/provider call, workflows can use:

```json
{
  "task_id": "W",
  "kind": "delay",
  "delay_ms": 500,
  "result": { "assistant_text": "woke up" }
}
```

Semantics:
- Sleeps for `delay_ms` (clamped to `0..600000`).
- Produces a deterministic result object with `ok=true`, plus any merged keys from `result`.
- If `result.assistant_text` is omitted, `assistant_text` defaults to `"delay:<delay_ms>"`.

### Deterministic outbound HTTP JSON task (`kind:"http_json"`) (v1.8)

For broker/agent interop and other cross-service collaboration, workflows can run a deterministic outbound HTTP task.

Security model:
- This task is **disabled by default** (SSRF risk).
- Operators must opt in by starting the daemon with `--workflow-enable-http-tasks` (or env `AGENTD_WORKFLOW_ENABLE_HTTP_TASKS=1`).
- Optional hardening: restrict outbound targets with `--workflow-http-allow-host <host[:port]>` (repeatable),
  or env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`.
- Further hardening:
  - `--workflow-http-allow-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
  - `--workflow-http-deny-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_DENY_CIDRS=...`
  - `--workflow-http-deny-private` / env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
  - `--workflow-http-dns-pin` / env `AGENTD_WORKFLOW_HTTP_DNS_PIN=1`
    - When enabled, the daemon pins the **same** resolved addresses used by the outbound policy decision into the actual HTTP request
      (defense-in-depth against DNS rebinding between “check” and “connect”).
- Do **not** embed secrets into persisted workflow specs via headers. `Authorization` headers are rejected at submit time.
  - If you need a bearer token, use `http_json.bearer_env` to reference an env var name (only the name is persisted).

Example (local echo server):

```json
{
  "task_id": "H",
  "kind": "http_json",
  "http_json": {
    "url": "http://127.0.0.1:8080/echo",
    "method": "POST",
    "timeout_ms": 5000,
    "max_bytes": 65536,
    "body": { "ping": "pong" }
  }
}
```

Result shape (high level):
- `ok` is true when HTTP status is 2xx.
- `http.status` is the HTTP status code.
- `http.response_text` is the captured response body (bounded by `max_bytes`).
- `http.resolved_addrs` is best-effort DNS resolution evidence when `--workflow-http-dns-pin` is enabled (array of IP strings).
- `http.response_json` is best-effort parsed JSON (when parse succeeds), which enables downstream `${task.H.json:/http/response_json/...}`
  and `{"$ref":"task.H.json:/http/response_json/..."}` wiring.
- `http.response_sha256` is a best-effort deterministic hash token over `http.response_json` canonical bytes (algorithm `agent_json_c14n_v1`).
  This is intended for quorum-style joins (`kind:"aggregate" mode:"quorum_hashes"`) and cross-agent correctness correlation.
  - `mode:"quorum_hashes"` compares stable string tokens; when a pointer resolves to a non-string JSON value, the platform hashes the
    canonical JSON bytes and votes on the resulting `sha256:<64hex>` token (ergonomics for structured results).

### Deterministic agent-to-agent collaboration task (`kind:"agentd_call"`) (v1.9)

For multi-agent collaboration where **another `agentd` instance** should run a durable workflow and return a correctness-checkable
result, workflows can use `kind:"agentd_call"`.

This is a deterministic collaboration primitive:
 - submit a remote workflow (`POST <base_url>/api/v1/workflow/submit`)
 - poll until terminal (`GET <base_url>/api/v1/workflow?workflow_id=...`)
 - compute best-effort deterministic hash surfaces for quorum/correlation:
   - `agentd.result_sha256`: hash of a **stable projection** of terminal remote `final.result` (preferred stable surface)
     - `agentd.result_sha256_schema=agentd_call_result_stable_v1` (defines which ephemeral fields are pruned before hashing)
   - `agentd.final_sha256`: hash of the full terminal remote workflow JSON (includes workflow_id/timestamps; mainly for audit/debug)
   (algorithm `agent_json_c14n_v1`)

Broker proxy compatibility:
 - `base_url` does not have to be a directly reachable agentd. It can also be a **broker proxy prefix**:
  - `https://<broker>/v1/agents/<agent_id>/proxy`
  - The task will then call `POST .../proxy/api/v1/workflow/submit` and poll `GET .../proxy/api/v1/workflow?...`.
- In broker deployments, auth is usually `Authorization: Bearer <OIDC_JWT>`:
  - use `agentd_call.bearer_env` to reference an env var name containing the OIDC token (secret value is not persisted).
  - optionally, you can avoid hardcoding the proxy path by using `agentd_call.broker_proxy` (server computes/persists `base_url`):

```json
{
  "task_id": "REMOTE",
  "kind": "agentd_call",
  "agentd_call": {
    "broker_proxy": { "broker_base_url": "https://broker.example", "agent_id": "1" },
    "bearer_env": "OIDC_TOKEN",
    "workflow": { "tasks": [ { "task_id": "W", "kind": "delay", "delay_ms": 10 } ] }
  }
}
```

Security model:
- This task is **disabled by default** (same SSRF surface as `http_json`).
- Operators must opt in by starting the daemon with `--workflow-enable-http-tasks` (or env `AGENTD_WORKFLOW_ENABLE_HTTP_TASKS=1`).
- Optional hardening: restrict outbound targets with `--workflow-http-allow-host <host[:port]>` (repeatable),
  or env `AGENTD_WORKFLOW_HTTP_ALLOW_HOSTS=host[:port],...`.
- Further hardening:
  - `--workflow-http-allow-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_ALLOW_CIDRS=...`
  - `--workflow-http-deny-cidr <cidr>` (repeatable) / env `AGENTD_WORKFLOW_HTTP_DENY_CIDRS=...`
  - `--workflow-http-deny-private` / env `AGENTD_WORKFLOW_HTTP_DENY_PRIVATE=1`
  - `--workflow-http-dns-pin` / env `AGENTD_WORKFLOW_HTTP_DNS_PIN=1`
    - When enabled, the daemon pins the **same** resolved addresses used by the outbound policy decision into the actual HTTP request.
- Do **not** embed secrets into persisted workflow specs via headers. `Authorization` headers are rejected at submit time.
  - If you need a bearer token, use `agentd_call.bearer_env` to reference an env var name (only the name is persisted).

Example (remote agent runs a tiny deterministic delay workflow):

```json
{
  "task_id": "REMOTE",
  "kind": "agentd_call",
  "agentd_call": {
    "base_url": "http://127.0.0.1:9090",
    "op": "workflow_submit_and_wait",
    "timeout_ms": 20000,
    "poll_ms": 50,
    "include_tasks": true,
    "include_results": true,
    "workflow": {
      "tasks": [
        { "task_id": "W", "kind": "delay", "delay_ms": 10, "result": { "assistant_text": "remote ok" } }
      ]
    }
  }
}
```

Result shape (high level):
- `ok` is true when the *remote workflow* reaches status `"done"`.
- `agentd.workflow_id` is the remote workflow_id (reused on retry to avoid duplicate submit).
- `agentd.final` is the remote `GET /api/v1/workflow` JSON response (best-effort bounded), enabling references like:
  - `${task.REMOTE.json:/agentd/final/workflow/status}`
  - `{"$ref":"task.REMOTE.json:/agentd/final/result/results_by_task/W/assistant_text"}`

### Parallel agent collaboration macro (`kind:"agentd_parallel"`) (v1.10)

`kind:"agentd_call"` is the primitive; `kind:"agentd_parallel"` is the high-leverage composition:
fan out to multiple remote `agentd` instances **in parallel**, then join deterministically with `kind:"aggregate"`.

This is submit-time syntactic sugar (like `delegate_parallel`):
- the server expands the macro into one derived `kind:"agentd_call"` task per target (task ids `<task_id>:<target_id>`)
- then replaces the macro task with a deterministic join task at the original `task_id` (`kind:"aggregate"`)

Security / gating:
- This macro expands into `agentd_call` tasks, so it is gated by `--workflow-enable-http-tasks` (SSRF surface).
- The same outbound hardening applies (`--workflow-http-allow-host`, `--workflow-http-allow-cidr`, `--workflow-http-deny-cidr`, `--workflow-http-deny-private`, `--workflow-http-dns-pin`).

Example (fan out across 2 remotes; pick the first successful branch):

```json
{
  "task_id": "P",
  "kind": "agentd_parallel",
  "agentd_parallel": {
    "targets": [
      { "id": "r0", "base_url": "http://127.0.0.1:9090" },
      { "id": "r1", "base_url": "http://127.0.0.1:9091" }
    ],
    "agentd_call": {
      "op": "workflow_submit_and_wait",
      "timeout_ms": 20000,
      "poll_ms": 50,
      "include_tasks": true,
      "include_results": true,
      "workflow": {
        "tasks": [
          { "task_id": "W", "kind": "delay", "delay_ms": 10, "result": { "assistant_text": "remote ok" } }
        ]
      }
    },
    "aggregate": {
      "mode": "first_ok",
      "ok_pointer": "/ok",
      "value_pointer": "/agentd/final/result/results_by_task/W/assistant_text"
    }
  }
}
```

Notes:
- `agentd_parallel.agentd_call.base_url` must be omitted (each target provides a base_url).
- The server overwrites `aggregate.task_ids` to match the derived tasks.
- Default join strategy is `mode:"first_ok"` if `aggregate.mode` is omitted.
- For `aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.node_pointer="/agentd/base_url"` when omitted, so
  `require_distinct_nodes:true` counts distinct remote agent targets correctly.
- For `aggregate.mode:"quorum_hashes"`, the server also defaults `aggregate.pointers=["/agentd/result_sha256"]` when omitted.
- `agentd_parallel.targets[]` entries may also use `broker_proxy:{broker_base_url,agent_id}` (same as `agentd_call.broker_proxy`); the server computes `base_url` as `.../v1/agents/<agent_id>/proxy`.

### Deterministic memory update task (`kind:"memory_put"`) (v1.7)

To make memory updates **correctness-gated** and correlated to durable execution, workflows can run a deterministic
structured memory upsert task:

```json
{
  "task_id": "M",
  "kind": "memory_put",
  "depends_on": ["A"],
  "memory_put": {
    "path": "STRUCTURED.md",
    "entries": [
      { "key": "wf.last_alpha", "kind": "fact", "value": "${task.A.assistant_text}" }
    ],
    "checkpoint": true,
    "keep_checkpoints": 100
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host --host-policy full` (this task executes the host tool `memory_put`).
- Only **structured** updates are allowed (it does not accept legacy `text` overwrites).
- The engine injects correlation evidence into `entries[].source` when missing, so structured memory records keep a bounded
  `sources[]` trail like: `workflow:<workflow_id> task:<task_id> trace:<trace_id> [session:<session_id>]`.
- This task does not call an LLM/provider, so it is suitable for deterministic “write facts only when upstream checks passed”
  workflows (pair it with `depends_on` + `expect` on the upstream tasks).

### Deterministic memory search task (`kind:"memory_search"`) (v1.7)

For deterministic, **read-only** memory retrieval (substring/FTS search), workflows can run:

```json
{
  "task_id": "S",
  "kind": "memory_search",
  "memory_search": {
    "query": "refresh-proof",
    "max_results": 5,
    "daily_days": 0,
    "case_sensitive": false,
    "use_index": true
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host` (this task executes the host tool `memory_search`).
- This is a **read-only** host tool, so it works with `--host-policy readonly` or `--host-policy full`.
- The full host tool response is surfaced under the task result as `memory_search_response` for deterministic `expect` assertions
  and JSON templating (`${task.S.json:/memory_search_response/data/results/0/citation}` etc.).

### Deterministic memory timeline task (`kind:"memory_timeline"`) (v1.7)

For deterministic retrieval of a **bounded context window** around a citation (`path:line`), workflows can run:

```json
{
  "task_id": "T",
  "kind": "memory_timeline",
  "depends_on": ["S"],
  "memory_timeline": {
    "citation": "${task.S.json:/memory_search_response/data/results/0/citation}",
    "context_lines": 4,
    "max_chars": 2000
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host` (this task executes the host tool `memory_timeline`).
- This is a **read-only** host tool, so it works with `--host-policy readonly` or `--host-policy full`.
- The full host tool response is surfaced under the task result as `memory_timeline_response` for deterministic `expect` assertions
  and JSON templating (`${task.T.json:/memory_timeline_response/data/text}` etc.).

### Deterministic structured memory query task (`kind:"memory_structured_query"`) (v1.7)

For deterministic retrieval of **structured facts/preferences/tasks** (not fuzzy substring search), workflows can run:

```json
{
  "task_id": "Q",
  "kind": "memory_structured_query",
  "depends_on": ["M"],
  "memory_structured_query": {
    "path": "STRUCTURED.md",
    "key_prefix": "wf.",
    "kinds": ["fact"],
    "status": "active",
    "include_versions": false,
    "include_sources": true,
    "limit": 50
  }
}
```

Semantics:
- Requires the daemon to run with `--tools host` (this task executes the host tool `memory_structured_query`).
- This is a **read-only** host tool, so it works with `--host-policy readonly` or `--host-policy full`.
- To avoid accidental full dumps, the server requires at least one filter: `key`, `key_prefix`, non-empty `kinds[]`, or `source_contains`.
- Optional query-plan knobs:
  - `source_contains`: filter by evidence trail (e.g. `trace:<trace_id>...` injected by `memory_put` tasks)
  - `updated_since_utc` / `updated_until_utc`: bound by record `updated_utc` (ISO UTC strings)
  - `order_by:"updated_desc"`: fetch most recently updated records first
- The full host tool response is surfaced under the task result as `memory_structured_query_response` for deterministic `expect` assertions
  and JSON templating (`${task.Q.json:/memory_structured_query_response/data/results/0/record/value}` etc.).

### Agent collaboration / fallback delegation (`kind:"delegate"`) (v1.6)

Sometimes “power” comes from **redundancy** and **explicit fallback**: run the same intent through multiple candidate
requests (different base_url/model/tools/budgets) and accept the first attempt that succeeds deterministically.

`kind:"delegate"` executes a sequence of sub-requests (attempts) and returns:
- a structured `delegate.attempts[]` array with per-attempt `ok/run_ok/expect_ok` outcomes
- `delegate.chosen_id` (first successful attempt when `stop_on_ok=true`)
- `assistant_text` copied from the chosen attempt (for easy templating)

Example:

```json
{
  "task_id": "D",
  "kind": "delegate",
  "delegate": {
    "stop_on_ok": true,
    "attempts": [
      {
        "id": "primary",
        "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/bad", "api_key": "dummy", "model": "stub" }
      },
      {
        "id": "fallback",
        "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" },
        "expect": { "assistant_text_contains": "OK" }
      }
    ]
  }
}
```

Notes:
- Attempt requests are normal `/api/v1/run` request objects; workflow-level `defaults` are merged into each attempt.
- The workflow engine injects a stable per-attempt `trace_id` when missing (`<workflow_trace_id>:<task_id>:<attempt_id>`).
- This is a **sequential** primitive (v1). For true parallel multi-agent collaboration, model the fan-out as separate workflow tasks
  and use `kind:"aggregate"` to join deterministically.

### Parallel collaboration macro (`kind:"delegate_parallel"`) (v1.7.0)

For true scheduler-visible collaboration (parallel execution, fairness caps, per-attempt budgets), `agentd` supports a submit-time
macro task:

- `kind:"delegate_parallel"`

This is **syntactic sugar**: on submit, the server expands it into:
- one normal workflow task per attempt (derived task IDs `<task_id>:<attempt_id>`, soft-failing by default via `allow_error=true`)
- one deterministic `kind:"aggregate"` join task at the original `task_id` (default mode `first_ok`)

This means:
- attempts run in parallel under normal workflow concurrency and fairness caps
- you can attach `expect` to each attempt (deterministic correctness)
- a single successful attempt yields the join task `ok=true`, while failed attempts do not fail the workflow

Example:

```json
{
  "task_id": "P",
  "kind": "delegate_parallel",
  "delegate": {
    "attempts": [
      { "id": "primary", "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" } },
      { "id": "fallback", "request": { "prompt": "OK", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" },
        "expect": { "assistant_text_contains": "OK" }
      }
    ]
  }
}
```

Join result fields live under the normal aggregate output at task `P`:
- `chosen_task_id` (e.g. `"P:fallback"`)
- `assistant_text` from the chosen attempt (defaults to `/assistant_text`)

#### Custom join strategy (`delegate.aggregate`)

For some collaboration patterns, “first successful” is not the best join. For example:
- **best-of-n** with self-scoring (choose the highest score)
- **quorum hashes** across multiple candidates (deterministic consensus checks)
- **collect** (materialize multiple outputs for downstream deterministic processing)

`delegate_parallel` lets you customize the join by passing `delegate.aggregate` (same knobs as `kind:"aggregate"`, except `task_ids`):
 - the server **overwrites** `aggregate.task_ids` with the derived attempt task ids (`<task_id>:<attempt_id>`)
 - if `aggregate.mode` is omitted, the server defaults to `first_ok`
 - for `aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.pointers=["/assistant_text"]` when omitted
 - for `aggregate.mode:"quorum_hashes"`, the server defaults `aggregate.node_pointer="/effective_base_url"` when omitted (enables distinct-provider quorum)

 Example: best-of-n join over JSON candidates emitted as assistant text:

```json
{
  "task_id": "P",
  "kind": "delegate_parallel",
  "delegate": {
    "aggregate": {
      "mode": "best_of_n",
      "candidate_pointer": "/assistant_text",
      "parse_json": true,
      "score_pointer": "/score",
      "value_pointer": "/answer",
      "maximize": true,
      "require_ok": true
    },
    "attempts": [
      {
        "id": "lo",
        "request": { "prompt": "{\"score\":0.2,\"answer\":\"OK_LOW\"}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" }
      },
      {
        "id": "hi",
        "request": { "prompt": "{\"score\":0.9,\"answer\":\"OK_HIGH\"}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" }
      }
    ]
  }
}
```

## HTTP API

All endpoints require daemon auth when the daemon is started with `--auth-token`.

### Submit a workflow

`POST /api/v1/workflow/submit`

Minimal example (DAG A → B → C):

```bash
curl -fsS \
  -H "Content-Type: application/json" \
  -d '{
    "allow_inline_api_keys": true,
    "tasks": [
      { "task_id": "A", "request": { "prompt": "Alpha", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" } },
      { "task_id": "B", "depends_on": ["A"], "request": { "prompt": "B got ${task.A.assistant_text}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" } },
      { "task_id": "C", "depends_on": ["B"], "request": { "prompt": "C got ${task.B.assistant_text}", "no_session": true, "tools": "none", "base_url": "http://127.0.0.1:9999/v1", "api_key": "dummy", "model": "stub" },
        "expect": { "assistant_text_contains": "Alpha" }
      }
    ]
  }' \
  "http://127.0.0.1:8123/api/v1/workflow/submit"
```

Notes:
- For durable workflows, inline `api_key` storage is rejected by default; set `allow_inline_api_keys=true` only when you
  explicitly accept storing keys in the daemon DB. Prefer configuring keys via daemon runtime config/secrets.

### Get workflow status

`GET /api/v1/workflow?workflow_id=...&include_tasks=1&include_results=0|1`

Optional debugging:
- `include_spec=1` returns the redacted persisted submit request as:
  - `spec_json` (always; may be truncated)
  - `spec` (only when `spec_json` parses as JSON)

Budget visibility (best-effort):
- `workflow_limits` is surfaced when present in the persisted submit spec.
- `workflow_usage` aggregates retry-safe per-task cumulative counters (available even when `include_tasks=0`).
- `workflow_remaining` is computed as `(limit - used)` for any positive limits (available even when `include_tasks=0`).

### Workflow scheduler stats

`GET /api/v1/workflow/stats`

Returns lightweight queue pressure metrics, useful for backpressure tuning and debugging:

- `workflows_by_status` (counts)
- `tasks_by_status` (counts)
- `tasks_queued_ready` vs `tasks_queued_not_ready` (retry/backoff vs runnable)

Optional (best-effort) session-level snapshot for multi-tenant fairness tuning:
- `GET /api/v1/workflow/stats?include_sessions=1&session_limit=32`
- Response fields: `sessions[]` with `inflight_tasks/queued_tasks/running_tasks` per `session_id`

### Workflow events (durable)

`GET /api/v1/workflow/events?workflow_id=...&after_event_id=0&limit=256`

This returns the persisted workflow event log (DB-backed), which is also the source for the SSE stream.

### Workflow streaming (SSE)

`GET /api/v1/workflow/stream?workflow_id=...&cursor=0`

Streams:

- `event: workflow_event` (each durable event record)
- `event: workflow_done` (terminal summary; stream closes)

### List workflows

`GET /api/v1/workflows?status=running&limit=50`

### Cancel a workflow (best-effort)

`POST /api/v1/workflow/cancel`

Body:

```json
{ "workflow_id": "wf_..." }
```

Cancellation semantics:
- queued tasks are transitioned to `cancelled` best-effort
- running tasks are cooperatively cancelled at the next safe boundary (v1.4)
  - host tools: long-running subprocesses are terminated best-effort
  - provider calls: cancellation takes effect between requests/tool calls (cannot always interrupt an in-flight HTTP request)

## Borrowed workflow ideas (adopted and tracked)

These were imported from the urine_monitor project because they compound reliability and operability.

Implemented:
- Evidence bundles: tools/capture_agent_evidence_bundle.sh + tools/check_agent_evidence_bundle.py
- Scenario packs: tools/scenarios/ + tools/scenario_runner.py
- One-command dev stack: tools/devstack_agent.sh + tools/devstack_agent_down.sh
- Operator UX defaults: runtime config via ui/public/agentui-config.js (no rebuild required)

Proposed (still open):
- Standardized API error envelope: {"err":"...","code":"...","details":{...}}

## Storage (SQLite)

See `docs/DB.md` for:

- `workflows`
- `workflow_tasks`
- `workflow_events`
---

# Source: docs/openapi/README.md

# OpenAPI specs layout

The OpenAPI specs are split into a small root file plus referenced subfiles.

- `agentd.yaml` references:
  - `agentd/paths.yaml`
  - `agentd/components.yaml` (which indexes `agentd/components/*.yaml`)
- `broker.yaml` references:
  - `broker/paths.yaml`
  - `broker/components.yaml`

Update the `paths.yaml` and `components.yaml` files for most changes. For
agentd schemas, edit the domain-specific files under
`docs/openapi/agentd/components/`. Keep the root specs (`agentd.yaml`,
`broker.yaml`) focused on metadata, tags, and top-level `$ref` wiring so
tooling can resolve the full contract.

## Bundling helper

For consumers that do not resolve `$ref` values, use:

```bash
tools/openapi_bundle.py docs/openapi/agentd.yaml -o out/agentd.openapi.yaml
tools/openapi_bundle.py docs/openapi/broker.yaml -o out/broker.openapi.yaml
```
---

# Source: docs/spec/README.md

# Spec Index

Date: 2026-02-19
Status: reference index (rolling)

This folder contains versioned protocol and subsystem specifications. Each spec
declares its own status (draft/rolling/implemented) in the document header.

## Core protocol

- `run-events/run_events_v1.md`: canonical run/workflow event envelope + payload schemas.
- `run_request_refactor_v1.md`: run request refactor record (implemented).

## Streaming

- `streaming/core_stream_v1.md`: streaming contract and decoder expectations.
- `streaming/audio_stream_v0.md`: audio streaming signaling (draft).

## OTA

- `ota/agentd_ota_v0.md`: agentd OTA control plane and operator handoff.

## Tool plugins

- `tool_plugins_sandbox_v0.md`: plugin sandboxing and host policy rules.
- `tool_plugins_embedded_v0.md`: embedded/MCU tool plugin ABI (draft).

## Memory

- `memory/memory_dynamic_policy_v0.md`: retention + salience policy model.

## Agent/edge interop

- `agentd-agentd/agentd_agent_interop_v0_1.md`: agentd-to-agentd interop contract.
- `um-eais/`: embedded/edge interop specs, schemas, and fixtures.

## AVM + VM ports

- `avm_capsule_run_v0.md`: AVM capsule execution contract.
- `agent_vm_port_v0.md`: agent VM port integration contract.
