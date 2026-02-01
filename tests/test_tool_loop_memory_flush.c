#include "agent/tool_loop.h"
#include "agent/tools.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MemoryFlushProviderCtx {
  int calls;
  int saw_flush_prompt;
  int saw_flush_step0;
  int saw_main_prompt;
} MemoryFlushProviderCtx;

typedef struct MemoryFlushEventCtx {
  unsigned long long flush_end_count;
  unsigned long long total_requested;
  unsigned long long total_executed;
} MemoryFlushEventCtx;

static int messages_contain(const agent_tool_provider_request_t* req, const char* needle) {
  if (!req || !needle || !needle[0]) return 0;
  for (size_t i = 0; i < req->message_count; i++) {
    const agent_chat_message_view_t* m = &req->messages[i];
    const char* c = m->content ? m->content : "";
    if (strstr(c, needle) != NULL) return 1;
  }
  return 0;
}

static void on_event(void* vctx, const char* type, const char* data_json) {
  MemoryFlushEventCtx* ctx = (MemoryFlushEventCtx*)vctx;
  if (!ctx || !type) return;
  if (strcmp(type, "memory_flush") != 0) return;
  if (!data_json) data_json = "";
  if (strstr(data_json, "\"phase\":\"end\"") == NULL) return;
  ctx->flush_end_count += 1ULL;
  unsigned long long req = 0ULL;
  unsigned long long ex = 0ULL;
  const char* reqp = strstr(data_json, "\"tool_calls_requested\":");
  if (reqp) {
    reqp += strlen("\"tool_calls_requested\":");
    req = strtoull(reqp, NULL, 10);
  }
  const char* exp = strstr(data_json, "\"tool_calls_executed\":");
  if (exp) {
    exp += strlen("\"tool_calls_executed\":");
    ex = strtoull(exp, NULL, 10);
  }
  ctx->total_requested += req;
  ctx->total_executed += ex;
}

static agent_status_t memory_flush_provider_generate(
  void* vctx,
  const agent_tool_provider_request_t* req,
  agent_tool_provider_response_t* out_resp
) {
  MemoryFlushProviderCtx* ctx = (MemoryFlushProviderCtx*)vctx;
  ctx->calls++;
  agent_tool_provider_response_free(out_resp);

  const int is_flush = messages_contain(req, "INTERNAL MEMORY FLUSH");
  if (is_flush) {
    ctx->saw_flush_prompt = 1;
    // First flush call: request a memory_write tool call.
    if (req->step == 0) {
      ctx->saw_flush_step0 = 1;
      assert(agent_string_set_copy(&out_resp->assistant_content, "", 0) == AGENT_OK);
      out_resp->tool_calls = (agent_tool_call_t*)agent_malloc(sizeof(agent_tool_call_t));
      assert(out_resp->tool_calls != NULL);
      memset(out_resp->tool_calls, 0, sizeof(agent_tool_call_t));
      out_resp->tool_call_count = 1;
      assert(agent_string_set_copy(&out_resp->tool_calls[0].id, "flush_call_0", strlen("flush_call_0")) == AGENT_OK);
      assert(agent_string_set_copy(&out_resp->tool_calls[0].name, "memory_write", strlen("memory_write")) == AGENT_OK);
      assert(
        agent_string_set_copy(
          &out_resp->tool_calls[0].arguments_json,
          "{\"layer\":\"core\",\"title\":\"legacy\",\"text\":\"- feature set A is deprecated; no longer required\\n\"}",
          strlen("{\"layer\":\"core\",\"title\":\"legacy\",\"text\":\"- feature set A is deprecated; no longer required\\n\"}")
        ) == AGENT_OK
      );
      return AGENT_OK;
    }
    // Second flush call: conclude.
    assert(agent_string_set_copy(&out_resp->assistant_content, "NO_REPLY", strlen("NO_REPLY")) == AGENT_OK);
    return AGENT_OK;
  }

  // Main run (post-compaction).
  if (messages_contain(req, "hello-main")) {
    ctx->saw_main_prompt = 1;
  }
  assert(agent_string_set_copy(&out_resp->assistant_content, "DONE", strlen("DONE")) == AGENT_OK);
  return AGENT_OK;
}

