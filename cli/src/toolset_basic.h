#pragma once

#include "agent/tools.h"

// Creates a tool registry and an executor implementing a minimal "basic" toolset.
// Currently included:
// - calculator (arithmetic expression)
//
// The caller owns the returned registry and must destroy it via `agent_tool_registry_destroy`.
// The returned executor is a small POD with callbacks; it does not require destruction.
agent_status_t toolset_basic_create(agent_tool_registry_t** out_registry, agent_tool_executor_t* out_executor);
