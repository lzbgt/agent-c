# Embedded C Integration (ESP32-S3)

This repo contains a **portable, pure-C core library** (`agent_core`) intended to run on embedded targets (including ESP32-class MCUs) as long as you provide:

- a **tool-capable LLM provider** (`agent_tool_provider_t`) that can produce tool calls and assistant text
- a **tool executor** (`agent_tool_executor_t`) that runs your device tools (GPIO, sensors, display, etc.)
- (optionally) a custom allocator via `agent_set_allocator()` to control memory

The core library **does not** perform HTTP/JSON parsing for you on embedded. That is deliberate: networking and JSON stacks vary wildly across embedded environments.

For a maturity/feasibility assessment and architecture options (remote agent vs on-device), see:
- `docs/ESP32S3_AGENT_CORE_MATURITY.md`

## Build: core-only (no host deps)

To build only the embedded-safe core (no libcurl / jsoncpp / daemon / UI):

```sh
cmake -S . -B build-core -DAGENT_BUILD_HOST=OFF
cmake --build build-core -j
```

This produces the `agent_core` static library.

## Memory model

All heap allocations inside the core go through `agent_malloc()` / `agent_free()`.
You can override these globally at process startup:

```c
#include "agent/agent.h"

static void* my_malloc(size_t n) { /* arena/heap */ }
static void my_free(void* p) { /* arena/heap */ }

void app_init_allocator(void) {
  agent_allocator_t a = { .malloc_fn = my_malloc, .free_fn = my_free };
  (void)agent_set_allocator(&a);
}
```

On ESP32-S3, a common pattern is:
- one arena allocator for the agent task (fast reset between runs)
- a small fallback heap allocator for long-lived objects you keep across runs

## Core data types (what you need to implement)

### 1) Tool registry

Tools are declared as OpenAI-compatible JSON schema blobs, but the core treats them as opaque strings.

```c
#include "agent/tools.h"

agent_tool_registry_t* tools = NULL;
agent_tool_registry_create(&tools);
agent_tool_registry_add(tools, "gpio_write", "Set a GPIO pin high/low",
  "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\"}},\"required\":[\"pin\",\"value\"]}"
);
```

### 2) Tool executor (runs on-device tools)

The executor must return an `agent_string_t` result. The simplest way is to build the output using `agent_string_set_copy()` (which allocates via the active allocator):

```c
#include "agent/tools.h"

static agent_status_t exec_tool(
  void* ctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  (void)ctx;
  // Parse arguments_json (your JSON lib) and do work...
  // Return a compact result; smaller is cheaper in tokens.
  const char* ok = "{\"ok\":true}";
  return agent_string_set_copy(out_result, ok, strlen(ok));
}

static agent_tool_executor_t make_executor(void) {
  agent_tool_executor_t ex = {0};
  ex.ctx = NULL;
  ex.execute = exec_tool;
  return ex;
}
```

### 3) Tool-capable LLM provider (your network / model adapter)

The provider is responsible for calling your LLM and returning:
- assistant text (`assistant_content`)
- optional tool calls (`tool_calls[]`)

Core does not ship an embedded HTTP client. Implement `agent_tool_provider_generate_fn` using whatever stack you have:

```c
#include "agent/tool_provider.h"

static agent_status_t my_generate(
  void* provider_ctx,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  (void)provider_ctx;
  // req->messages: includes tool_call_id/tool_calls metadata (OpenAI-compatible shape)
  // req->tools: tool registry
  // req->model: model hint (you may ignore)
  //
  // 1) Serialize req into your LLM request
  // 2) Send request to your LLM
  // 3) Parse response:
  //    - assistant content string
  //    - optional tool call list (name + arguments_json + optional id)
  //
  // Fill out_resp using agent_string_set_copy and agent_malloc allocations.
  //
  // Placeholder:
  const char* txt = "Hello from embedded provider";
  (void)agent_string_set_copy(&out_resp->assistant_content, txt, strlen(txt));
  out_resp->tool_calls = NULL;
  out_resp->tool_call_count = 0;
  return AGENT_OK;
}

static agent_tool_provider_t make_provider(void) {
  agent_tool_provider_t p = {0};
  p.ctx = NULL;
  p.generate = my_generate;
  return p;
}
```

