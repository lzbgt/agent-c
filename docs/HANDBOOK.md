<!-- GENERATED FILE. DO NOT EDIT. -->
<!-- Edit docs/handbook/OVERVIEW.md and source docs; run tools/build_handbook_bundle.py. -->

# Agent Handbook (Unified)

Date: 2026-02-19

This handbook is a **compact summary**. It avoids long history and points to the feature design docs for depth.
For versioned specs, see `docs/openapi/README.md` and `docs/spec/README.md`.

---

## 1) Quickstart

Default host build + tests:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

One-command verify (configure + build + tests; writes logs under `build/`):

```bash
tools/verify.sh
```

Core-only build (portable core library; skips host binaries):

```bash
cmake -S . -B build-core -DAGENT_BUILD_HOST=OFF
cmake --build build-core -j
ctest --test-dir build-core --output-on-failure
```

---

## 2) Architecture (summary)

Goals:
- Fast startup, small footprint.
- OpenAI-compatible backends (OpenAI/OpenRouter/DeepSeek/Moonshot, etc.).
- Deterministic execution (workflows, audits, replay bundles).
- Portability: desktop + daemon + embedded/VM.

Stack:
1. `agent_core` (portable C library)
2. `agentd` (daemon with HTTP/SSE, persistence, workflows)
3. `agent` CLI (local sessions, transport, host tools)
4. Broker (optional relay + auth)
5. Clients (WebUI, mobile, integrations)

System map: `DESIGN.md`.

---

## 3) Role-based quick paths (merged guidance)

Operator (run it in prod/local):
- Start with `docs/DEPLOYMENT.md` + `docs/OPERATIONS.md` + `docs/AGENTD.md`.
- Verify: `tools/verify.sh` (or `tools/verify_compose_stack.sh` for full stack).
- Keep auth + CORS + size limits aligned with `docs/LIMITS.md`.

Developer (core/daemon/CLI):
- Build + tests (Quickstart above), then `docs/CLI.md`, `docs/TOOLS.md`, `docs/WORKFLOWS.md`.
- Protocol/event payloads: `docs/PROTOCOL.md` + `docs/spec/README.md`.

Architect/PM (capability roadmap):
- Read `DESIGN.md` + `docs/AGENTIC_VISION.md` + `TODOS.md`.
- Control plane + relay: `docs/BROKER.md`; memory: `docs/MEMORY.md`.

---

## 4) Runtime modes

- **Daemon (`agentd`)**: HTTP/SSE APIs, persistence, tools, workflows, broker connectivity.
- **CLI (`agent`)**: local sessions; can target daemon endpoints.
- **Core-only**: portable C API; storage and transport are host responsibilities.

---

## 5) Configuration + secrets

Auth + CORS (daemon):
- `--auth-token` required when binding to non-loopback.
- Loopback defaults to permissive CORS for dev.
- Non-loopback requires explicit `--cors-origin` allowlist.

Provider keys (preferred):
- `.not_in_repo` at repo root (gitignored), or `project.local.md`.
- `AGENTD_DOTENV_PATH=/path/to/.env` overrides dotenv lookup.
- OpenRouter may require `OPENROUTER_HTTP_REFERER` and `OPENROUTER_X_TITLE` headers.
- Audio signaling smokes can use `AGENTD_TEST_PG_DSN` if Docker is unavailable.

Useful env overrides:
- `AGENTD_DB_PATH`, `AGENTD_STATE_DIR`
- `AGENTD_HTTP_MAX_BODY_BYTES`, `AGENTD_HTTP_MAX_HEADER_BYTES`, `AGENTD_HTTP_READ_TIMEOUT_MS`
- `AGENTD_UPLOAD_MAX_BYTES`
- `AGENTD_CORS_ORIGINS`, `AGENTD_CORS_ALLOW_CREDENTIALS`

---

## 6) Safety defaults (agentd)

Tool-loop defaults used when requests omit values (0 = unlimited):
- `max_steps_default = 0`
- `max_tool_calls_total_default = 0`
- `max_tool_calls_per_tool_default = 0`
- `max_tool_call_args_chars_default = 0`
- `max_tool_result_chars_default = 12000`
- `tool_call_limits_default = {}` (no per-tool caps)

