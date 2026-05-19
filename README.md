# agent

This repo is a **production-oriented agentic platform**: a portable core plus a daemon, broker, CLI, and WebUI for durable, audited automation.
It is **not a prototype** — the repo includes deployment checklists, verification scripts, and evidence bundles for repeatable, production-grade runs.

## Support

If Agent-C helps your work, you can make an optional PayPal contribution to support public project maintenance and experiments: https://x2.brucelu.top/products/support/checkout/?source=github-agent-c-readme

## Status

- Rolling release: expect breaking changes until the first stability milestone.
- Production-oriented verification and deployment tooling (see `docs/OPERATIONS.md` and `docs/DEPLOYMENT.md`).
- Evidence bundles + repo guards for repeatable runs (`tools/capture_agent_evidence_bundle.sh`, `tools/verify_repo_guards.sh`).

## Components
- **agent_core** (portable C library): session model, compaction helpers, role utilities.
- **agentd** (daemon): HTTP/SSE APIs, persistence, tool runtime, workflows, approvals, diagnostics.
- **broker** (optional relay): auth + multi-deployment routing, audit/event replay, team runs, OTA fanout.
- **WebUI** (advanced layout): Scene + Conversation, queued prompts, Memory/Trace/Workflows panels, broker console + teams.
- **CLI** (`agent`): local sessions or remote daemon targeting.

Key capabilities:
- **OpenAI-compatible providers** (OpenAI/OpenRouter/DeepSeek/Moonshot, etc.) with runtime diagnostics and smoke tests.
- **Durable workflows** (DAGs + idempotency) with restart-safe requeueing and evidence tooling.
- **Team orchestration** with role plans, approvals, runtime members, and replayable events.
- **Memory tooling** (recaps, correlation index, trace linking) and operator-grade panels.
- **Policy/limits hooks** for tool budgets, approvals, and automation profiles.
- **OTA updates** (agentd + broker + WebUI) with drain + continuity checks.

## Demo: ESP32 voice agentic

https://github.com/user-attachments/assets/be605653-04cf-49f1-9696-4dff41796635

## Build

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

One-command verify (configure + build + tests; logs under `build/`):

```bash
tools/verify.sh
```

Include repo hygiene guards:

```bash
tools/verify.sh --repo-guards
```

For a CI-equivalent workspace-size check, run `tools/verify_repo_guards.sh --strict`.

For the Docker broker lane, run `tools/verify_compose_broker_smokes.sh`.

Source `${HOME}/.env` before verification (provider keys for smokes):

```bash
tools/verify_prod.sh
```

Platform dependencies, Windows helpers, core-only build, and smoke-test notes live in `docs/OPERATIONS.md`.

### Tool extensions

Tool plugins and tool servers are documented in `docs/TOOLS.md`.

## Testing

For network smokes, the real browser E2E harness, and verification workflows, see `docs/OPERATIONS.md`.
Daemon runtime details (config snapshot, defaults update, state safety, and HTTP APIs) live in `docs/AGENTD.md`.

## Run in production (agentd + WebUI)

Production shape:
- `agentd` runs as the backend daemon (tools + persistence + HTTP API).
- The WebUI runs as a separate static site (Vite build) that talks to `agentd` or the broker.

For a full production checklist (TLS, broker/connector, auth hardening, backups), see `docs/DEPLOYMENT.md`.
For WebUI dev/build/runtime config, see `docs/WEBUI.md`.

## Cleanup (disk usage)

Cleanup commands, repo size reports, and guard scripts live in `docs/OPERATIONS.md`.

## Docker Compose (prod-like local verification)

For a prod-like local stack (Postgres + Keycloak + broker + connector + agentd + WebUI):

```bash
./tools/verify_compose_stack.sh
```

Alternatives:
- No Docker: `tools/verify_mac_local_stack.sh` (agentd + WebUI only).
- Docker runs but builds blocked: `tools/verify_mac_full_stack_host.sh`.
- No Docker/Keycloak, local Postgres: `tools/verify_mac_full_stack_local_postgres.sh`.
- One-command devstack: `tools/devstack_agent.sh` (stop with `tools/devstack_agent_down.sh`, add `--wipe-volumes` to reset Keycloak).

Tip: local verify supports provider tests via `MAC_LOCAL_PROVIDER_TEST=1` (uses `AGENTD_TOOLS=basic` by default; override with `AGENTD_TOOLS=basic|host|none`).

Full details, env flags, port notes, and Keycloak guidance live in `docs/DEPLOYMENT.md`.

## Git remote (publishing)

Remote setup and publish helpers live in `docs/OPERATIONS.md`.

## CLI usage

CLI examples and tool-loop guidance live in `docs/CLI.md`.

## Core library

- Header: `core/include/agent/agent.h`
- Scope: session model + char-budget compaction + role helpers
- No environment variable access in the core (host-only concern).

## Daemon + Web UI (day-1)

Local daemon usage and HTTP/SSE API notes live in `docs/AGENTD.md`.
WebUI dev/build/runtime config lives in `docs/WEBUI.md`.
