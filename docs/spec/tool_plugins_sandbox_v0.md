# Tool Plugin Sandbox (v0)

Date: 2026-02-18

## Goals

- Run tool plugins out-of-process to isolate crashes and memory leaks.
- Preserve the existing tool schema surface (OpenAI-compatible JSON Schema).
- Reuse the existing tool server JSON-lines protocol for simplicity.
- Keep agentd stable: if the plugin process dies, the daemon stays up and can restart the tool server on the next call.

## Design (v0)

### Process boundary

`agentd` already supports tool servers (`--tool-server-cmd`) using a strict stdin/stdout JSON-lines protocol.
We leverage this to sandbox plugins by running them in a helper process:

- `agentd_tool_plugin_host` loads one or more plugins (same ABI as `--tool-plugin`)
- The host serves the tool server protocol:
  - `manifest`: returns plugin tool schemas
  - `execute`: dispatches to the plugin chain
  - `ping`: optional keep-alive

### CLI wiring

Operators can switch from in-process plugins to isolated plugins by using:

```
--tool-server-cmd "./build/agentd_tool_plugin_host --plugin <path> [--plugin-config <json>] ..."
```

This keeps plugin loading out of the daemon process while retaining the same tool schemas and tool names.

### Failure modes

- If the plugin host crashes, `agentd` treats it as a tool server failure and will restart it for the next tool call
  (bounded by the existing backoff policy).
- Tool server timeouts and max-line limits apply to plugin-host responses.
- The plugin host rejects tool results larger than 4 MiB before JSON parsing to keep memory usage bounded.

### Resource limits (best-effort)

The plugin host can apply basic process-level limits:

- `--limit-cpu-ms` (CPU time via `RLIMIT_CPU`)
- `--limit-wall-ms` (wall-clock watchdog; exits with code 124)
- `--limit-as-mb` (address space via `RLIMIT_AS`)

If a limit is requested but cannot be applied, the host exits with an error.

Limits can also be supplied via the plugin config JSON:

```json
{
  "policy": {
    "limits": {
      "cpu_ms": 60000,
      "wall_ms": 60000,
      "as_mb": 1024
    }
  }
}
```

When both CLI limits and config limits are present, the host enforces the **most restrictive** (minimum) values.

## Follow-up goals (tracked in `TODOS.md`)

- Policy-based isolation: per-plugin limits (CPU/memory), optional seccomp/AppArmor profiles (Linux).
- Windows tool server support (CreateProcess + async pipes), enabling sandboxed plugins on Windows.
- Explicit plugin attestation/signature checks for production deployments.
