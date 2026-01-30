#include "agent/tool_loop.h"
#include "agent/tools.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct FakeProviderCtx {
  int call_count;
  int too_long_once;
  int saw_tool_result;
} FakeProviderCtx;

static agent_status_t no_tools_generate(
  void* /*vctx*/,
  const agent_tool_provider_request_t* /*req*/,
  agent_tool_provider_response_t* out_resp
) {
  agent_tool_provider_response_free(out_resp);
  const char* done = "DONE";
  assert(agent_string_set_copy(&out_resp->assistant_content, done, strlen(done)) == AGENT_OK);
  return AGENT_OK;
}

static agent_status_t fake_provider_generate(
  void* vctx,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  FakeProviderCtx* ctx = (FakeProviderCtx*)vctx;
  ctx->call_count++;

  agent_tool_provider_response_free(out_resp);

  if (ctx->too_long_once) {
    ctx->too_long_once = 0;
    const char* msg = "context too long (synthetic)";
    (void)agent_string_set_copy(&out_resp->error_message, msg, strlen(msg));
    return AGENT_ERR_CONTEXT_TOO_LONG;
  }

  // Step 0: request one tool call.
  if (req->step == 0) {
    const char* content = "";
    assert(agent_string_set_copy(&out_resp->assistant_content, content, strlen(content)) == AGENT_OK);
    out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(sizeof(agent_tool_call_t));
    assert(out_resp->tool_calls != NULL);
    memset(out_resp->tool_calls, 0, sizeof(agent_tool_call_t));
    out_resp->tool_call_count = 1;
    assert(agent_string_set_copy(&out_resp->tool_calls[0].id, "call_0_0", strlen("call_0_0")) == AGENT_OK);
    assert(agent_string_set_copy(&out_resp->tool_calls[0].name, "echo", strlen("echo")) == AGENT_OK);
    assert(agent_string_set_copy(&out_resp->tool_calls[0].arguments_json, "{}", strlen("{}")) == AGENT_OK);
    return AGENT_OK;
  }

  // Step 1: ensure we received a tool result message and then finish.
  ctx->saw_tool_result = 0;
  for (size_t i = 0; i < req->message_count; i++) {
    const agent_chat_message_view_t* m = &req->messages[i];
    if (m->role != AGENT_ROLE_TOOL) continue;
    if (m->tool_call_id && strcmp(m->tool_call_id, "call_0_0") == 0) {
      ctx->saw_tool_result = 1;
      break;
    }
  }

  const char* done = "DONE";
  assert(agent_string_set_copy(&out_resp->assistant_content, done, strlen(done)) == AGENT_OK);
  return AGENT_OK;
}

static agent_status_t fake_tool_execute(void* /*ctx*/, const char* tool_name, const char* /*arguments_json*/, agent_string_t* out_result) {
  if (!out_result) return AGENT_ERR_INVALID_ARGUMENT;
  if (!tool_name) return AGENT_ERR_INVALID_ARGUMENT;
  if (strcmp(tool_name, "echo") != 0) return AGENT_ERR_INVALID_ARGUMENT;
  const char* ok = "OK";
  return agent_string_set_copy(out_result, ok, strlen(ok));
}

static void event_counter_on_event(void* vctx, const char* type, const char* /*data_json*/) {
  int* retry_count = (int*)vctx;
  if (type && strcmp(type, "retry") == 0) {
    (*retry_count)++;
  }
}

static uint8_t cancel_immediately(void* /*ctx*/) {
  return 1;
}

static agent_status_t repeat_tool_call_provider_generate(
  void* /*vctx*/,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  agent_tool_provider_response_free(out_resp);
  const char* content = "";
  assert(agent_string_set_copy(&out_resp->assistant_content, content, strlen(content)) == AGENT_OK);
  out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(sizeof(agent_tool_call_t));
  assert(out_resp->tool_calls != NULL);
  memset(out_resp->tool_calls, 0, sizeof(agent_tool_call_t));
  out_resp->tool_call_count = 1;

  char idbuf[64];
  snprintf(idbuf, sizeof(idbuf), "call_%llu_0", (unsigned long long)req->step);
  assert(agent_string_set_copy(&out_resp->tool_calls[0].id, idbuf, strlen(idbuf)) == AGENT_OK);
  assert(agent_string_set_copy(&out_resp->tool_calls[0].name, "echo", strlen("echo")) == AGENT_OK);
  assert(agent_string_set_copy(&out_resp->tool_calls[0].arguments_json, "{}", strlen("{}")) == AGENT_OK);
  return AGENT_OK;
}

