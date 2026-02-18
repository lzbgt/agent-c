# Tool Plugins (Runtime-Loaded Tools) — Draft

Date: 2026-02-03

This document defines the **tool plugin ABI** used by the standalone `agentd` executable via:

- `agentd --tool-plugin /path/to/plugin.so` (repeatable)
- Windows: `agentd --tool-plugin C:\path\to\plugin.dll`
- `agentd --tool-plugin-config '{"key":"value"}'` (applies to the most recent `--tool-plugin`)

The goal is to make this project a **framework**: adding tools should be a packaging/configuration operation, not a fork/rebuild operation.

## Scope / goals

- Support host and embedded targets, with a compatible plugin/extension path for MCUs via `agent_core`.
- Provide a sandboxed execution option for plugins (process isolation, policy, and resource limits).
- Support Windows in v1 with a LoadLibrary-based loader and packaging guidance.

## Plugin ABI (v1)

`agentd` loads a shared library and resolves the following C symbols.

### Required: tool manifest

```c
const char* agentd_tool_plugin_manifest_json(void);
```

Returns a UTF-8 JSON document describing the tools exported by the plugin.

Optional (config-aware) manifest:

```c
const char* agentd_tool_plugin_manifest_json_ex(const char* config_json);
```

If present, `agentd` will call this with the plugin config JSON (or `NULL` if none). Otherwise it falls back
to `agentd_tool_plugin_manifest_json()`.

Accepted shapes:
- `{"ok":true,"tools":[ ... ]}`
- or a raw array `[ ... ]`

Each tool entry is an object:

```json
{
  "name": "my_tool",
  "description": "optional",
  "parameters_json": "{\"type\":\"object\",...}"
}
```

Alternatively, `parameters` may be provided as a JSON object/array, and `agentd` will stringify it:

```json
{
  "name": "my_tool",
  "description": "optional",
  "parameters": { "type": "object", "properties": { ... }, "required": [ ... ] }
}
```

### Required: tool execution

```c
char* agentd_tool_plugin_execute_json(const char* tool_name, const char* arguments_json);
```

Executes `tool_name` with the raw `arguments_json` blob (OpenAI-compatible tool-call arguments) and returns a newly allocated UTF-8 JSON string.

Optional (config-aware) execution:

```c
char* agentd_tool_plugin_execute_json_ex(
  const char* tool_name,
  const char* arguments_json,
  const char* config_json
);
```

If present, `agentd` will pass the plugin config JSON (or `NULL` if none). Otherwise it falls back to
`agentd_tool_plugin_execute_json()`.

The returned string must be a JSON value; recommended shape:

```json
{ "ok": true, "result": { ... } }
```

On failure, return a JSON object with `ok:false` and an `error` string:

```json
{ "ok": false, "error": "reason" }
```

### Required: free

```c
void agentd_tool_plugin_free(char* p);
```

Frees the pointer returned by `agentd_tool_plugin_execute_json`.

## Example

This repo includes a minimal demo plugin:

- `tools/agentd_tool_plugin_echo.c`

It exports a single tool `ext_echo` that returns a small JSON response.

## How `agentd` dispatches plugin tools

- The daemon builds its baseline toolset (`tools=host|basic|none`).
- During toolset construction, it calls the plugin chain `register_tools(...)` and appends plugin tool schemas.
- During execution, only tool names appended by plugins are dispatched to plugins (base tools still run in the built-in host toolset).

## Sandboxed execution (tool server host)

To isolate plugins in a separate process, run them behind the tool server protocol using the
`agentd_tool_plugin_host` helper:

```bash
./build/agentd --tools basic \
  --tool-server-cmd "./build/agentd_tool_plugin_host --plugin ./build/libagentd_tool_plugin_echo.(so|dylib|dll) --plugin-config '{\"tag\":\"isolated\",\"policy\":{\"limits\":{\"wall_ms\":60000,\"cpu_ms\":60000}}}'"
```

This keeps plugin crashes contained and lets you apply tool-server timeouts and line-size limits.
On Windows, tool servers are still disabled, so this isolation mode is Linux/macOS only.

## Config JSON

Plugin config JSON is passed verbatim to the optional `*_ex` symbols. It is validated as JSON before plugin load.
Use this to parameterize tool behavior without rebuilding or forking the daemon.
