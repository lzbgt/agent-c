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

## Non-goals (current)

- Full coverage for every streaming variant across providers (there are many).
- Streaming for the **core** layer (core remains JSON/HTTP-free).
- Token-accurate streaming budgets (streaming is transport-layer; budgeting remains char-based in host/core).

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
