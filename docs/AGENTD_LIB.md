# Embedding `agentd` as a Library (Sidecar Mode)

This repo now builds the daemon as:

- `agentd_lib` (static library): the full agentd implementation
- `agentd` (executable): a thin CLI entrypoint that links `agentd_lib`

This enables desktop/server apps to embed agentd **in-process** and optionally inject additional tools.

## Build

`agentd_lib` is available when host components are enabled (`AGENT_BUILD_HOST=ON`).

```sh
cmake -S . -B build
cmake --build build -j
```

Targets:
- `agentd_lib`
- `agentd` (only when `-DAGENTD_ENABLE_HTTP=ON`)

### Disable the built-in HTTP server

For app embedding where you don't want to bind a local port (e.g., you use a cloud relay/broker or a custom transport),
build without the socket-based HTTP server:

```sh
cmake -S . -B build-nohttp -DAGENTD_ENABLE_HTTP=OFF
cmake --build build-nohttp -j
```

## Primary API: `agentd::AgentdService`

Header:

- `daemon/include/agentd/service.h`

Note: `AgentdService` is only available when `AGENTD_ENABLE_HTTP=ON`.

The service owns:
- SQLite DB (`AgentDb`)
- runtime config store (`DaemonConfigStore`)
- HTTP server (`HttpServer`)

### Lifecycle

- `init(out_error)`:
  - fills best-effort env defaults (base_url/api_key/model/db_path/state dirs)
  - ensures `db_path` defaults to `./agentd.db` if empty
  - opens DB and loads runtime config from DB
  - registers HTTP routes

- `serve_blocking(out_error)`:
  - blocks in the accept loop until `stop()` is called

- `start_background(out_error)`:
  - starts `serve_blocking()` on an internal thread

- `stop()`:
  - stops the HTTP server and joins the background thread (if any)

### Minimal embedding example

```cpp
#include "agentd/service.h"

int main() {
  agentd::DaemonConfig cfg;
  cfg.listen_host = "127.0.0.1";
  cfg.listen_port = 8123;
  cfg.db_path = "./agentd.db";
  cfg.cors_disabled = true; // typical for non-browser embedding

  agentd::AgentdService svc(agentd::AgentdService::Options{cfg});
  std::string err;
  if (!svc.start_background(&err)) {
    // handle error
    return 1;
  }

  // ... host app runs ...

  svc.stop();
  return 0;
}
```

## Tool extension API

`agentd` supports an optional `ToolExtension` injection point:

- `ToolExtension::register_tools(ctx, registry)` is called after the base toolset (`basic` or `host`) is created.
- Tools added by the extension are dispatched to `ToolExtension::execute_tool(...)`.

Contract:
- `register_tools` should **only append tools** to the registry.
- `execute_tool` must handle the tools added by `register_tools`.
- The extension callbacks must remain valid for the service lifetime.

### Example: add a device-specific tool

```cpp
static agent_status_t register_my_tools(void*, agent_tool_registry_t* reg) {
  return agent_tool_registry_add(
    reg,
    "device_beep",
    "Play a short beep on the host device",
    "{\"type\":\"object\",\"properties\":{\"ms\":{\"type\":\"integer\"}},\"required\":[\"ms\"]}"
  );
}

static agent_status_t exec_my_tool(void*, const char*, const char*, agent_string_t* out) {
  const char* ok = "{\"ok\":true}";
  return agent_string_set_copy(out, ok, strlen(ok));
}

agentd::ToolExtension ext;
ext.ctx = nullptr;
ext.register_tools = register_my_tools;
ext.execute_tool = exec_my_tool;

agentd::AgentdService::Options opt;
opt.cfg = cfg;
opt.enable_tool_extension = true;
opt.tool_extension = ext;

agentd::AgentdService svc(opt);
```

## Notes / caveats

- The HTTP server is intentionally minimal (no TLS, no chunked encoding). For production remote exposure, put a reverse proxy in front or extend `HttpServer`.
- `stop()` now closes the listening socket to ensure `serve()` unblocks promptly (important for embedding and tests).
- Tool execution semantics are still controlled by `tools=host|basic|none` and safety knobs (`yolo`, `host_policy`, `tools_root`) at runtime.

## Transport-agnostic API (`AgentdApi`)

If you want to drive agentd over a non-HTTP transport (MQTT, cloud relay, custom IPC), you can avoid the socket server
and call the daemon endpoints directly through `AgentdApi`:

- `daemon/include/agentd/api.h`

It accepts/returns `HttpRequest`/`HttpResponse` objects (`daemon/include/agentd/http_types.h`), which are intentionally
“HTTP-shaped” so a transport can map its messages onto the same endpoint handlers without rewriting the daemon logic.