## Multimodal inputs (image + text)

The core can store multimodal message “parts” (text + image URL / binary image bytes) via `agent/parts.h`.

Important constraints:
- Parts are currently **in-memory only** (the portable session codec v1 stores role+text only).
- The **tool loop** (`agent_tool_loop_run`) passes messages to `agent_tool_provider_t` as text strings; it does not currently
  surface parts to the tool-provider interface.
- For **non-tool runs** (pure chat), `agent_run_once` can pass the session pointer into the provider via an extended request:
  implement `agent_provider_t.generate_ex` (see `agent/provider.h`). The core prefers `generate_ex` when present.

Minimal example (non-tool run, with an image URL part):

```c
#include "agent/agent.h"
#include "agent/parts.h"
#include "agent/provider.h"
#include "agent/runner.h"

agent_session_t* s = NULL;
agent_session_create(&s);

agent_content_part_t parts[2] = {0};
parts[0].type = AGENT_PART_TEXT;
parts[0].text_or_null = "Describe this image.";
parts[1].type = AGENT_PART_IMAGE_URL;
parts[1].url_or_null = "https://example.com/image.png";
agent_session_add_message_parts(s, AGENT_ROLE_USER, parts, 2);

// provider.generate_ex must translate session parts into the provider's multimodal request format.
```

## Running the core tool loop

The `agent_tool_loop_run()` function orchestrates:
- transcript compaction
- provider calls
- tool execution
- tool result injection back into the transcript

Minimal invocation:

```c
#include "agent/tool_loop.h"

agent_tool_loop_result_t r = {0};
agent_tool_loop_options_t opt = {0};
opt.model = "your-model-id";
opt.max_steps = 8;
opt.max_chars = 8000;
opt.keep_last_messages = 12;
opt.max_tool_result_chars = 2000;
opt.max_tool_call_args_chars = 0; // default (0 disables)
opt.verbose_events = 0;
opt.max_capture_chars = 0;        // default (only used when events enabled)
opt.disable_tool_records = 1;     // recommended for embedded

agent_tool_loop_hooks_t hooks = {0}; // leave all NULL for minimal allocations

agent_status_t st = agent_tool_loop_run(
  &provider,
  tools,
  &executor,
  seed_session,
  user_prompt,
  &opt,
  &hooks,
  &r
);

// Use r.final_assistant_text; then free:
agent_tool_loop_result_free(&r);
```

Notes:
- For embedded, **keep hooks NULL** unless you truly need event streaming; otherwise you pay for JSON event construction.
- `disable_tool_records=1` avoids allocating duplicated tool transcript copies in the final result.
- Always zero-init `agent_tool_loop_options_t` / `agent_tool_loop_result_t` (`{0}`) so new fields have safe defaults.

## Minimizing token footprint (practical knobs)

These settings directly reduce prompt size and tool-loop amplification:

- `opt.max_tool_result_chars`: keep tool outputs compact before re-inserting into the LLM transcript
- `opt.max_chars` + `opt.keep_last_messages`: bounds the transcript size and compaction work
- `opt.max_steps` / `opt.max_tool_calls_total` / `opt.max_tool_calls_per_tool` / `opt.max_tool_call_args_chars`: hard limits to prevent runaway loops
- Tool schemas: keep JSON schema small; avoid large descriptions; prefer enums over long free-form fields

If you need richer tool outputs for debugging, return structured *but compact* JSON, e.g.:
`{"ok":true,"v":123}` not multi-kilobyte pretty-printed blobs.

## ESP32-S3 architecture recommendation

On ESP32-S3, treat the core tool loop as a **single task**:

- Inputs: prompt + current session snapshot + tool registry
- Outputs: final assistant text (+ optionally a short event stream)
- Side effects: tools run via `agent_tool_executor_t` only

Keep all persistence (flash/SD/network) and UI outside the core library. This keeps the C API portable and minimizes binary size.
