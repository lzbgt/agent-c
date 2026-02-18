# Platform Support Matrix

This repo targets **desktop/server** environments. `agentd` is intended for Windows, Linux, and macOS. `agent_core` is intended for embedded/MCU.

## Agentd (daemon)

| Feature | Linux | macOS | Windows |
|---|---|---|---|
| Core HTTP API + WebUI (daemon-only) | ✅ | ✅ | ✅ |
| SQLite state (`--db-path`) | ✅ | ✅ | ✅ |
| Tool plugins (`--tool-plugin`) | ✅ | ✅ | ✅ |
| Tool servers (`--tool-server-cmd`) | ✅ | ✅ | ❌ |
| AVM endpoints (`/api/v1/avm/*`) | ✅ | ✅ | ❌ (501) |

**Windows notes**
- The HTTP server is Winsock-based (no POSIX socket calls).
- Tool plugins use `LoadLibrary` on Windows; tool servers remain disabled (no POSIX process/poll).
- AVM endpoints return 501 (unsupported) on Windows.
- Validation: run `tools/verify_windows_build.ps1` on a Windows host (optionally with `VCPKG_ROOT` set; `-InstallDeps` can bootstrap vcpkg + deps).
- CI check (host-independent): `tools/check_ci_windows_build.sh` prints the latest workflow status (uses GitHub API; set `GITHUB_TOKEN` or `GH_TOKEN` for private repos).
- CI trigger: `tools/trigger_ci_windows_build.sh [ref]` dispatches the workflow (requires token or logged-in `gh`).

## Broker + Connector

The broker and connector are Go binaries and are portable across Windows/Linux/macOS. Production deployments typically run them in Linux containers.

## WebUI

The WebUI is a static site (Vite build). It can be hosted by any static web server (nginx, Caddy, S3/CloudFront, etc.).
