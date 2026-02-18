# Tool Servers (stdio) — out-of-process tools for `agentd`

`agentd` can load tools from out-of-process “tool servers” using a strict stdin/stdout **JSON-lines** protocol.

Platform support: Linux/macOS only (uses POSIX process + poll). Windows builds compile without tool server support.

Why this matters for “power unleashed”:
- Keeps `agentd` stable: tool crashes don’t crash the daemon (process boundary).
- Enables ecosystem growth: Playwright/browser automation, AVM policy runners, device bridges (serial/MQTT) can live out-of-process.
- Avoids embedding complexity until the interface is proven.

## Enable

Start `agentd` with one or more tool servers:

```bash
./build/agentd --tools basic \
  --tool-server-cmd "python3 -u ./tests/tool_server_echo.py" \
  --tool-server-timeout-ms 30000 \
  --tool-server-max-line-bytes $((4*1024*1024)) \
  --tool-server-ping-interval-ms 0
```

Tool servers are an **extension** mechanism: their tools are appended to the selected base toolset (`basic` or `host`).

Notes:
- `--tool-server-timeout-ms` and `--tool-server-max-line-bytes` are **per-server** and must appear *after* the corresponding
  `--tool-server-cmd` (they apply to “the most recently declared tool server”).
- `--tool-server-max-line-bytes` bounds both the server’s JSON response line and any buffered partial line (fail-closed).
- `--tool-server-ping-interval-ms` is optional (default disabled). If enabled, `agentd` may send an `op:"ping"` after idle periods
  to detect a dead/hung server before executing a real tool call. If a server responds with `ok:false` and error contains
  `"unknown op"`, ping is treated as unsupported and automatically disabled for that server.

## Protocol (newline-delimited JSON objects)

All messages are single-line JSON objects terminated by `\n`.

### Manifest

Request:

```json
{"id":1,"op":"manifest"}
```

Response (example):

```json
{"id":1,"ok":true,"tools":[{"name":"server_echo","description":"...","parameters":{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}}]}
```

Accepted `tools[]` entries:
- `name` (string, required)
- `description` (string, optional)
- `parameters` (JSON Schema object) **or** `parameters_json` (stringified JSON Schema)

Protocol hardening:
- The response **must** include the same `id` as the request. Mismatches are treated as a protocol error and the server is killed.
- If the response is malformed (invalid JSON or exceeds `max_line_bytes`), the tool call error includes `protocol_violation:true`.

### Execute

Request:

```json
{"id":2,"op":"execute","tool_name":"server_echo","arguments":{"text":"hello"}}
```

Response:

```json
{"id":2,"ok":true,"tool_result":{"ok":true,"data":{"echo":"hello"}}}
```

Notes:
- `tool_result` may be an object or a string. `agentd` returns the stringified `tool_result` as the tool output.
- Keep stderr separate from stdout (stdout is reserved for JSON-lines responses).
- If the server dies, `agentd` will restart it with bounded backoff for the *next* tool call (it does **not** auto-retry the same call).

### Ping (optional)

Request:

```json
{"id":3,"op":"ping"}
```

Response:

```json
{"id":3,"ok":true,"pong":true}
```

## Deterministic testing

`ctest` includes `agentd_tool_server_smoke`, which uses an OpenAI-compatible stub provider that forces a tool call to `server_echo`,
then checks that `agentd` routes the call to the tool server and returns the expected tool result.

## Plugin host (sandboxed plugins)

The `agentd_tool_plugin_host` helper exposes tool plugins over the tool server protocol, so you can run plugins out-of-process:

```bash
./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"
```

The plugin host rejects tool results larger than 4 MiB before JSON parsing to keep memory usage bounded.
