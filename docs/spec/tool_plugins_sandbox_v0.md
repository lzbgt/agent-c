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

## Follow-up goals (tracked in `TODOS.md`)

- Policy-based isolation: per-plugin limits (CPU/memory), optional seccomp/AppArmor profiles (Linux).
- Windows tool server support (CreateProcess + async pipes), enabling sandboxed plugins on Windows.
- Explicit plugin attestation/signature checks for production deployments.
