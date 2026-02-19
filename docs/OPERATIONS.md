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

Compose image builds pass proxy args for package installs. Defaults:
- `HTTP_PROXY`/`HTTPS_PROXY`: `http://host.docker.internal:8120`
- `NO_PROXY`: `localhost,127.0.0.1,::1,host.docker.internal`
Use `host.docker.internal` (not an ad-hoc hostname like `m2`) so containers can resolve the proxy consistently.
If the proxy is unreachable during build, the Dockerfiles now retry apt installs once without proxy to avoid
hard failures (still prefers proxy when available).

Runtime containers do not receive proxy environment variables by default so intra-stack
traffic stays direct. Override by exporting `HTTP_PROXY`/`HTTPS_PROXY` before `docker compose`.

## Network smoke tests

`ctest` includes network smokes (OpenRouter + DeepSeek). They run when keys are available via:
- environment variables, or
- `.not_in_repo` (preferred, gitignored), or
- `project.local.md` (gitignored).

Disable all network tests:

```bash
export AGENT_DISABLE_NETWORK_TESTS=1
```

Skip only OpenRouter tests:

```bash
export AGENT_TEST_SKIP_OPENROUTER=1
```

Network tests assume an HTTP proxy may be required; the scripts default to `http://localhost:8120`
via `https_proxy` / `http_proxy`. Use `AGENT_TEST_DISABLE_PROXY=1` to bypass the proxy.

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
