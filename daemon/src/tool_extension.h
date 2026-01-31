#pragma once

#include "agent/agent.h"
#include "agent/tools.h"

namespace agentd {

// Optional host-provided tool extension point for embedded/sidecar deployments.
//
// Intended usage:
// - agentd creates a baseline tool registry/executor (basic|host)
// - host registers additional tools into the same registry
// - agentd dispatches those additional tools to `execute_tool`
//
// Contract:
// - `register_tools` should only append new tools to the registry.
// - tools registered by the extension must be handled by `execute_tool`.
struct ToolExtension {
  void* ctx = nullptr;
  agent_status_t (*register_tools)(void* ctx, agent_tool_registry_t* registry) = nullptr;
  agent_status_t (*execute_tool)(
    void* ctx,
    const char* tool_name,
    const char* arguments_json,
    agent_string_t* out_result
  ) = nullptr;
};

}  // namespace agentd

