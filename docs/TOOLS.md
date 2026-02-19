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

## Tool servers (out-of-process)

Enable one or more tool servers:

./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tests/tool_server_echo.py" \
  --tool-server-timeout-ms 30000 \
  --tool-server-max-line-bytes $((4*1024*1024)) \
  --tool-server-ping-interval-ms 0

Notes:
- tool-server flags are per-server and apply to the most recently declared
  --tool-server-cmd
- max_line_bytes bounds both the JSON response line and buffered partial lines
- optional ping (op:"ping") can be enabled to detect dead servers

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

### Deterministic testing

ctest includes agentd_tool_server_smoke, which routes a forced tool call to a
server and validates the response.

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

## Sandboxed execution (plugin host)

Use the tool server protocol to run plugins out-of-process:

./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"

This provides process isolation plus tool-server timeouts and line limits.

## References

- tests/tool_server_echo.py (example tool server)
- tools/agentd_tool_plugin_echo.c (example plugin)
- docs/spec/tool_plugins_sandbox_v0.md (sandbox policy details)
