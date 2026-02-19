# Embedded Tool Plugins (v0)

Date: 2026-02-19
Status: draft (not implemented)

## Goals

- Provide a **compile-time** tool plugin ABI for embedded/MCU builds that cannot load shared libraries.
- Reuse the existing `agent_core` tool registry/executor surfaces (`agent_tool_registry_t`, `agent_tool_executor_t`).
- Keep memory usage bounded and deterministic (static strings, caller-owned allocation).
- Make it easy to share tool implementations between embedded `agent_core` and host-side `agentd` where feasible.

## Non-goals

- Dynamic loading of shared libraries on MCU targets.
- Process isolation/sandboxing on MCU targets (no fork/exec).
- Replacing the JSON-lines tool server protocol (still used for host isolation).

## Constraints (existing behavior)

- The core treats **tool schema** and **tool arguments** as opaque JSON strings; it does not parse JSON.
- Embedded builds may provide a custom allocator via `agent_set_allocator()`.
- `agent_tool_execute_fn` returns tool results via `agent_string_t` (caller-owned allocation).
- The tool loop already enforces caps such as `max_tool_call_args_chars` and `max_tool_result_chars`.

## Design (v0)

### ABI surface (header proposal)

A small C header (e.g. `core/include/agent/tool_plugin_embedded.h`) defines:

```c
typedef struct agent_tool_plugin_v0_def {
  const char* name;              // required
  const char* description;       // required (may be empty)
  const char* parameters_json;   // required (OpenAI-compatible JSON Schema)
  agent_tool_execute_fn execute; // required
  void* ctx;                     // optional
} agent_tool_plugin_v0_def_t;

typedef struct agent_tool_plugin_v0 {
  const agent_tool_plugin_v0_def_t* defs;
  size_t def_count;
} agent_tool_plugin_v0_t;

// Required symbol for embedded/MCU builds (static linkage):
const agent_tool_plugin_v0_t* agent_tool_plugin_v0(void);
```

Notes:
- All strings are **static** (flash/rodata) and must remain valid for the lifetime of the process.
- Execution uses `agent_tool_execute_fn` so the tool loop can dispatch directly.
- `ctx` is a plugin-owned pointer (often a static struct).

### Registration flow

1) Host (embedded firmware or gateway) calls `agent_tool_plugin_v0()` to get the tool list.
2) For each tool, the host registers the schema into `agent_tool_registry_t`.
3) The host sets a single `agent_tool_executor_t` that dispatches to the tool’s `execute` function.

This keeps `agent_core` unchanged while providing a deterministic, compile-time tool list.

### Optional config (v0.1 candidate)

If config is required, add an optional init hook:

```c
typedef agent_status_t (*agent_tool_plugin_v0_init_fn)(
  const char* config_json,
  void** out_ctx
);
```

The host can pass config JSON from firmware settings or build-time constants.

### Limits and safety

- Tool execution is still bounded by the **tool loop** limits (`max_tool_call_args_chars`,
  `max_tool_result_chars`, and tool-call limits). Plugins must return compact JSON results.
- Embedded integrations should prefer `disable_tool_records=1` and keep hooks NULL to reduce allocations.

### Host/sandbox policy

- **MCU/embedded:** in-process only. The tool list is trusted at build time; no sandboxing.
- **Host (agentd):** prefer `agentd_tool_plugin_host` for isolation and use existing limits
  (`--limit-cpu-ms`, `--limit-wall-ms`, `--limit-as-mb`) when running untrusted plugins.
- **Gateway/edge devices:** choose based on deployment trust; if plugins are untrusted, use the host.

## Compatibility and migration

- Existing shared-library plugins (JSON ABI) remain supported for desktop/server.
- Embedded plugins use the **native executor ABI** and can share tool logic with host plugins by
  exposing a thin wrapper that adapts JSON arguments to the internal implementation.

## Open questions

- Should we add per-tool limits (result bytes, wall/cpu) to the embedded ABI, or keep those in
  the host/tool-loop config only?
- Do we need a standardized “tool capability” descriptor for MCU tools (e.g. idempotent, side-effectful)?
- Should we provide a helper that builds a tool registry directly from `agent_tool_plugin_v0_t` to reduce boilerplate?
