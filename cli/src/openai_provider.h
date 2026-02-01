#pragma once

#include "agent/provider.h"

#include "openai_client.h"

#include <string>

// OpenAI-compatible `agent_provider_t` adapter for `tools=none` runs.
//
// This is shared between CLI and daemon via `agent_host`.
// It provides:
// - mapping of provider "context too long" errors to `AGENT_ERR_CONTEXT_TOO_LONG`
// - best-effort capture of last request/response for tracing
//
// Note: This adapter is intentionally *non-streaming* (used by `agent_run_once`).

struct OpenAIProviderCtx {
  OpenAIClientConfig cfg;

  // Optional injection point for unit tests (no network).
  // If null, defaults to `openai_chat_completions`.
  OpenAIChatResult (*chat_fn)(const OpenAIClientConfig& cfg, const struct agent_message_view* messages, size_t message_count) = nullptr;

  long last_http_status = 0;
  std::string last_body;
  std::string last_request_body;
  std::string last_error;
};

// Creates an `agent_provider_t` backed by the given ctx.
// The returned provider borrows `ctx` (caller must keep it alive).
agent_provider_t openai_make_provider(OpenAIProviderCtx* ctx);

// The provider `generate` implementation (exposed for direct tests).
agent_status_t openai_provider_generate(
  void* provider_ctx,
  const agent_generate_request_t* req,
  agent_generate_response_t* out_resp
);

// Optional extended generate implementation that consumes session multimodal parts (agent/parts.h).
agent_status_t openai_provider_generate_ex(
  void* provider_ctx,
  const agent_generate_request_ex_t* req,
  agent_generate_response_t* out_resp
);
