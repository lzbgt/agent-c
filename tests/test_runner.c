#include "agent/runner.h"

#include <assert.h>
#include <string.h>

typedef struct dummy_provider_ctx {
  int calls;
} dummy_provider_ctx_t;

static agent_status_t dummy_generate(void* provider_ctx, const agent_generate_request_t* req, agent_generate_response_t* out_resp) {
  (void)req;
  if (!provider_ctx || !out_resp) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  dummy_provider_ctx_t* ctx = (dummy_provider_ctx_t*)provider_ctx;
  ctx->calls += 1;
  const char* s = "OK";
  return agent_string_set_copy(&out_resp->assistant_text, s, strlen(s));
}

static void test_run_once_appends_assistant(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned") == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_USER, "Hi") == AGENT_OK);

  dummy_provider_ctx_t ctx = {0};
  agent_provider_t p;
  memset(&p, 0, sizeof(p));
  p.ctx = &ctx;
  p.generate = dummy_generate;
  p.generate_ex = NULL;

  agent_run_options_t opt;
  opt.model = "dummy-model";
  opt.max_chars = 1024;
  opt.keep_last_messages = 8;
  opt.summary_or_null = NULL;

  agent_run_report_t rep;
  memset(&rep, 0, sizeof(rep));
  assert(agent_run_once(s, &p, &opt, &rep) == AGENT_OK);
  assert(ctx.calls == 1);
  assert(rep.provider_called == 1);

  const size_t n = agent_session_message_count(s);
  assert(n == 3);
  agent_message_view_t v;
  assert(agent_session_get_message(s, n - 1, &v) == AGENT_OK);
  assert(v.role == AGENT_ROLE_ASSISTANT);
  assert(v.content_len == 2);
  assert(strncmp(v.content, "OK", 2) == 0);

  agent_session_destroy(s);
}

void test_runner_module(void) {
  test_run_once_appends_assistant();
}
