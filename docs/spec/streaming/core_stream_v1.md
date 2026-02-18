# Core Streaming Interface v1

Date: 2026-02-18

This document defines the **core-layer streaming interface** for OpenAI-compatible
Chat Completions streaming (`stream: true`). It is designed to be **transport-agnostic**
and **JSON-parser-agnostic**, so the core can remain small and portable while hosts
implement provider-specific parsing.

Status: **implemented** in `core/include/agent/stream_decoder.h` and
`core/src/agent_stream_decoder.c`.

## Goals

- Provide a core-owned streaming state machine that is independent of HTTP/SSE transport details.
- Support incremental assistant deltas and tool-call deltas with deterministic assembly.
- Keep memory bounded and deterministic (explicit limits + fail-fast errors).
- Avoid mandatory JSON libraries inside `agent_core` (host supplies decode function).
- Unify CLI and daemon behavior to reduce drift and ensure consistent event semantics.

## Deferred for v1 (not non-goals)

- Provider-specific streaming formats that are not OpenAI-compatible.
- Audio/video streaming (Opus/WebRTC); tracked under the media roadmap.
- Automatic token-accurate budgets inside the core (host-provided usage is supported).

## Design overview

### Layering

1) **Transport layer (host)**
   - Provides bytes from HTTP/SSE stream to the core.
   - Responsible for HTTP retries, auth, and provider endpoints.

2) **SSE framing (core)**
   - Uses `agent_sse_parser_t` to convert raw bytes into `event/id/data`.

3) **Chunk decode (host)**
   - Host provides a callback to decode `data` JSON into a minimal `agent_stream_chunk_t`.
   - This avoids JSON deps in core and allows provider-specific rules.

4) **Delta assembly (core)**
   - Core merges tool-call deltas into complete tool calls.
   - Core emits **normalized stream events** to the host via callback.

### Stream chunk (host-provided)

The host decodes each SSE `data` JSON payload into:

```
typedef struct agent_stream_tool_call_delta {
  size_t index;
  agent_string_t id;
  agent_string_t name;
  agent_string_t arguments_delta;
} agent_stream_tool_call_delta_t;

typedef struct agent_stream_chunk {
  uint8_t has_delta_text;
  agent_string_t delta_text;

  agent_stream_tool_call_delta_t* tool_calls;
  size_t tool_calls_count;

  uint8_t has_finish_reason;
  agent_string_t finish_reason;

  uint8_t has_usage;
  uint64_t prompt_tokens;
  uint64_t completion_tokens;
  uint64_t total_tokens;

  uint8_t has_error;
  agent_string_t error_message;
} agent_stream_chunk_t;
```

Notes:
- `agent_string_t` is a core type.
- Tool-call deltas mirror OpenAI `delta.tool_calls` (index + partial args).
- The decoder takes ownership of chunk allocations and calls `agent_stream_chunk_free`.

### Stream events (core-emitted)

Core emits events via callback as `type` + `data_json` payload strings:

- `assistant_delta` (text)
- `tool_call_delta` (optional; when a partial tool-call fragment arrives)
- `tool_call` (when a tool call is fully assembled)
- `llm_usage` (when usage is present in stream)
- `error`
- `end`

`assistant_delta` payloads include `{ delta, step, epoch }` to align with tool-loop event semantics.

## C API (implemented)

```
typedef struct agent_stream_decoder agent_stream_decoder_t;

typedef agent_status_t (*agent_stream_decode_fn)(
  void* ctx,
  const char* data,
  size_t len,
  agent_stream_chunk_t* out_chunk
);

typedef void (*agent_stream_event_fn)(
  void* ctx,
  const char* type,
  const char* data_json
);

typedef struct agent_stream_config {
  size_t max_tool_calls_total;
  size_t max_tool_call_args_chars;
  size_t max_events_per_feed;
  uint64_t step;
  uint64_t epoch;
} agent_stream_config_t;

void agent_stream_decoder_init(
  agent_stream_decoder_t* dec,
  const agent_stream_config_t* cfg,
  agent_stream_decode_fn decode_fn,
  void* decode_ctx,
  agent_stream_event_fn event_fn,
  void* event_ctx
);

agent_status_t agent_stream_decoder_feed(
  agent_stream_decoder_t* dec,
  const char* bytes,
  size_t len
);

void agent_stream_decoder_reset(agent_stream_decoder_t* dec);
void agent_stream_decoder_free(agent_stream_decoder_t* dec);
```

The decoder:
- feeds bytes into `agent_sse_parser_t`
- calls `decode_fn` for each SSE `data` payload
- assembles tool calls and emits normalized events

## Limits and error behavior

- If tool-call args exceed `max_tool_call_args_chars`, return `AGENT_ERR_LIMIT`.
- If tool-call count exceeds `max_tool_calls_total`, return `AGENT_ERR_LIMIT`.
- If `decode_fn` reports an error, decoding stops and returns that error.

## Testing strategy

- Unit test: `tests/test_stream_decoder.c` (synthetic SSE inputs; validates event payloads).
- Fixture tests with recorded SSE chunks for:
  - assistant deltas
  - tool-call delta assembly
  - usage chunks
  - error handling
- Golden JSONL event outputs validated against `run_event_v1` schema.

## Integration plan

1) Extract the existing OpenAI stream decoder into a host adapter that fills
   `agent_stream_chunk_t`.
2) Done: core decoder + unit tests using synthetic SSE inputs.
3) Wire CLI/daemon streaming to use the core decoder, preserving current output.
