<!-- GENERATED FILE. DO NOT EDIT. -->
<!-- Edit docs/handbook/OVERVIEW.md and source docs; run tools/build_handbook_bundle.py. -->

# Agent Handbook (Unified)

Date: 2026-02-19

This handbook consolidates the essential information needed to build, run, extend, and operate the agent stack.
It merges the key content from the architecture, protocol, workflows, tools, memory, diagnostics, DB, streaming,
limits, broker, and platform notes into a single place so you can onboard without jumping across many files.

If you need deeper, versioned specs, see `docs/openapi/README.md` and `docs/spec/README.md`.

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
See `docs/DOD_ACK.md` for the handshake contract.

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

Compatibility notes and probe tooling live in `docs/STREAMING.md`.

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
Full semantics: `docs/LIMITS.md`.

---

## 12) Diagnostics + DB observability

Diagnostics endpoints (bearer auth required if `--auth-token` is set):
- `GET /api/v1/diagnostics` (health snapshot)
- `GET /api/v1/diagnostics/providers` (key presence + base URLs)
- `POST /api/v1/diagnostics/provider_test` (quick provider smoke test)

SQLite DB (when enabled with `--db-path`):
- Canonical store for sessions, runs, events, tools, workflows, artifacts, audit.
- Query endpoints provide read-only troubleshooting and analytics surfaces.

Primary DB docs: `docs/DB.md`.

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

Full protocol details and security model: `docs/BROKER.md`.

---

## 14) Deployment + hardening (summary)

Recommended: expose broker publicly, keep agentd private.

Hardening highlights:
- Always set `--auth-token` for non-loopback binds.
- Use strict CORS allowlists for browser clients.
- Keep provider keys server-side (`.not_in_repo` or `AGENTD_DOTENV_PATH`).
- Set HTTP size/time limits via env.
- For OTA: enable `--ota-enable` and set `--ota-command`.

Production details: `docs/DEPLOYMENT.md`.

---

## 15) Platform support

| Feature | Linux | macOS | Windows |
|---|---|---|---|
| agentd HTTP API + WebUI | yes | yes | yes |
| SQLite (`--db-path`) | yes | yes | yes |
| Tool plugins | yes | yes | yes |
| Tool servers | yes | yes | no |
| AVM endpoints | yes | yes | no (501) |

Full matrix: `docs/PLATFORM_SUPPORT.md`.

---

## 16) OpenAPI + specs

- OpenAPI: `docs/openapi/README.md` (YAML in `docs/openapi/`)
- Versioned specs: `docs/spec/README.md`

---

## 17) Additional references

- `docs/TOOLS.md` (tool servers/plugins)
- `docs/WORKFLOWS.md` (workflow engine deep dive)
- `docs/MEMORY.md` (memory internals)
- `docs/DIAGNOSTICS.md` (diagnostics API)
- `docs/DB.md` (SQLite schema + query API)
- `docs/STREAMING.md` (streaming notes + compatibility)
- `docs/BROKER.md` (broker design + API)
- `docs/PROTOCOL.md` and `docs/CLIENT.md` (detailed client protocol)

---

# Source Index

The handbook is a curated summary. For full detail, refer to the source docs below.

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

