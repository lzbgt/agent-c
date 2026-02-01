#pragma once

#include "agent/tool_provider.h"
#include "agent/tool_loop.h"

#include "openai_client.h"

#include <string>
#include <unordered_map>

// OpenAI-compatible tool provider adapter (host-side, JSONCPP-based).
//
// This converts the portable `agent_chat_message_view_t` transcript into an OpenAI Chat Completions JSON request
// including `tools` and (when needed) `tool_choice`, then parses the response back into structured tool calls.

struct OpenAIToolProviderCtx {
  OpenAIClientConfig cfg;

  // Optional trace/event hook for llm_request/llm_response.
  agent_tool_loop_event_fn on_event = nullptr;
  void* on_event_ctx = nullptr;

  // Optional injection points for deterministic unit tests (no network).
  // When null, defaults to the real OpenAI-compatible HTTP client.
  OpenAIRawResult (*chat_raw_fn)(const OpenAIClientConfig& cfg, const std::string& request_body_json) = nullptr;
  OpenAIStreamResult (*chat_stream_fn)(
    const OpenAIClientConfig& cfg,
    const std::string& request_body_json,
    OpenAIStreamChunkCallback on_chunk,
    void* on_chunk_ctx,
    size_t max_capture_bytes
  ) = nullptr;

  // When true, uses OpenAI-compatible SSE streaming (`stream: true`) and emits `assistant_delta` events.
  // Tool calls are reconstructed incrementally from streamed `delta.tool_calls` when present (best-effort).
  bool stream_assistant = false;

  bool verbose_events = false;
  // Caps the captured streaming wire body (best-effort) returned as `last_response_body`.
  // This only applies to streaming calls (openai_chat_completions_raw_stream).
  size_t max_capture_bytes = 256 * 1024;
  // Caps large fields included in emitted events (best-effort), e.g. request_json/response_body.
  size_t max_event_chars = 256 * 1024;

  // Last HTTP response (best-effort) for callers that want to surface provider errors.
  long last_http_status = 0;
  std::string last_response_body;
  std::string last_request_json;

  // Best-effort carry-forward for providers that require reasoning content to be present
  // during multi-step tool calling (keyed by tool call ids).
  std::unordered_map<std::string, std::string> reasoning_by_tool_call_ids;
};

// Creates an `agent_tool_provider_t` backed by the given ctx.
// The returned provider borrows `ctx` (caller must keep it alive).
agent_tool_provider_t openai_make_tool_provider(OpenAIToolProviderCtx* ctx);
