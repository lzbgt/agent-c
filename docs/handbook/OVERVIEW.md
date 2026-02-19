# Agent Handbook (Unified)

Date: 2026-02-19

This handbook is a **curated summary** of how to build, run, and operate the agent stack. It avoids
long, chronological detail and points to the feature design docs for depth.

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

Host build deps (macOS/Homebrew):

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

Goals:
- Fast startup, small footprint.
- OpenAI-compatible backends (OpenAI/OpenRouter/DeepSeek/Moonshot, etc.).
- Deterministic, inspectable execution (workflows, audits, replay bundles).
- Portability: desktop + daemon + embedded/VM.

Layered stack:
1. `agent_core` (portable C library)
2. `agentd` (daemon with HTTP/SSE, persistence, workflows)
3. `agent` CLI (local sessions, transport, host tools)
4. Broker (optional relay + auth)
5. Clients (WebUI, mobile, integrations)

Design details: `DESIGN.md`.

---

## 3) Runtime modes

- **Daemon (`agentd`)**: HTTP/SSE APIs, persistence, tools, workflows, broker connectivity.
- **CLI (`agent`)**: local sessions; can target daemon endpoints.
- **Core-only**: portable C API; storage and transport are host responsibilities.

---

## 4) Configuration + secrets

Auth + CORS (daemon):
- `--auth-token` required when binding to non-loopback.
- Loopback defaults to permissive CORS for dev.
- Non-loopback requires explicit `--cors-origin` allowlist.

Provider keys (preferred):
- `.not_in_repo` at repo root (gitignored), or `project.local.md`.
- `AGENTD_DOTENV_PATH=/path/to/.env` overrides dotenv lookup.

Useful env overrides:
- `AGENTD_DB_PATH`, `AGENTD_STATE_DIR`
- `AGENTD_HTTP_MAX_BODY_BYTES`, `AGENTD_HTTP_MAX_HEADER_BYTES`, `AGENTD_HTTP_READ_TIMEOUT_MS`
- `AGENTD_UPLOAD_MAX_BYTES`
- `AGENTD_CORS_ORIGINS`, `AGENTD_CORS_ALLOW_CREDENTIALS`

---

## 5) API surfaces (summary)

Core client API groups:
- Sessions, runs, artifacts, uploads, and audits: `docs/PROTOCOL.md`
- Workflows and task semantics: `docs/WORKFLOWS.md`
- Diagnostics: `docs/DIAGNOSTICS.md`
- DB query endpoints: `docs/DB.md`

OpenAPI index: `docs/openapi/README.md`.

---

## 6) Tools + extensions

Tool modes: `none`, `basic`, `host`.

Out-of-process tool servers (Linux/macOS):
- JSON-lines protocol over stdout.
- Configure via `--tool-server-cmd` and related flags.

In-process tool plugins:
- Shared libs with required `manifest/execute/free` symbols.
- Optional config-aware `_ex` variants.

Isolation option:
- `agentd_tool_plugin_host` runs plugins out-of-process using the tool-server protocol.

Reference: `docs/TOOLS.md`.

---

## 7) Memory (durable + searchable)

Memory lives under `state_dir/memory/` and supports:
- durable facts/preferences
- structured memory
- recaps and salience

Context injection modes: `files`, `search`, `index`, `salience`.

Reference: `docs/MEMORY.md`.

---

## 8) Streaming

Streaming is handled via shared parsers/decoders in core and host adapters.
Compatibility matrix + probe tooling: `docs/STREAMING.md`.

---

## 9) Limits + safety

Run limits include: steps, tool-call caps, args/result size, repeated-call guards.
Full semantics: `docs/LIMITS.md`.

---

## 10) Diagnostics + DB observability

Diagnostics endpoints and provider checks: `docs/DIAGNOSTICS.md`.
SQLite schema, blobs, and query APIs: `docs/DB.md`.

---

## 11) Broker (secure relay)

Provides OIDC/JWT auth, mTLS connectors, agent registry, and proxy/SSE relay.
Reference: `docs/BROKER.md`.

---

## 12) Deployment + hardening

Recommended: broker public, agentd private.
Reference: `docs/DEPLOYMENT.md`.

---

## 13) Platform support

Matrix (Linux/macOS/Windows): `docs/PLATFORM_SUPPORT.md`.

---

## 14) OpenAPI + specs

- OpenAPI: `docs/openapi/README.md` (YAML in `docs/openapi/`)
- Versioned specs: `docs/spec/README.md`

---

## 15) Additional references

- `docs/TOOLS.md` (tool servers/plugins)
- `docs/WORKFLOWS.md` (workflow engine deep dive)
- `docs/MEMORY.md` (memory internals)
- `docs/DIAGNOSTICS.md` (diagnostics API)
- `docs/DB.md` (SQLite schema + query API)
- `docs/STREAMING.md` (streaming notes + compatibility)
- `docs/BROKER.md` (broker design + API)
- `docs/PROTOCOL.md` and `docs/CLIENT.md` (detailed client protocol)
