#pragma once

#include "agent/agent.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal chat transcript view used by tool providers.
//
// Notes:
// - This is NOT the portable session storage model (agent_session_t). It is an ephemeral transcript
//   view used when building provider requests that need tool-call metadata (tool_calls / tool_call_id).
// - Tool argument strings are treated as opaque UTF-8 (often JSON).

typedef struct agent_chat_tool_call_view {
  const char* id;              // optional (may be NULL)
  const char* name;            // required when tool_call_count>0
  const char* arguments_json;  // required when tool_call_count>0
} agent_chat_tool_call_view_t;

typedef struct agent_chat_message_view {
  agent_role_t role;
  const char* content;    // UTF-8 text (may be empty but not NULL)
  size_t content_len;     // bytes, excluding terminator

  // Optional message metadata used by OpenAI-compatible tool calling:
  const char* name;          // used for system summary markers (optional)
  const char* tool_call_id;  // for role=tool messages (optional)

  // Optional tool calls attached to an assistant message.
  const agent_chat_tool_call_view_t* tool_calls;
  size_t tool_call_count;
} agent_chat_message_view_t;

#ifdef __cplusplus
}  // extern "C"
#endif

