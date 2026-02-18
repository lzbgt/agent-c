# Streaming (OpenAI-compatible) – Design Notes

Date: 2026-01-30

This document describes how this repo handles **OpenAI-compatible Chat Completions streaming** (`stream: true`, SSE),
and the shared implementation used by:

- CLI `agent` (`--stream-assistant`)
- daemon `agentd` (`stream_assistant: true`)
- the host tool provider (`cli/src/openai_tool_provider.cpp`) for tool-loop steps

## Goals

- **Single source of truth** for parsing SSE `data: { ... }` JSON chunks into:
  - assistant text deltas (`choices[0].delta.content`)
  - tool-call deltas (`choices[0].delta.tool_calls`)
  - `finish_reason` (best-effort)
- **Best-effort compatibility** across OpenAI-ish providers:
  - missing `index` in `delta.tool_calls`
  - tool-call arguments fragmented across many chunks
  - legacy `delta.function_call` streaming shape (treated as a single tool call at index 0)
  - providers that ignore `stream: true` and return a normal JSON completion body
- **Minimal coupling**: keep streaming decode logic independent from daemon job/event plumbing.

## Additional goals (current)

- Full coverage for every streaming variant across providers, backed by a compatibility test matrix.
- Streaming support for the **core** layer via a transport-agnostic interface.
- Token-accurate streaming budgets for *all* providers with reconciliation fallbacks.

## Usage accounting (best-effort)

For OpenAI-compatible Chat Completions streaming, this repo attempts to preserve token accounting so workflow-level
`max_total_tokens` budgets remain enforceable even when `stream_assistant:true` is used.

- When streaming is enabled, the tool provider requests `stream_options.include_usage=true`.
- If a provider rejects `stream_options` (commonly as an HTTP 400 "unknown/unrecognized field"), the client retries once
  without `stream_options` to preserve streaming functionality (but token usage will not be available).
- When a streamed chunk includes a top-level `usage` object, the tool provider emits a `llm_usage` event, allowing the daemon
  to aggregate tokens across tool-loop steps and charge durable workflow budgets.

## Interfaces

Core SSE framing (portable, C API):

- `core/include/agent/sse_parser.h`
  - `agent_sse_parser_feed(...)`: incremental SSE framing; emits complete events
  - `agent_sse_event_t`: `{event,id,data}` fields (data lines joined with `\n`)

Shared decoder (host library, used by both CLI + daemon):

- `cli/src/openai_stream_decoder.h`
  - `OpenAIStreamChunk`: decoded fields from a single streamed JSON chunk
  - `openai_stream_parse_chunk_json(...)`: parse `data: { ... }` JSON payload
  - `OpenAIToolCallStreamAccumulator`: reconstruct complete tool calls from `delta.tool_calls` fragments
- `cli/src/openai_stream_adapter.h`
  - `OpenAIStreamCoreAdapter`: bridges OpenAI stream JSON chunks into the core stream decoder
  - CLI/daemon/tools now share the same core delta assembly + usage handling

Core streaming interface (design draft):

- `docs/spec/streaming/core_stream_v1.md`
- Core reference implementation (agent-core):
  - `core/include/agent/stream_decoder.h`
  - `core/src/agent_stream_decoder.c`

## Event schema notes

Host streaming emits `assistant_delta` events (daemon SSE and tool-loop events array) using a consistent payload shape:

- `delta` (string): incremental assistant text
- `step` (number): tool-loop step (`0` for `tools="none"`)
- `epoch` (number): retry/rotation epoch (daemon uses attempt index for `tools="none"`)

The CLI streaming mode prints deltas to stdout (not an event stream), but uses the same decoded content deltas.

## Compatibility matrix (variants)

Decoder variants and how they are covered in tests:

| Variant | Coverage | Evidence |
| --- | --- | --- |
| `delta.content` assistant text | Covered | `tests/test_openai_stream_decoder.cpp`, `tests/agent_local_stream_assistant_smoke.sh` |
| `delta.tool_calls` fragmented args | Covered | `tests/test_openai_stream_decoder.cpp`, `tests/agent_local_stream_tool_loop_smoke.sh` |
| Legacy `delta.function_call` | Covered | `tests/test_openai_stream_decoder.cpp`, `tests/test_openai_tool_provider_stream.cpp` |
| `stream_options.include_usage` usage chunks | Covered | `tests/agentd_workflow_budget_tokens_stream_smoke.sh` |
| Provider retry during stream (429) | Covered | `tests/agentd_stream_provider_retry_smoke.sh` |

## Compatibility matrix (providers)

Provider-level coverage is tracked by runnable smoke tests. These are **evidence of coverage**, not a blanket guarantee:

| Provider | Streaming tested | Tool-call delta | Usage in stream | Evidence |
| --- | --- | --- | --- | --- |
| Local OpenAI-compatible stub | Yes | Yes | Yes | `tests/agent_local_stream_assistant_smoke.sh`, `tests/agent_local_stream_tool_loop_smoke.sh`, `tests/agentd_workflow_budget_tokens_stream_smoke.sh` |
| DeepSeek (live) | Yes (assistant text) | Yes (tool calls) | Unknown | `tests/agentd_stream_assistant_smoke.sh`, `tests/agentd_deepseek_stream_tool_call_smoke.sh` (requires `DEEPSEEK_API_KEY`) |
| OpenRouter (live) | Yes (assistant text) | Yes (tool calls) | Unknown | `tests/agentd_openrouter_stream_assistant_smoke.sh`, `tests/agentd_openrouter_stream_tool_call_smoke.sh` (requires `OPENROUTER_API_KEY`) |
| Moonshot (live) | Yes (assistant text) | Yes (tool calls) | Unknown | `tests/agentd_moonshot_stream_assistant_smoke.sh`, `tests/agentd_moonshot_stream_tool_call_smoke.sh` (requires `KIMI_API_KEY_CN` / `MOONSHOT_API_KEY`) |

## Provider model pinning (OpenRouter)

OpenRouter models can change behavior or deprecate streaming. To identify **known-good** models for streaming
assistant deltas and tool-call deltas, use the probe script:

```bash
tools/probe_openrouter_stream_models.sh build/agentd
```

If you need a quick key sanity check before probing, run:

```bash
tools/check_openrouter_auth.sh
```

The script produces a JSON summary under `build/openrouter_probe/` and prints a list of models that pass
both assistant and tool-call streaming checks. Pin those in:

- `AGENT_TEST_OPENROUTER_STREAM_MODEL`
- `AGENT_TEST_OPENROUTER_STREAM_TOOL_MODEL`

To force a specific candidate list (bypass `/openrouter/models` discovery), set:

- `OPENROUTER_STREAM_PROBE_MODELS=modelA,modelB,...`

For a repo-tracked pin file, rerun the probe with `OPENROUTER_STREAM_PROBE_WRITE_PINS=1` to write:

- `ref/openrouter/streaming_pins.json`

The OpenRouter smoke tests will prefer this pin file when present, falling back to the env vars and defaults above.
If every candidate fails with OpenRouter auth errors, the probe exits with code 77 and prints a skip message.
