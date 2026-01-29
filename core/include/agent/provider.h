#pragma once

#include "agent/agent.h"

#ifdef __cplusplus
extern "C" {
#endif

// A provider is the host-supplied bridge from the core session model to an LLM backend.
//
// Core principles:
// - Core does not assume HTTP/JSON/env/filesystem.
// - Provider may use any transport/protocol and should return assistant text.

typedef struct agent_generate_request {
  const char* model; // required (provider may ignore if it uses a fixed model)
  const agent_message_view_t* messages;
  size_t message_count;
} agent_generate_request_t;

typedef struct agent_generate_response {
  agent_string_t assistant_text; // UTF-8 text
} agent_generate_response_t;

typedef agent_status_t (*agent_provider_generate_fn)(
  void* provider_ctx,
  const agent_generate_request_t* req,
  agent_generate_response_t* out_resp
);

typedef struct agent_provider {
  void* ctx;
  agent_provider_generate_fn generate;
} agent_provider_t;

#ifdef __cplusplus
}  // extern "C"
#endif

