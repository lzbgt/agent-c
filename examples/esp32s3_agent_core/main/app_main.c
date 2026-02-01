#include "agent/agent.h"
#include "agent/tool_loop.h"
#include "agent/tool_provider.h"
#include "agent/tools.h"

#include <string.h>

// This example intentionally does NOT include an HTTP/TLS provider.
// It uses a fake provider that always requests a single tool call once.

static agent_status_t fake_generate(void* ctx, const agent_tool_provider_request_t* req, agent_tool_provider_response_t* out_resp) {
  (void)ctx;
  (void)req;
  if (!out_resp) return AGENT_ERR_INVALID_ARGUMENT;

  // Ask for a tool call: gpio_write(pin=2,value=1)
  out_resp->tool_call_count = 1;
  out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(sizeof(agent_tool_call_t));
  if (!out_resp->tool_calls) return AGENT_ERR_OOM;
  memset(out_resp->tool_calls, 0, sizeof(agent_tool_call_t));

  (void)agent_string_set_copy(&out_resp->tool_calls[0].id, "call_1", strlen("call_1"));
  (void)agent_string_set_copy(&out_resp->tool_calls[0].name, "gpio_write", strlen("gpio_write"));
  (void)agent_string_set_copy(&out_resp->tool_calls[0].arguments_json, "{\"pin\":2,\"value\":1}", strlen("{\"pin\":2,\"value\":1}"));

  // Empty assistant content is allowed when tool calls exist.
  (void)agent_string_set_copy(&out_resp->assistant_content, "", 0);
  return AGENT_OK;
}

static agent_status_t fake_exec_tool(void* ctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  (void)ctx;
  if (!tool_name || !arguments_json || !out_result) return AGENT_ERR_INVALID_ARGUMENT;

  // In a real device, parse arguments_json (cJSON) and call gpio_set_level(pin,value).
  // Keep results compact.
  if (strcmp(tool_name, "gpio_write") == 0) {
    const char* ok = "{\"ok\":true}";
    return agent_string_set_copy(out_result, ok, strlen(ok));
  }
  const char* err = "{\"ok\":false,\"error\":\"unknown tool\"}";
  return agent_string_set_copy(out_result, err, strlen(err));
}

void app_main(void) {
  agent_tool_registry_t* reg = NULL;
  (void)agent_tool_registry_create(&reg);
  (void)agent_tool_registry_add(
    reg,
    "gpio_write",
    "Set a GPIO pin high/low",
    "{\"type\":\"object\",\"properties\":{\"pin\":{\"type\":\"integer\"},\"value\":{\"type\":\"integer\",\"enum\":[0,1]}},\"required\":[\"pin\",\"value\"]}"
  );

  agent_tool_provider_t provider = {0};
  provider.ctx = NULL;
  provider.generate = fake_generate;

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_exec_tool;

  agent_session_t* seed = NULL;
  (void)agent_session_create(&seed);
  (void)agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "You are a helpful embedded agent.");

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake-model";
  opt.max_steps = 2;
  opt.max_repeated_tool_calls = 4;
  opt.max_tool_calls_total = 8;
  opt.max_tool_result_chars = 512;
  opt.keep_last_messages = 8;
  opt.max_chars = 4000;
  opt.disable_tool_records = 1;

  agent_tool_loop_result_t r = {0};
  const agent_status_t st = agent_tool_loop_run(
    &provider,
    reg,
    &exec,
    seed,
    "Turn on the LED.",
    &opt,
    /*hooks=*/NULL,
    &r
  );

  // In a real app: log st and r.final_assistant_text via ESP_LOGI.
  (void)st;

  agent_tool_loop_result_free(&r);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

