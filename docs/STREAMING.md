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

Shared decoder (host library, used by both CLI + daemon):

- `cli/src/openai_stream_decoder.h`
  - `OpenAIStreamChunk`: decoded fields from a single streamed JSON chunk
  - `openai_stream_parse_chunk_json(...)`: parse `data: { ... }` JSON payload
  - `OpenAIToolCallStreamAccumulator`: reconstruct complete tool calls from `delta.tool_calls` fragments

## Event schema notes

Host streaming emits `assistant_delta` events (daemon SSE and tool-loop events array) using a consistent payload shape:

- `delta` (string): incremental assistant text
- `step` (number): tool-loop step (`0` for `tools="none"`)
- `epoch` (number): retry/rotation epoch (daemon uses attempt index for `tools="none"`)

The CLI streaming mode prints deltas to stdout (not an event stream), but uses the same decoded content deltas.
