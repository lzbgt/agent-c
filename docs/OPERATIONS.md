# Operations Guide

This guide consolidates build, verification, testing, cleanup, and publishing workflows.

## Quick build (host + tests)

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

One-command verify (configure + build + tests; logs under `build/`):

```bash
tools/verify.sh
```

Host verify also runs a lightweight workflow list query smoke (API `q` filter) after CTest.

Optional: run eval pack smoke after build/tests:

```bash
tools/verify.sh --eval-pack
```

Eval pack regression gating can compare to a stored baseline:

```bash
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json --baseline out/baselines/basic_agentd_smoke.summary.json
python3 tools/eval_pack.py --file tools/eval_packs/basic_agentd_smoke.json --baseline out/baselines/basic_agentd_smoke.summary.json --update-baseline
```

Or via the verifier:

```bash
tools/verify.sh --eval-pack-baseline out/baselines/eval_pack_smoke.summary.json
tools/verify.sh --eval-pack-baseline out/baselines/eval_pack_smoke.summary.json --eval-pack-update-baseline
```

Include repo hygiene guards:

```bash
tools/verify.sh --repo-guards
```

Source `${HOME}/.env` before verification (provider keys for smokes):

```bash
tools/verify_prod.sh
```

## Dependencies

Host builds (`agent` / `agentd`) require `libcurl` and `jsoncpp` via `pkg-config`.

macOS (Homebrew):

```bash
brew install cmake pkg-config curl jsoncpp sqlite
```

Windows (native build): install dependencies via vcpkg, then configure with its toolchain file:

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

Platform-specific notes (Windows limitations, plugin support, AVM endpoints) live in `docs/PLATFORM_SUPPORT.md`.
macOS production packaging is documented in `docs/MACOS_PACKAGING.md`.

CI helpers (optional):
- `tools/trigger_ci_windows_build.sh [ref]` (dispatches Windows CI; requires token or `gh auth login`)
- `tools/check_ci_windows_build.sh` (prints latest Windows CI status; requires token for private repos)

## Core-only build (portable; no host deps)

If you only want the portable core library + core unit tests (embedded/toolchain bring-up), disable host builds:

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
See `docs/EMBEDDED_C_API.md` for embedded integration details.

## Full-stack verification (macOS)

Use the macOS scripts when you want broker + connector in the loop:
- Docker compose build: `tools/verify_mac_full_stack.sh`
- Docker runs but builds blocked: `tools/verify_mac_full_stack_host.sh`
- No Docker/Keycloak, local Postgres: `tools/verify_mac_full_stack_local_postgres.sh`
- No broker: `tools/verify_mac_local_stack.sh`
  - Optional env: `AGENTD_TOOLS=basic|host|none` to override the agentd tools mode (provider tests switch to `basic` by default)

Compose image builds pass proxy args for package installs. Defaults:
- `HTTP_PROXY`/`HTTPS_PROXY`: `http://host.docker.internal:8120`
- `NO_PROXY`: `localhost,127.0.0.1,::1,host.docker.internal`
Use `host.docker.internal` (not an ad-hoc hostname like `m2`) so containers can resolve the proxy consistently.
If the proxy is unreachable during build, the Dockerfiles now retry apt installs once without proxy to avoid
hard failures (still prefers proxy when available).
Local compose health checks bypass host proxies (proxy env vars are unset and `curl --noproxy "*"`)
to avoid TLS errors when a machine-wide proxy is configured.
On macOS we prefer Homebrew curl if available (OpenSSL backend) to avoid LibreSSL TLS issues.
Override with `CURL_BIN=/path/to/curl` (or `AGENT_SMOKE_CURL_BIN` for smoke tests) if needed.
Smoke tests will wrap curl calls by default; set `AGENT_SMOKE_WRAP_CURL=0` to opt out,
or `AGENT_SMOKE_ALLOW_PROXY=1` if you need proxy env vars for outbound tests.

Runtime containers do not receive proxy environment variables by default so intra-stack
traffic stays direct. Dockerfiles also remove apt proxy config after installs to avoid
accidentally forcing proxies at runtime. Override by exporting `HTTP_PROXY`/`HTTPS_PROXY`
before `docker compose`.

