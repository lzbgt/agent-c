#pragma once

#include "openai_client.h"
#include "agent/tools.h"

#include <cstddef>
#include <string>
#include <iosfwd>

struct ToolLoopOptions {
  std::string force_tool;    // optional tool name to force on first request
  bool require_tool_call = false;
  size_t max_steps = 8;
};

struct ToolLoopResult {
  std::string final_assistant_text;
  bool saw_tool_call = false;
};

// Runs an OpenAI-compatible tool-call loop using the host's HTTP client (libcurl).
// On success, returns true and fills out_result.
// On failure, returns false and fills out_error (best effort).
bool run_tool_loop(
  const OpenAIClientConfig& cfg,
  const struct agent_session* seed_session,
  const std::string& user_prompt,
  const struct agent_tool_registry* tools,
  const struct agent_tool_executor* executor,
  const ToolLoopOptions& options,
  std::ostream* trace_stream,
  ToolLoopResult* out_result,
  std::string* out_error,
  long* out_http_status,
  std::string* out_http_body
);
