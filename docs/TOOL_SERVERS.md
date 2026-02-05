# Tool Servers (stdio) — out-of-process tools for `agentd`

`agentd` can load tools from out-of-process “tool servers” using a strict stdin/stdout **JSON-lines** protocol.

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
  --tool-server-max-line-bytes $((4*1024*1024))
```

Tool servers are an **extension** mechanism: their tools are appended to the selected base toolset (`basic` or `host`).

Notes:
- `--tool-server-timeout-ms` and `--tool-server-max-line-bytes` are **per-server** and must appear *after* the corresponding
  `--tool-server-cmd` (they apply to “the most recently declared tool server”).
- `--tool-server-max-line-bytes` bounds both the server’s JSON response line and any buffered partial line (fail-closed).

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

## Deterministic testing

`ctest` includes `agentd_tool_server_smoke`, which uses an OpenAI-compatible stub provider that forces a tool call to `server_echo`,
then checks that `agentd` routes the call to the tool server and returns the expected tool result.
