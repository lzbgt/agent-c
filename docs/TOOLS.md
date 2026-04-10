# Tools: Plugins and Tool Servers (Living)

Date: 2026-02-19

This document consolidates the tool extension mechanisms for `agentd`.
It replaces:
- docs/TOOL_SERVERS.md
- docs/TOOL_PLUGINS.md

## Overview

There are two extension paths:

1) Tool plugins (in-process, shared library ABI)
2) Tool servers (out-of-process, JSON-lines over stdio)

Use plugins when you need low-latency calls and can trust the plugin process.
Use tool servers when you want isolation, independent dependencies, or a
separate runtime (Playwright, device bridges, AVM runners, etc.).

Platform support:
- Tool plugins: Linux, macOS, Windows
- Tool servers: Linux and macOS only (POSIX process + poll)

## Built-in host toolset

These tools are available when `--tools host` is enabled (subject to `yolo` and `host_policy`):

- `shell_exec` (runs `/bin/sh -lc <cmd>`, returns JSON envelope with `exit_code`, `timed_out`, `truncated`, `output`)
- `proc_exec` (runs an argv array via `posix_spawnp`, no shell; returns JSON envelope with `argv`, `exit_code`, `timed_out`, `truncated`, `output`)
- `file_apply_patch` (applies a unified diff via `git apply`; returns the patch as a diff-style audit trail)
- `fs_stat` (file/dir metadata; returns structured fields + a human-readable `output`)
- `fs_list` (bounded directory listing; returns structured `entries` + `output`)
- `fs_find` (bounded file discovery; returns structured `entries` + `output`)
- `fs_read` (bounded file read with pagination by line; returns `content`/`output` + `has_more` + `next_start_line`)
- `text_search` (token-safe substring search; returns structured `matches` + `output`)
- `ui_action` (typed UI-side action requests; see `docs/CLIENT.md`)

Host policy notes:
- In `--host-policy readonly`, the registry omits `shell_exec`, `proc_exec`, and `file_apply_patch`.
- In daemon restricted mode (`yolo=false`), the registry omits `shell_exec` and `proc_exec` even when `host_policy=full`.

## Token-safety guidance (filesystem tools)

- Prefer `fs_list` / `fs_read` / `fs_stat` for bounded output and pagination.
- Prefer `fs_find` over `find`/`tree` when you need predictable output size and default excludes.
- Prefer `text_search` over `grep -R` when you need predictable output size (bounded matches + per-file size limits).
  - Use `extensions` (e.g. `[".cpp",".h"]`) to restrict scanning.
- `fs_list`, `fs_find`, and `text_search` support `exclude_globs` (fnmatch) to filter noisy paths.
- `fs_list`, `fs_find`, and `text_search` support `respect_gitignore: true` (best-effort; reads repo-root `.gitignore`).
- `fs_stat` supports an optional bounded line count (`count_lines: true`) for small text files.
- `fs_read` supports paging (`start_line`, `max_lines`, `end_line`) and a character cap (`max_chars`).

## Tool servers (out-of-process)

Enable one or more tool servers:

./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tests/tool_server_echo.py" \
  --tool-server-timeout-ms 30000 \
  --tool-server-max-line-bytes $((4*1024*1024)) \
  --tool-server-ping-interval-ms 0