static agent_status_t always_tool_call_provider_generate(
  void* /*vctx*/,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  agent_tool_provider_response_free(out_resp);
  const char* content = "";
  assert(agent_string_set_copy(&out_resp->assistant_content, content, strlen(content)) == AGENT_OK);
  out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(sizeof(agent_tool_call_t));
  assert(out_resp->tool_calls != NULL);
  memset(out_resp->tool_calls, 0, sizeof(agent_tool_call_t));
  out_resp->tool_call_count = 1;

  char idbuf[64];
  snprintf(idbuf, sizeof(idbuf), "call_%llu_0", (unsigned long long)req->step);
  assert(agent_string_set_copy(&out_resp->tool_calls[0].id, idbuf, strlen(idbuf)) == AGENT_OK);
  assert(agent_string_set_copy(&out_resp->tool_calls[0].name, "echo", strlen("echo")) == AGENT_OK);
  assert(agent_string_set_copy(&out_resp->tool_calls[0].arguments_json, "{\"x\":1}", strlen("{\"x\":1}")) == AGENT_OK);
  return AGENT_OK;
}

static agent_status_t many_tool_calls_in_one_step_provider_generate(
  void* /*vctx*/,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  agent_tool_provider_response_free(out_resp);
  const char* content = "";
  assert(agent_string_set_copy(&out_resp->assistant_content, content, strlen(content)) == AGENT_OK);

  // Only step 0 requests many tools; later steps stop.
  if (req->step != 0) {
    const char* done = "DONE";
    agent_tool_provider_response_free(out_resp);
    assert(agent_string_set_copy(&out_resp->assistant_content, done, strlen(done)) == AGENT_OK);
    return AGENT_OK;
  }

  const int n = 10;
  out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(sizeof(agent_tool_call_t) * (size_t)n);
  assert(out_resp->tool_calls != NULL);
  memset(out_resp->tool_calls, 0, sizeof(agent_tool_call_t) * (size_t)n);
  out_resp->tool_call_count = (size_t)n;

  for (int i = 0; i < n; i++) {
    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "call_0_%d", i);
    assert(agent_string_set_copy(&out_resp->tool_calls[i].id, idbuf, strlen(idbuf)) == AGENT_OK);
    assert(agent_string_set_copy(&out_resp->tool_calls[i].name, "echo", strlen("echo")) == AGENT_OK);
    char argbuf[64];
    snprintf(argbuf, sizeof(argbuf), "{\"i\":%d}", i);
    assert(agent_string_set_copy(&out_resp->tool_calls[i].arguments_json, argbuf, strlen(argbuf)) == AGENT_OK);
  }
  return AGENT_OK;
}

