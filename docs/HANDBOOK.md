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

## 5) API surfaces (by doc)

- Sessions, runs, artifacts, uploads: `docs/PROTOCOL.md`
- Client collaboration and UI actions: `docs/CLIENT.md`
- Workflows and task semantics: `docs/WORKFLOWS.md`
- Tools + plugins: `docs/TOOLS.md`
- Memory architecture: `docs/MEMORY.md`
- Diagnostics: `docs/DIAGNOSTICS.md`
- DB schema + query API: `docs/DB.md`
- Streaming compatibility: `docs/STREAMING.md`
- Broker relay + auth model: `docs/BROKER.md`
- Deployment hardening: `docs/DEPLOYMENT.md`
- Platform support matrix: `docs/PLATFORM_SUPPORT.md`

OpenAPI index: `docs/openapi/README.md`.

---

## 6) Operational highlights

- Broker should be public; agentd should remain private.
- Prefer server-side provider keys (`.not_in_repo` / `AGENTD_DOTENV_PATH`).
- Enforce size/time limits via daemon env flags (see `docs/LIMITS.md`).

---

## 7) Specs

- OpenAPI: `docs/openapi/README.md` (YAML in `docs/openapi/`)
- Versioned specs: `docs/spec/README.md`

---

## 8) Reference index

- `DESIGN.md`
- `docs/PROTOCOL.md`
- `docs/CLIENT.md`
- `docs/WORKFLOWS.md`
- `docs/TOOLS.md`
- `docs/MEMORY.md`
- `docs/DIAGNOSTICS.md`
- `docs/DB.md`
- `docs/STREAMING.md`
- `docs/BROKER.md`
- `docs/DEPLOYMENT.md`
- `docs/PLATFORM_SUPPORT.md`

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

