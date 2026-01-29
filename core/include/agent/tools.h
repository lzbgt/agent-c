#pragma once

#include "agent/agent.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tool definition is intentionally transport/protocol agnostic.
// The "parameters_json" field is expected to be an OpenAI-compatible JSON Schema object string
// (for example: {"type":"object","properties":{...},"required":[...]}).
// The core stores this blob but does not parse JSON.
typedef struct agent_tool_def_view {
  const char* name;
  const char* description;
  const char* parameters_json;
} agent_tool_def_view_t;

typedef struct agent_tool_registry agent_tool_registry_t;

agent_status_t agent_tool_registry_create(agent_tool_registry_t** out_registry);
void agent_tool_registry_destroy(agent_tool_registry_t* registry);

agent_status_t agent_tool_registry_add(
  agent_tool_registry_t* registry,
  const char* name,
  const char* description,
  const char* parameters_json
);

size_t agent_tool_registry_count(const agent_tool_registry_t* registry);
agent_status_t agent_tool_registry_get(const agent_tool_registry_t* registry, size_t index, agent_tool_def_view_t* out_view);

// Tool execution is always host-provided; core just defines a stable callback shape.
typedef agent_status_t (*agent_tool_execute_fn)(
  void* ctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
);

typedef struct agent_tool_executor {
  void* ctx;
  agent_tool_execute_fn execute;
} agent_tool_executor_t;

#ifdef __cplusplus
}  // extern "C"
#endif

