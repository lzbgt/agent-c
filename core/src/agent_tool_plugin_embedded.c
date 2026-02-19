#include "agent/tool_plugin_embedded.h"

#include <string.h>

static agent_status_t plugin_execute_dispatch(
  void* ctx,
  const char* tool_name,
  const char* arguments_json,
  agent_string_t* out_result
) {
  if (!ctx || !tool_name || !out_result) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  const agent_tool_plugin_executor_ctx_t* exec_ctx = (const agent_tool_plugin_executor_ctx_t*)ctx;
  if (!exec_ctx->plugins || exec_ctx->plugin_count == 0) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < exec_ctx->plugin_count; i++) {
    const agent_tool_plugin_v0_t* plugin = &exec_ctx->plugins[i];
    if (!plugin || !plugin->defs) {
      continue;
    }
    for (size_t j = 0; j < plugin->def_count; j++) {
      const agent_tool_plugin_v0_def_t* def = &plugin->defs[j];
      if (!def->name || !def->execute) {
        continue;
      }
      if (strcmp(def->name, tool_name) != 0) {
        continue;
      }
      return def->execute(def->ctx, tool_name, arguments_json, out_result);
    }
  }
  return AGENT_ERR_INVALID_ARGUMENT;
}

agent_status_t agent_tool_registry_add_plugin(
  agent_tool_registry_t* registry,
  const agent_tool_plugin_v0_t* plugin
) {
  if (!registry || !plugin || !plugin->defs || plugin->def_count == 0) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < plugin->def_count; i++) {
    const agent_tool_plugin_v0_def_t* def = &plugin->defs[i];
    if (!def->name || !def->name[0] || !def->parameters_json || !def->execute) {
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    const char* desc = def->description ? def->description : "";
    const agent_status_t st = agent_tool_registry_add(registry, def->name, desc, def->parameters_json);
    if (st != AGENT_OK) {
      return st;
    }
  }
  return AGENT_OK;
}

agent_status_t agent_tool_registry_add_plugins(
  agent_tool_registry_t* registry,
  const agent_tool_plugin_v0_t* const* plugins,
  size_t plugin_count
) {
  if (!registry || !plugins || plugin_count == 0) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < plugin_count; i++) {
    const agent_tool_plugin_v0_t* plugin = plugins[i];
    if (!plugin) {
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    const agent_status_t st = agent_tool_registry_add_plugin(registry, plugin);
    if (st != AGENT_OK) {
      return st;
    }
  }
  return AGENT_OK;
}

agent_status_t agent_tool_plugin_executor_init(
  agent_tool_plugin_executor_ctx_t* ctx,
  const agent_tool_plugin_v0_t* plugins,
  size_t plugin_count,
  agent_tool_executor_t* out_exec
) {
  if (!ctx || !out_exec) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  ctx->plugins = plugins;
  ctx->plugin_count = plugin_count;
  out_exec->ctx = ctx;
  out_exec->execute = plugin_execute_dispatch;
  return AGENT_OK;
}
