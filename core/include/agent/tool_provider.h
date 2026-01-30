#pragma once

#include "agent/agent.h"
#include "agent/chat.h"
#include "agent/tools.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_tool_call {
  agent_string_t id;             // optional (may be empty)
  agent_string_t name;           // required
  agent_string_t arguments_json; // required (opaque UTF-8 string; often JSON)
} agent_tool_call_t;

typedef struct agent_tool_provider_request {
  const char* model; // required (provider may ignore if it uses a fixed model)
  const agent_chat_message_view_t* messages;
  size_t message_count;
  const agent_tool_registry_t* tools;

  // Optional hint for step 0: force a specific tool by name.
  const char* force_tool_or_null;

  // For diagnostics / tracing only (providers may ignore).
  size_t step;
  uint64_t epoch;
} agent_tool_provider_request_t;

typedef struct agent_tool_provider_response {
  // Assistant message content. May be empty if tool_calls are present.
  agent_string_t assistant_content;

  // Optional tool calls requested by the assistant.
  agent_tool_call_t* tool_calls;
  size_t tool_call_count;

  // Optional best-effort error string (used when status != AGENT_OK).
  agent_string_t error_message;
} agent_tool_provider_response_t;

void agent_tool_provider_response_free(agent_tool_provider_response_t* r);

typedef agent_status_t (*agent_tool_provider_generate_fn)(
  void* provider_ctx,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
);

typedef struct agent_tool_provider {
  void* ctx;
  agent_tool_provider_generate_fn generate;
} agent_tool_provider_t;

#ifdef __cplusplus
}  // extern "C"
#endif