Port selection note: the compose verification script prefers the default ports, but will
fall back to random high ports if the common ones are already in use. You can always pin
ports explicitly via `BROKER_PUBLISHED_PORT`, `KEYCLOAK_PUBLISHED_PORT`,
`POSTGRES_PUBLISHED_PORT`, `AGENTD_PUBLISHED_PORT`, `WEBUI_PUBLISHED_PORT`.
Set `COMPOSE_AUTONOMOUS=1` to include the autonomous overlay (`docker/compose.autonomous.yml`)
when running `tools/verify_compose_stack.sh`.
Broker compose smoke tests ignore inherited port envs unless you set
`AGENT_SMOKE_USE_PUBLISHED_PORTS=1`, to avoid cross-test contamination from
previous runs that export these variables.
Set `BROKER_SMOKE_FORCE_NEW_STACK=1` to skip reuse of detected stacks and
force a fresh compose stack (useful when testing new broker behaviors).
On macOS, Docker preflight will try to auto-start Docker Desktop when the daemon
is down. Set `AGENT_DOCKER_AUTOSTART=0` to disable, or tune the wait with
`AGENT_DOCKER_STARTUP_TIMEOUT_SEC` (default: 120 seconds).

Additional broker compose smokes:
- OIDC refresh sidecar + token file: `tests/broker_oidc_refresh_compose_smoke.sh`

### Connector status (broker registry)

To publish connector health into the broker registry:

```bash
curl -H "Authorization: Bearer ${BROKER_ADMIN_TOKEN}" \
  -H "Content-Type: application/json" \
  -d '{"status":"ready","last_error":"","ts_unix_ms":0}' \
  https://broker.example.com/v1/connectors/slack/status
```

Or use the helper:

```bash
agentd-connector-status \
  --broker-base https://broker.example.com \
  --auth-token "$BROKER_ADMIN_TOKEN" \
  --connector-id slack \
  --status ready \
  --interval 30s
```

## Spawn adapter (orchestrator provisioning)

The broker can persist spawn requests for new runtime members. The
`agentd-spawn-adapter` CLI polls those requests and fulfills them via a
command you provide.

Example (devstack + local adapter command):

```bash
# run a single poll cycle via helper script
SPAWN_COMMAND='printf "{\"status\":\"allocated\",\"assigned_members\":[{\"agent_id\":\"agent-1\",\"role\":\"planner\"}]}\n"' \
  tools/run_spawn_adapter_devstack.sh --once --insecure
```

Allocator mode (reuse connected agents, no custom command required):

```bash
SPAWN_ALLOCATOR=1 tools/run_spawn_adapter_devstack.sh --once --insecure
```

Notes:
- `SPAWN_COMMAND` runs via `/bin/sh -c` and must emit JSON to stdout.
- `SPAWN_ALLOCATOR=1` uses the broker runtime member allocator instead of a command.
- `BROKER_INSECURE_TLS=1` or `--insecure` is useful for local self-signed stacks.
- See `docs/spec/agent_spawn_adapter_v0.md` for the full contract.

## Autonomous orchestrator loop

`agentd-orchestrator` polls orchestrator runs, heartbeats leases, and dispatches
team runs (plus spawn requests when roles are missing).

Example (devstack + single poll cycle):

```bash
tools/run_orchestrator_devstack.sh --once --insecure
```

Full autonomous devstack (orchestrator + spawn adapter together):

```bash
tools/run_autonomous_devstack.sh --insecure
```

Notes:
- The loop honors `orchestrator_run.meta` (see `docs/spec/autonomous_orchestrator_v0.md`).
- `BROKER_INSECURE_TLS=1` or `--insecure` is useful for local self-signed stacks.
- Client auth tokens require broker `--client-auth-allow-automation`.

## Network smoke tests

`ctest` includes network smokes (OpenRouter + DeepSeek). They run when keys are available via:
- environment variables, or
- `.not_in_repo` (preferred, gitignored), or
- `project.local.md` (gitignored).
- `~/.env` (developer convenience; e.g. `DEEPSEEK_API_KEY=...`).

Disable all network tests:

```bash
export AGENT_DISABLE_NETWORK_TESTS=1
```

Skip only OpenRouter tests:

```bash
export AGENT_TEST_SKIP_OPENROUTER=1
```

Network tests assume an HTTP proxy may be required; the scripts default to `http://localhost:8120`
(or `http://host.docker.internal:8120` when running inside a container) via `https_proxy` / `http_proxy`.
Use `AGENT_TEST_DISABLE_PROXY=1` to bypass the proxy, or `AGENT_SMOKE_ALLOW_PROXY=1` to force proxy envs
through the shared curl wrapper in smoke scripts.
Compose-based broker smokes wait for an agent to connect; use `AGENT_SMOKE_AGENT_WAIT_SECS` to extend
the wait (default: 60s) if your Docker host is slow to start the connector.

Key file formats and precedence live in `docs/AGENTD.md` (search for “Secrets and local key files”).

## Real end-to-end (agentd + browser) test

This repo includes a real E2E harness that drives the Web UI in a headless browser (Playwright)
and makes live provider calls via `agentd`.

