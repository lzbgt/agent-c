#include "agent/agent.h"
#include "agent/tool_plugin_embedded.h"

#include <string.h>
#include <assert.h>

static agent_status_t exec_alpha(void* ctx, const char* tool_name, const char* arguments_json, agent_string_t* out) {
  (void)ctx;
  (void)tool_name;
  (void)arguments_json;
  const char* payload = "{\"ok\":true,\"tool\":\"alpha\"}";
  return agent_string_set_copy(out, payload, strlen(payload));
}

static agent_status_t exec_beta(void* ctx, const char* tool_name, const char* arguments_json, agent_string_t* out) {
  (void)ctx;
  (void)tool_name;
  (void)arguments_json;
  const char* payload = "{\"ok\":true,\"tool\":\"beta\"}";
  return agent_string_set_copy(out, payload, strlen(payload));
}

void test_tool_plugin_embedded_module(void) {
  const agent_tool_plugin_v0_def_t defs[] = {
    {
      .name = "alpha",
      .description = "alpha tool",
      .parameters_json = "{}",
      .execute = exec_alpha,
      .ctx = NULL,
    },
    {
      .name = "beta",
      .description = "beta tool",
      .parameters_json = "{}",
      .execute = exec_beta,
      .ctx = NULL,
    },
  };
  const agent_tool_plugin_v0_t plugin = {
    .defs = defs,
    .def_count = sizeof(defs) / sizeof(defs[0]),
  };

  agent_tool_registry_t* registry = NULL;
  assert(agent_tool_registry_create(&registry) == AGENT_OK);
  assert(registry != NULL);
  assert(agent_tool_registry_add_plugin(registry, &plugin) == AGENT_OK);
  assert(agent_tool_registry_count(registry) == 2);

  agent_tool_plugin_executor_ctx_t exec_ctx = {0};
  agent_tool_executor_t exec = {0};
  assert(agent_tool_plugin_executor_init(&exec_ctx, &plugin, 1, &exec) == AGENT_OK);

  agent_string_t out = {0};
  assert(exec.execute(exec.ctx, "beta", "{}", &out) == AGENT_OK);
  const char* expected = "{\"ok\":true,\"tool\":\"beta\"}";
  assert(out.data != NULL);
  assert(out.len == strlen(expected));
  assert(memcmp(out.data, expected, out.len) == 0);
  agent_string_free(&out);

  agent_tool_registry_destroy(registry);
}
