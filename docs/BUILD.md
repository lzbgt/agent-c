# Build Guide

This guide covers build steps, platform dependencies, and smoke-test notes.

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

## Smoke tests

`ctest` includes bash-based `agentd_*_smoke.sh` tests that start/stop the daemon; shared helpers live in
`tests/lib/agentd_smoke_lib.sh`.

Audio signaling smokes (`broker_audio_signal_docker_smoke`, `agentd_audio_signal_loopback_smoke`) normally spin up
a temporary Postgres via Docker. If Docker is unavailable but `initdb`/`pg_ctl` are usable, they attempt to launch a
local ephemeral Postgres instead. You can override with `AGENTD_TEST_PG_DSN` to point at any reachable Postgres DSN.
If local Postgres is misconfigured (e.g., missing `postgres.bki`), the tests will skip and print the reason.