Prereqs:
- `.not_in_repo` populated with provider keys (or env vars set)
- `./build/agentd` built (`tools/verify.sh`)
- UI deps installed (`cd ui && npm install`)

Run:

```bash
tools/e2e_real.sh
```

Logs and Playwright artifacts are written under `build/e2e/`.

The Playwright suite includes heavier flows that are opt-in via env flags:
- `AGENT_E2E_REQUIRE_REAL=1` enables the real agent flow spec (live provider calls).
- `AGENT_E2E_REQUIRE_CANVAS=1` enables the canvas2d observe spec (open-world harness).
- `AGENT_E2E_REQUIRE_VOICE=1` enables the voice observe spec (open-world harness).

Playwright will auto-start a local Vite dev server for UI e2e runs unless
`AGENT_E2E_UI_BASE_URL` is set (use that env var to target an existing server).

The open-world harness scripts under `tests/` set these flags for you when they run.

To capture a full-page Teams layout screenshot from the broker console (uses the
mocked Playwright spec and writes the PNG to `out/`):

```bash
tools/capture_broker_team_layout.sh
```

Open-world harness entry points (stub provider, deterministic):

```bash
./tests/webui_observe_canvas2d_plot_openworld.sh
./tests/webui_observe_voice_hello_openworld.sh
```

## Smoke tests (local daemon)

`ctest` includes bash-based `agentd_*_smoke.sh` tests that start/stop the daemon; shared helpers live in
`tests/lib/agentd_smoke_lib.sh`.

Audio signaling smokes (`broker_audio_signal_docker_smoke`, `agentd_audio_signal_loopback_smoke`) normally spin up
a temporary Postgres via Docker. If Docker is unavailable but `initdb`/`pg_ctl` are usable, they attempt to launch a
local ephemeral Postgres instead. You can override with `AGENTD_TEST_PG_DSN` to point at any reachable Postgres DSN.
If local Postgres is misconfigured (e.g., missing `postgres.bki`), the tests will skip and print the reason.

## Cleanup and repo hygiene

Build artifacts and run logs can grow quickly (especially `build/`, `build-core*/`, and `out/`). Use the cleanup
tools to keep the repo lean and enforce size guardrails.

Docker cleanup (optional):

```bash
tools/clean_docker_agent_stacks.sh         # dry run (lists agent_* compose projects)
tools/clean_docker_agent_stacks.sh --apply # stop/remove all agent_* compose stacks
```

Basic cleanup:

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

Repo size reports:

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

List nested .git dirs:

```bash
tools/repo_size_report.py --list-nested-git
```

Guard scripts:

```bash
tools/stub_file_scan.py --fail
```

```bash
tools/tracked_file_guard.py --max-mb 10
```

```bash
tools/untracked_file_guard.py --exclude-defaults --max-mb 100
```

```bash
tools/vendored_guard.py --path ref
```

Run all repo hygiene guards locally:

```bash
tools/verify_repo_guards.sh
tools/verify_repo_guards.sh --strict
tools/verify_repo_guards.sh --max-total-gb 2 --max-file-mb 5
tools/verify_repo_guards.sh --max-untracked-mb 200
```

Vendored guard hooks:

```bash
git config core.hooksPath .githooks
tools/install_git_hooks.sh
```

Install the local pre-commit hook:

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

Skill transforms (auditable repo changes):

```bash
python3 tools/skills/apply_skill.py /path/to/skill
python3 tools/skills/apply_skill.py /path/to/skill --dry-run
python3 tools/skills/preview_skill.py /path/to/skill
```

Docs: `tools/skills/README.md` (manifest format + backups/state behavior).

Fail if the vendored guard is not installed:

```bash
tools/hooks_status.sh --check
```

Scriptable check with JSON:

```bash
tools/hooks_status.sh --json --check
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

## Git remote + publishing

If `git push` fails with “No configured push destination”, set `origin` explicitly:

```bash
git remote add origin <your_repo_url>
git push -u origin "$(git rev-parse --abbrev-ref HEAD)"
```

If `origin` exists but points to the wrong place, pass `--force`:

```bash
tools/setup_git_remote.sh --url "<your_repo_url>" --force --push
```

The helper does not guess a URL:

```bash
AGENT_GIT_REMOTE_URL="<your_repo_url>" tools/setup_git_remote.sh --push
```

It can also read `git_remote` from your gitignored `project.local.md`:

```bash
cp project.local.md.example project.local.md
# edit project.local.md and set:
# - git_remote: <your_repo_url>
tools/setup_git_remote.sh --push
```

To run a full local verify + push in one command:

```bash
tools/publish.sh --skip-ui
```
