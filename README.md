# agent (prototype)

This repo is an early scaffold for a **portable agent core** (env-free, persistence-agnostic) plus a **desktop CLI host adapter** (env/config + persistence + HTTP).

Docs quickstart:
- Unified handbook (curated summary; generated): `docs/HANDBOOK.md`
- Architecture + roadmap: `DESIGN.md` (full system map)
- Deployment checklist (agentd + broker + WebUI): `docs/DEPLOYMENT.md`
- WebUI dev/build/runtime config: `docs/WEBUI.md`
- Full docs index: `docs/README.md`

Handbook guard:
- `tools/verify_repo_guards.sh` enforces a handbook line limit via `HANDBOOK_MAX_LINES` (default: 250).

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

Source `${HOME}/.env` before verification (provider keys for smokes):

```bash
tools/verify_prod.sh
```

Platform dependencies, Windows helpers, core-only build, and smoke-test notes live in `docs/BUILD.md`.

### Tool extensions

Tool plugins and tool servers are documented in `docs/TOOLS.md`.

## Testing

For network smokes, the real browser E2E harness, and verification workflows, see `docs/TESTING.md`.
Daemon runtime details (config snapshot, defaults update, state safety, and HTTP APIs) live in `docs/AGENTD.md`.

## Run in production (agentd + WebUI)

Production shape:
- `agentd` runs as the backend daemon (tools + persistence + HTTP API).
- The WebUI runs as a separate static site (Vite build) that talks to `agentd` or the broker.

For a full production checklist (TLS, broker/connector, auth hardening, backups), see `docs/DEPLOYMENT.md`.
For WebUI dev/build/runtime config, see `docs/WEBUI.md`.

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

Cleanup commands, repo size reports, and guard scripts live in `docs/CLEANUP.md`.

## Docker Compose (prod-like local verification)

For a prod-like local stack (Postgres + Keycloak + broker + connector + agentd + WebUI):

```bash
./tools/verify_compose_stack.sh
```

Alternatives:
- No Docker: `tools/verify_mac_local_stack.sh` (agentd + WebUI only).
- Docker runs but builds blocked: `tools/verify_mac_full_stack_host.sh`.
- One-command devstack: `tools/devstack_agent.sh` (stop with `tools/devstack_agent_down.sh`).

Full details, env flags, port notes, and Keycloak guidance live in `docs/DEPLOYMENT.md`.

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

## CLI usage

CLI examples and tool-loop guidance live in `docs/CLI.md`.

## Core library

- Header: `core/include/agent/agent.h`
- Scope: session model + char-budget compaction + role helpers
- No environment variable access in the core (host-only concern).

## Daemon + Web UI (day-1)

Local daemon usage and HTTP/SSE API notes live in `docs/AGENTD.md`.
WebUI dev/build/runtime config lives in `docs/WEBUI.md`.
