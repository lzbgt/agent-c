# Tool Plugins (Runtime-Loaded Tools) — Draft

Date: 2026-02-03

This document defines the **tool plugin ABI** used by the standalone `agentd` executable via:

- `agentd --tool-plugin /path/to/plugin.so` (repeatable)

The goal is to make this project a **framework**: adding tools should be a packaging/configuration operation, not a fork/rebuild operation.

## Scope / non-goals

- This is a **host-only** feature (desktop/server). MCUs should use `agent_core` and compile tools in.
- This is not a “sandbox”. A plugin has native code execution; treat it like installing any other native extension.
- Windows support is not guaranteed in v1 (dlopen-based loader).

## Plugin ABI (v1)

`agentd` loads a shared library and resolves the following C symbols.

### Required: tool manifest

```c
const char* agentd_tool_plugin_manifest_json(void);
```

Returns a UTF-8 JSON document describing the tools exported by the plugin.

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