Policy hooks (optional guardrails):
- `policy_mode = off|audit|enforce` (audit logs decisions without enforcement).
- Allow/deny tool lists + budget caps (`policy_max_*`).
- Emits `policy_decision` events for audits.

Recommended operator caps (see `docs/LIMITS.md`):
- `proc_exec=4`, `shell_exec=16`, `artifact_register=16`, `ui_action=16`.

State/DB defaults:
- `state_dir` defaults to the daemon working directory (override: `AGENTD_STATE_DIR`).
- `db_path` defaults to `<state_dir>/agentd.db` (override: `--db-path` or `AGENTD_DB_PATH`).

---

## 7) Diagnostics & health

When `--auth-token` is set, all endpoints require `Authorization: Bearer ...`.

- `/api/v1/health`: basic liveness.
- `/api/v1/diagnostics`: readiness, DB size + table counts, job/workflow counts, active provider (no secrets).
- `/api/v1/diagnostics/providers`: provider key presence + base URL (no secrets).
- `/api/v1/diagnostics/provider_test`: small provider smoke test
  (`provider`, `prompt`, `tools`, `require_tool_call`, `timeout_ms`, `max_steps`, etc.).

---

## 8) Workflows (durable scheduling)

- Workflows are persisted DAGs; restart requeues running tasks (at-least-once).
- Priority hint is clamped to `[-1000, 1000]` (default 0).
- Optional `deadline_unix_ms` cancels queued tasks once exceeded.
- Fairness defaults: `--workflow-max-inflight-per-workflow=2`, per-session cap default `0` (disabled).
- Admission control defaults to disabled (`--workflow-admit-max-inflight-tasks-per-session=0`,
  `--workflow-admit-max-inflight-tasks-total=0`).
- Optional idempotency via `idempotency_key` (per-session scope; first submit wins).

---

## 9) Streaming (SSE)

- Shared SSE parser + stream decoder across CLI/daemon/tool loop.
- Requests set `stream_options.include_usage=true` and retry without it on 400s.
- OpenRouter pin workflow: run `tools/probe_openrouter_stream_models.sh`,
  set `OPENROUTER_STREAM_PROBE_WRITE_PINS=1`, commit `ref/openrouter/streaming_pins.json`.
- Smoke tests try pinned models in order (primary keys, then `ok_models_*` lists) before env defaults.
- OpenRouter auth hints: `OPENROUTER_HTTP_REFERER`, `OPENROUTER_X_TITLE`,
  optional `AGENT_TEST_OPENROUTER_SKIP_CHAT_PREFLIGHT=1`.

---

## 10) Where to look (feature design docs)

- `DESIGN.md` for system map and boundaries.
- `docs/AGENTIC_VISION.md` for user/client + architect expectations and evidence-driven acceptance checks.
- `docs/README.md` for a fast doc index.
- `TODOS.md` for roadmap and weighted tasks.
- `docs/` for feature docs: protocol, client, workflows, tools, memory, diagnostics, DB, streaming, broker, deployment, WebUI, platform.
- `docs/OPERATIONS.md` for build, verification, testing, cleanup, and publishing workflows.
- `docs/AGENTD.md` for daemon operations and runtime configuration.
- `docs/CLI.md` for CLI usage and tool-loop guidance.
- `docs/spec/` for versioned specs.
- `docs/openapi/` for OpenAPI definitions.

---

## 11) Operational highlights

- Broker should be public; agentd should remain private.
- Prefer server-side provider keys (`.not_in_repo` / `AGENTD_DOTENV_PATH`).
- Enforce size/time limits via daemon env flags (see `docs/LIMITS.md`).
- Compose verification scripts use a docker readiness check; set `AGENT_DOCKER_INFO_TIMEOUT_SEC` to shorten it.

---

# Source Index

The handbook is a curated summary. For full detail, refer to the source docs below.

- `README.md`
- `DESIGN.md`
- `docs/AGENTIC_VISION.md`
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
- `docs/OPERATIONS.md`
- `docs/OREN_LANG_ECOSYSTEM.md`
- `docs/PLATFORM_SUPPORT.md`
- `docs/PROTOCOL.md`
- `docs/STREAMING.md`
- `docs/TOOLS.md`
- `docs/VENDORED.md`
- `docs/WORKFLOWS.md`
- `docs/openapi/README.md`
- `docs/spec/README.md`
