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
- OpenRouter may require `OPENROUTER_HTTP_REFERER` and `OPENROUTER_X_TITLE` headers.
- Audio signaling smokes can use `AGENTD_TEST_PG_DSN` if Docker is unavailable.

Useful env overrides:
- `AGENTD_DB_PATH`, `AGENTD_STATE_DIR`
- `AGENTD_HTTP_MAX_BODY_BYTES`, `AGENTD_HTTP_MAX_HEADER_BYTES`, `AGENTD_HTTP_READ_TIMEOUT_MS`
- `AGENTD_UPLOAD_MAX_BYTES`
- `AGENTD_CORS_ORIGINS`, `AGENTD_CORS_ALLOW_CREDENTIALS`

---

## 5) Where to look (feature design docs)

- `DESIGN.md` for system map and boundaries.
- `TODOS.md` for roadmap and weighted tasks.
- `docs/` for feature docs: protocol, client, workflows, tools, memory, diagnostics, DB, streaming, broker, deployment, platform.
- `docs/spec/` for versioned specs.
- `docs/openapi/` for OpenAPI definitions.

---

## 6) Operational highlights

- Broker should be public; agentd should remain private.
- Prefer server-side provider keys (`.not_in_repo` / `AGENTD_DOTENV_PATH`).
- Enforce size/time limits via daemon env flags (see `docs/LIMITS.md`).