static void test_tool_loop_happy_path(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  FakeProviderCtx pctx = {0};
  agent_tool_provider_t provider = {0};
  provider.ctx = &pctx;
  provider.generate = fake_provider_generate;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;
  opt.insert_compaction_summary = 1;
  opt.max_tool_result_chars = 200;
  opt.max_context_too_long_retries = 2;
  opt.verbose_events = 0;
  opt.max_capture_chars = 1000;

  agent_tool_loop_hooks_t hooks = {0};
  agent_tool_loop_result_t out = {0};
  assert(agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, &hooks, &out) == AGENT_OK);
  assert(out.final_assistant_text.data && strcmp(out.final_assistant_text.data, "DONE") == 0);
  assert(out.saw_tool_call == 1);
  assert(pctx.saw_tool_result == 1);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_repeated_tool_call_guard(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  agent_tool_provider_t provider = {0};
  provider.ctx = NULL;
  provider.generate = repeat_tool_call_provider_generate;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;
  opt.max_tool_result_chars = 200;
  opt.max_repeated_tool_calls = 3;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, NULL, &out);
  assert(st != AGENT_OK);
  assert(st == AGENT_ERR_LIMIT);
  assert(out.error_message.data && strstr(out.error_message.data, "repeated tool call") != NULL);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_max_steps_guard(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  agent_tool_provider_t provider = {0};
  provider.ctx = NULL;
  provider.generate = always_tool_call_provider_generate;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;
  opt.max_tool_result_chars = 200;
  opt.max_repeated_tool_calls = 0; // disable repetition guard so we hit max_steps deterministically
  opt.max_steps = 2;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, NULL, &out);
  assert(st == AGENT_ERR_LIMIT);
  assert(out.error_message.data && strstr(out.error_message.data, "max steps") != NULL);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_max_tool_calls_total_guard(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  agent_tool_provider_t provider = {0};
  provider.ctx = NULL;
  provider.generate = many_tool_calls_in_one_step_provider_generate;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;
  opt.max_tool_result_chars = 200;
  opt.max_steps = 0;
  opt.max_repeated_tool_calls = 0;
  opt.max_tool_calls_total = 3;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, NULL, &out);
  assert(st == AGENT_ERR_LIMIT);
  assert(out.error_message.data && strstr(out.error_message.data, "max tool calls") != NULL);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_max_tool_calls_per_tool_guard(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  agent_tool_provider_t provider = {0};
  provider.ctx = NULL;
  provider.generate = always_tool_call_provider_generate;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;
  opt.max_tool_result_chars = 200;
  opt.max_steps = 0;
  opt.max_repeated_tool_calls = 0;
  opt.max_tool_calls_per_tool = 2;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, NULL, &out);
  assert(st == AGENT_ERR_LIMIT);
  assert(out.error_message.data && strstr(out.error_message.data, "per tool") != NULL);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_context_too_long_retries(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);
  // Add lots of history to encourage compaction work.
  for (int i = 0; i < 80; i++) {
    assert(agent_session_add_message(seed, AGENT_ROLE_USER, "hello hello hello hello hello") == AGENT_OK);
    assert(agent_session_add_message(seed, AGENT_ROLE_ASSISTANT, "world world world world world") == AGENT_OK);
  }

  FakeProviderCtx pctx = {0};
  pctx.too_long_once = 1;
  agent_tool_provider_t provider = {0};
  provider.ctx = &pctx;
  provider.generate = fake_provider_generate;

  int retry_count = 0;
  agent_tool_loop_hooks_t hooks = {0};
  hooks.on_event = event_counter_on_event;
  hooks.on_event_ctx = &retry_count;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 400; // intentionally tiny so compaction runs
  opt.keep_last_messages = 8;
  opt.insert_compaction_summary = 1;
  opt.max_tool_result_chars = 50;
  opt.max_context_too_long_retries = 2;

  agent_tool_loop_result_t out = {0};
  assert(agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, &hooks, &out) == AGENT_OK);
  assert(retry_count >= 1);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_require_tool_call_errors(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  // Provider that never produces tool calls.
  FakeProviderCtx pctx = {0};
  agent_tool_provider_t provider = {0};
  provider.ctx = &pctx;
  provider.generate = no_tools_generate;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.require_tool_call = 1;
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, NULL, &out);
  assert(st != AGENT_OK);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

static void test_tool_loop_cancel(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "echo", "echo tool", "{\"type\":\"object\"}") == AGENT_OK);

  agent_tool_executor_t exec = {0};
  exec.ctx = NULL;
  exec.execute = fake_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "sys") == AGENT_OK);

  FakeProviderCtx pctx = {0};
  agent_tool_provider_t provider = {0};
  provider.ctx = &pctx;
  provider.generate = fake_provider_generate;

  agent_tool_loop_hooks_t hooks = {0};
  hooks.should_cancel = cancel_immediately;
  hooks.should_cancel_ctx = NULL;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 20000;
  opt.keep_last_messages = 16;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello", &opt, &hooks, &out);
  assert(st == AGENT_ERR_CANCELLED);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

void test_tool_loop_module(void) {
  test_tool_loop_happy_path();
  test_tool_loop_repeated_tool_call_guard();
  test_tool_loop_max_steps_guard();
  test_tool_loop_max_tool_calls_total_guard();
  test_tool_loop_max_tool_calls_per_tool_guard();
  test_tool_loop_context_too_long_retries();
  test_tool_loop_require_tool_call_errors();
  test_tool_loop_cancel();
  printf("test_tool_loop_module OK\n");
}