Broker runtime member tool server example (updates a team run's runtime members):

```bash
export BROKER_BASE_URL="https://broker.example.invalid"
export BROKER_AUTH_TOKEN="...bearer..."

./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tools/tool_server_broker_runtime_members.py"
```

Tool call shape:

```json
{
  "team_id": "team-alpha",
  "team_run_id": "run-123",
  "mode": "merge",
  "runtime_members": [
    {"member_id":"rt-1","agent_id":"agent-a","role":"executor"}
  ]
}
```

Notes:
- tool-server flags are per-server and apply to the most recently declared
  --tool-server-cmd
- max_line_bytes bounds both the JSON response line and buffered partial lines
- optional ping (op:"ping") can be enabled to detect dead servers
- child tool-server exec now closes inherited non-stdio fds before `exec`, so
  an orphaned helper cannot accidentally keep agentd listener sockets open
  across an ungraceful daemon restart

### Protocol (JSON-lines)

All messages are single-line JSON objects terminated by \n.

Manifest request:

{"id":1,"op":"manifest"}

Manifest response:

{"id":1,"ok":true,"tools":[{"name":"server_echo","description":"...","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}]}

Execute request:

{"id":2,"op":"execute","tool_name":"server_echo","arguments":{"text":"hello"}}

Execute response:

{"id":2,"ok":true,"tool_result":{"ok":true,"data":{"echo":"hello"}}}

Protocol hardening:
- response id must match request id
- malformed JSON or oversized lines are treated as protocol violations
- stderr is reserved for logs (stdout is JSON-lines only)

## Sandbox mount validation

Operators can validate whether a host path may be mounted into a sandboxed tool runner
via the mount allowlist:

```bash
curl -sS -X POST "http://127.0.0.1:60306/api/v1/sandbox/mount_validate" \
  -H "Content-Type: application/json" \
  -d '{"host_path":"/Users/you/Documents","container_path":"/workspace/extra/docs","container_prefix":"/workspace/extra","is_main":true}'
```

This endpoint does not execute a sandbox; it returns an allow/deny decision and
whether the mount must be read-only.

The same allowlist is enforced for AVM capsule execution when `capsule.mounts`
is present on `/api/v1/avm/capsule_run` or an `avm_capsule` workflow task.
Validated mounts are forwarded to AVM-aware runners through
`AGENTD_AVM_MOUNTS_JSON` and `AGENTD_AVM_MOUNT_<n>_*` env vars; if the runner
ignores those vars, no host mount is exposed.

### Deterministic testing

ctest includes:
- `agentd_tool_server_smoke` for registration + basic tool execution
- `agentd_tool_server_ping_smoke` for idle ping health checks
- `agentd_tool_server_restart_smoke` for both restart-with-backoff after child
  death and same-port daemon restart after an orphaned tool-server child

## Tool plugins (in-process ABI)

The tool plugin ABI allows `agentd` to load shared libraries at runtime.

Enable:
- agentd --tool-plugin /path/to/plugin.so (repeatable)
- agentd --tool-plugin-config '{"key":"value"}' (applies to most recent plugin)

### Required symbols

Manifest:

const char* agentd_tool_plugin_manifest_json(void);

Optional config-aware manifest:

const char* agentd_tool_plugin_manifest_json_ex(const char* config_json);

Execution:

char* agentd_tool_plugin_execute_json(const char* tool_name, const char* arguments_json);

Optional config-aware execution:

char* agentd_tool_plugin_execute_json_ex(const char* tool_name, const char* arguments_json, const char* config_json);

Free:

void agentd_tool_plugin_free(char* p);

### Manifest shapes

Accepted manifest shapes:
- {"ok":true,"tools":[ ... ]}
- [ ... ] (raw array)

Each tool entry:

{
  "name": "my_tool",
  "description": "optional",
  "parameters_json": "{\"type\":\"object\",...}"
}

You may also provide parameters as a JSON object under "parameters".

### Execution return shape

Recommended:
- {"ok": true, "result": { ... }}
- {"ok": false, "error": "reason"}

### Size limits

- manifest capped at 1 MiB
- tool result capped at 4 MiB

## Embedded/MCU tool plugins (compile-time)

Embedded targets typically cannot load shared libraries. For `agent_core` builds, use a
compile-time plugin list that registers tool schemas into `agent_tool_registry_t` and
dispatches via `agent_tool_executor_t`. See:

- `docs/spec/tool_plugins_embedded_v0.md` (implemented rolling ABI + constraints)

## Sandboxed execution (plugin host)

Use the tool server protocol to run plugins out-of-process:

./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"

This provides process isolation plus tool-server timeouts and line limits.

Current proof points:
- `tests/agentd_tool_plugin_smoke.sh`
- `tests/agentd_tool_plugin_server_smoke.sh`
- `tests/agentd_tool_plugin_big_smoke.sh`
- `tests/agentd_tool_plugin_server_big_smoke.sh`
- `tests/test_tool_plugin_host_limits.cpp` (Windows host-limit validation surface)
- `tools/verify_windows_build.ps1` (Windows build/test helper for plugin loader path)

## Builtin voice media provider inspection

The experimental builtin WebRTC native-provider seam is a separate C ABI from the
tool-plugin ABI above. Operators can inspect a candidate builtin voice media
provider shared library without starting `agentd`:

```bash
python3 tools/inspect_voice_media_provider.py \
  ./build/libagentd_voice_builtin_media_engine_sample.dylib --pretty
```

The tool validates the exported provider symbol (`agentd_voice_media_engine_get_api_v2`
preferred, `..._v1` as compatibility fallback), reports ABI version and required
callback presence, and prints provider metadata such as `name`, `version`, and
declared `capabilities`.

## Native voice media stack readiness

Before attempting a real embedded WebRTC/SRTP backend, inspect the local build
machine for the required native dependencies:

```bash
python3 tools/check_voice_native_media_stack.py --pretty
```

The report checks the local `pkg-config` surface plus Homebrew formula
availability for the current candidate dependency set:

- `opus`
- `portaudio`
- `srtp`
- `libusrsctp`
- `libjuice`
- `libdatachannel`

It also summarizes whether the machine is currently ready for the narrower
`libjuice + srtp + libusrsctp + opus + portaudio` candidate path without
guessing from memory.

## References

- tests/tool_server_echo.py (example tool server)
- tools/agentd_tool_plugin_echo.c (example plugin)
- docs/spec/tool_plugins_sandbox_v0.md (sandbox policy details)
