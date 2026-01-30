#pragma once

#include "agent/tool_provider.h"
#include "agent/tool_loop.h"

#include "openai_client.h"

#include <string>

// OpenAI-compatible tool provider adapter (host-side, JSONCPP-based).
//
// This converts the portable `agent_chat_message_view_t` transcript into an OpenAI Chat Completions JSON request
// including `tools` and (when needed) `tool_choice`, then parses the response back into structured tool calls.

struct OpenAIToolProviderCtx {
  OpenAIClientConfig cfg;

  // Optional trace/event hook for llm_request/llm_response.
  agent_tool_loop_event_fn on_event = nullptr;
  void* on_event_ctx = nullptr;

  bool verbose_events = false;
  size_t max_capture_chars = 256 * 1024;

  // Last HTTP response (best-effort) for callers that want to surface provider errors.
  long last_http_status = 0;
  std::string last_response_body;
  std::string last_request_json;
};

// Creates an `agent_tool_provider_t` backed by the given ctx.
// The returned provider borrows `ctx` (caller must keep it alive).
agent_tool_provider_t openai_make_tool_provider(OpenAIToolProviderCtx* ctx);
