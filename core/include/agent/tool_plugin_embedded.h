#pragma once

#include "agent/agent.h"
#include "agent/tools.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_tool_plugin_v0_def {
  const char* name;            // required
  const char* description;     // required (may be empty)
  const char* parameters_json; // required (OpenAI-compatible JSON Schema)
  agent_tool_execute_fn execute;
  void* ctx;
} agent_tool_plugin_v0_def_t;

typedef struct agent_tool_plugin_v0 {
  const agent_tool_plugin_v0_def_t* defs;
  size_t def_count;
} agent_tool_plugin_v0_t;

typedef struct agent_tool_plugin_executor_ctx {
  const agent_tool_plugin_v0_t* plugins;
  size_t plugin_count;
} agent_tool_plugin_executor_ctx_t;

agent_status_t agent_tool_registry_add_plugin(
  agent_tool_registry_t* registry,
  const agent_tool_plugin_v0_t* plugin
);

agent_status_t agent_tool_registry_add_plugins(
  agent_tool_registry_t* registry,
  const agent_tool_plugin_v0_t* const* plugins,
  size_t plugin_count
);

agent_status_t agent_tool_plugin_executor_init(
  agent_tool_plugin_executor_ctx_t* ctx,
  const agent_tool_plugin_v0_t* plugins,
  size_t plugin_count,
  agent_tool_executor_t* out_exec
);

#ifdef __cplusplus
}  // extern "C"
#endif