typedef struct MemoryToolExecCtx {
  int calls;
  int writes;
  char last_tool_name[64];
} MemoryToolExecCtx;

static agent_status_t memory_tool_execute(void* vctx, const char* tool_name, const char* arguments_json, agent_string_t* out_result) {
  MemoryToolExecCtx* ctx = (MemoryToolExecCtx*)vctx;
  (void)arguments_json;
  if (!out_result || !tool_name) return AGENT_ERR_INVALID_ARGUMENT;
  ctx->calls += 1;
  snprintf(ctx->last_tool_name, sizeof(ctx->last_tool_name), "%s", tool_name);
  if (strcmp(tool_name, "memory_write") == 0) {
    ctx->writes += 1;
    return agent_string_set_copy(out_result, "{\"ok\":true}", strlen("{\"ok\":true}"));
  }
  return agent_string_set_copy(out_result, "{\"ok\":false}", strlen("{\"ok\":false}"));
}

static void test_memory_flush_runs_before_compaction(void) {
  agent_tool_registry_t* reg = NULL;
  assert(agent_tool_registry_create(&reg) == AGENT_OK);
  assert(agent_tool_registry_add(reg, "memory_write", "memory write", "{\"type\":\"object\"}") == AGENT_OK);

  MemoryToolExecCtx ectx = {0};
  agent_tool_executor_t exec = {0};
  exec.ctx = &ectx;
  exec.execute = memory_tool_execute;

  agent_session_t* seed = NULL;
  assert(agent_session_create(&seed) == AGENT_OK);
  assert(agent_session_add_message(seed, AGENT_ROLE_SYSTEM, "Pinned") == AGENT_OK);

  // Create a large-ish seed transcript to force compaction with a small max_chars budget.
  char buf[512];
  for (int i = 0; i < 40; i++) {
    snprintf(buf, sizeof(buf), "user msg %d: %s", i, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
    assert(agent_session_add_message(seed, AGENT_ROLE_USER, buf) == AGENT_OK);
    snprintf(buf, sizeof(buf), "assistant msg %d: %s", i, "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
    assert(agent_session_add_message(seed, AGENT_ROLE_ASSISTANT, buf) == AGENT_OK);
  }

  MemoryFlushProviderCtx pctx = {0};
  agent_tool_provider_t provider = {0};
  provider.ctx = &pctx;
  provider.generate = memory_flush_provider_generate;

  MemoryFlushEventCtx ev = {0};
  agent_tool_loop_hooks_t hooks = {0};
  hooks.on_event_ctx = &ev;
  hooks.on_event = on_event;

  agent_tool_loop_options_t opt = {0};
  opt.model = "fake";
  opt.max_chars = 600; // small budget forces compaction
  opt.keep_last_messages = 6;
  opt.insert_compaction_summary = 1;
  opt.max_tool_result_chars = 200;
  opt.max_context_too_long_retries = 0;
  opt.memory_flush_enabled = 1;
  opt.memory_flush_max_excerpt_chars = 2000;
  opt.memory_flush_max_messages = 16;
  opt.memory_flush_per_message_chars = 200;

  agent_tool_loop_result_t out = {0};
  const agent_status_t st = agent_tool_loop_run(&provider, reg, &exec, seed, "hello-main", &opt, &hooks, &out);
  assert(st == AGENT_OK);
  assert(out.final_assistant_text.data && strcmp(out.final_assistant_text.data, "DONE") == 0);
  assert(pctx.saw_flush_prompt == 1);
  assert(pctx.saw_flush_step0 == 1);
  assert(pctx.saw_main_prompt == 1);
  assert(ev.flush_end_count >= 1ULL);
  assert(ev.total_requested >= 1ULL);
  assert(ev.total_executed == ev.total_requested);
  assert((unsigned long long)ectx.calls == ev.total_executed);
  assert(ectx.writes == ectx.calls);

  agent_tool_loop_result_free(&out);
  agent_session_destroy(seed);
  agent_tool_registry_destroy(reg);
}

void test_tool_loop_memory_flush_module(void) {
  test_memory_flush_runs_before_compaction();
  printf("test_tool_loop_memory_flush_module OK\n");
}
