# agent

This repo is a **production-oriented agentic platform**: a portable core plus a daemon, broker, CLI, and WebUI for durable, audited automation.
It is **not a prototype** — the repo includes deployment checklists, verification scripts, and evidence bundles for repeatable, production-grade runs.

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
- **Shared codexw broker bridge**: the native connector advertises runtime
  actions plus session/event activity surfaces so codexw's broker, iOS app, and
  WebUI can show `agentd` daemon sessions, workflows, client events, and
  workflow events through `/api/v2/runtime-instances/...`.

## NanoClaw leverage (in progress)

We reviewed NanoClaw (cloned under `refs/nanoclaw`) and captured a concrete
leverage plan in `docs/spec/nanoclaw_leverage_v0.md`. Near-term, low-risk
targets include:

- Channel/connector self-registration (registry boundary).
- Per-group workspace + memory scope isolation.
- Optional containerized tool runner with mount allowlist enforcement.
- First-class cron schedules for workflows.
- Skills-style guided transforms for common upgrades.
- Team run multi-result streaming semantics and session anchoring.

These items are **targets**, not assumptions about current behavior.

Docs quickstart:
- Unified handbook (curated summary; generated): `docs/HANDBOOK.md`
- Architecture + roadmap: `DESIGN.md` (full system map)
- Build/verify/test/cleanup/publish workflows: `docs/OPERATIONS.md`
- Deployment checklist (agentd + broker + WebUI): `docs/DEPLOYMENT.md`
- Daemon ops + runtime config: `docs/AGENTD.md`
- CLI usage + tool-loop guidance: `docs/CLI.md`
- WebUI dev/build/runtime config: `docs/WEBUI.md`
- Full docs index: `docs/README.md`
- Evidence bundle capture: `tools/capture_agent_evidence_bundle.sh`

Handbook guard:
- `tools/verify_repo_guards.sh` enforces a handbook line limit via `HANDBOOK_MAX_LINES` (default: 250).

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

### codexw shared broker bridge

`tools/agentd_codexw_native_broker_connector.py` registers `agentd` as a
`runtime_kind=agentd` instance in the sibling `codexw` broker. The connector
advertises `broker.runtime_capabilities.v1` with `surfaces.sessions` and
`surfaces.events`, then serves these runtime-local adapter commands:

- `GET /api/v1/runtime/sessions`: projects daemon sessions and workflows into
  neutral runtime-instance session rows.
- `GET /api/v1/runtime/events`: projects client events and workflow events into
  neutral `broker.runtime_event.v1` rows, with `limit`, `after_id`,
  `last_event_id`, `session_id`, and `event_prefix` filtering.
- `GET /api/v1/runtime/status`: reports read-only update readiness. The
  default bridge reports update disabled; the native connector reports
  `agentd` OTA status when `AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota`.
  If `/api/v1/ota/status` includes a `candidate`, `update_candidate`, `release`,
  or `artifact` object with `url`, `sha256`, `version`, or
  `drain_timeout_ms`, the bridge forwards it as `update.candidate` so the
  shared broker can show and prepare the exact `runtime.update` input.

The broker only uses these routes after capability negotiation. Older bridges
that do not advertise the surfaces continue to appear in the shared inventory
with empty common activity instead of fabricated codexw-specific state.

To prove the deployed codexw broker path end to end from this repo, run:

```bash
tools/verify_codexw_live_agentd_activity.sh
```

The smoke uses the sibling `codexw` repo's `scripts/broker-admin` session,
issues a one-time deployment enrollment token, approves the temporary proof
deployment after certificate enrollment, verifies `/api/v2/runtime-instances`,
`/sessions`, and `/events` against the live broker, writes proof JSON under
`build/`, and removes the temporary broker deployment unless
`KEEP_DEPLOYMENT=1` is set.

To prove the opt-in OTA update boundary without replacing a real daemon, run:

```bash
tools/verify_codexw_live_agentd_ota_candidate.sh
```

That proof connects the native connector to a temporary loopback fake
`agentd`/OTA API, enrolls and approves a short-lived broker deployment, verifies
the deployed broker exposes `runtime_kind=agentd`, `update.candidate`, and an
enabled `runtime.update` descriptor, then dispatches a confirmed
`runtime.update` with an idempotency key. The fake OTA endpoint records the
forwarded request body, proving the broker prepared `{url, sha256, version,
reason, drain_timeout_ms}` from current status instead of trusting a client-built
artifact payload. No daemon binary is replaced.

For durable service installs, use the launchd/systemd connector templates in
`docs/DEPLOYMENT.md`. They run `agentd` and the native codexw connector as
separate OS-managed services and keep broker passwords, enrollment secrets, and
local `AGENTD_AUTH_TOKEN` out of process arguments.
The connector also supports a read-only `--self-test` mode for service
readiness checks; with `--require-broker-visible` and broker read credentials,
it proves the durable service is enrolled, connected, and visible through
shared broker runtime-instance inventory.

Broker operator actions are opt-in. By default the connector still does not
advertise `runtime.restart`, `runtime.update`, or `runtime.upgrade`. Set
`AGENTD_CODEXW_RUNTIME_UPDATE_MODE=agentd_ota` only on hosts where `agentd`
OTA is enabled and configured with a daemon-owned drain/restart policy; then
the connector advertises `runtime.update` and forwards broker requests to
`POST /api/v1/ota/update`. `runtime.restart` remains unadvertised until it has
its own supervisor-safe contract.

Quick start (loopback only; no auth required):

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

If you bind to non-loopback (`--host 0.0.0.0`), `agentd` requires `--auth-token` and explicit CORS allowlists.
See `docs/DEPLOYMENT.md` for hardened examples and service configs.

Vendored subtrees live under `ref/` and are treated read-only in this repo.
If changes are needed, update upstream or copy into first-party code. See
`docs/VENDORED.md`.

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
