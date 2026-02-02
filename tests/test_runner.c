#include "agent/runner.h"
#include "agent/parts.h"

#include <assert.h>
#include <string.h>

typedef struct dummy_provider_ctx {
  int calls;
} dummy_provider_ctx_t;

typedef struct dummy_provider_ex_ctx {
  int calls_generate;
  int calls_generate_ex;
  int saw_any_parts;
} dummy_provider_ex_ctx_t;

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

static agent_status_t dummy_generate_ex(
  void* provider_ctx,
  const agent_generate_request_ex_t* req,
  agent_generate_response_t* out_resp
) {
  if (!provider_ctx || !req || !out_resp || !req->session) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  dummy_provider_ex_ctx_t* ctx = (dummy_provider_ex_ctx_t*)provider_ctx;
  ctx->calls_generate_ex += 1;

  // Verify the provider can access multimodal parts via the session pointer.
  // The core runner preserves the original session indexing for message parts.
  for (size_t i = 0; i < req->message_count; i++) {
    const size_t pc = agent_session_message_part_count(req->session, i);
    if (pc == 0) continue;
    ctx->saw_any_parts = 1;

    // Expect at least one text part.
    agent_content_part_view_t part0;
    memset(&part0, 0, sizeof(part0));
    assert(agent_session_get_message_part(req->session, i, 0, &part0) == AGENT_OK);
    assert(part0.type == AGENT_PART_TEXT);
    assert(part0.text_len > 0);

    // If there is a second part, it should be an image URL (for this test).
    if (pc >= 2) {
      agent_content_part_view_t part1;
      memset(&part1, 0, sizeof(part1));
      assert(agent_session_get_message_part(req->session, i, 1, &part1) == AGENT_OK);
      assert(part1.type == AGENT_PART_IMAGE_URL);
      assert(part1.url_len > 0);
    }
  }

  const char* s = "OK_EX";
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

static void test_run_once_generate_ex_sees_parts(void) {
  agent_session_t* s = NULL;
  assert(agent_session_create(&s) == AGENT_OK);
  assert(agent_session_add_message(s, AGENT_ROLE_SYSTEM, "Pinned") == AGENT_OK);

  agent_content_part_t parts[2];
  memset(parts, 0, sizeof(parts));
  parts[0].type = AGENT_PART_TEXT;
  parts[0].text_or_null = "What is in this image?";
  parts[1].type = AGENT_PART_IMAGE_URL;
  parts[1].url_or_null = "https://example.invalid/test.png";
  assert(agent_session_add_message_parts(s, AGENT_ROLE_USER, parts, 2) == AGENT_OK);

  dummy_provider_ex_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));

  agent_provider_t p;
  memset(&p, 0, sizeof(p));
  p.ctx = &ctx;
  p.generate = NULL;
  p.generate_ex = dummy_generate_ex;

  agent_run_options_t opt;
  opt.model = "dummy-model";
  opt.max_chars = 1024;
  opt.keep_last_messages = 8;
  opt.summary_or_null = NULL;

  agent_run_report_t rep;
  memset(&rep, 0, sizeof(rep));
  assert(agent_run_once(s, &p, &opt, &rep) == AGENT_OK);
  assert(ctx.calls_generate_ex == 1);
  assert(ctx.saw_any_parts == 1);
  assert(rep.provider_called == 1);

  const size_t n = agent_session_message_count(s);
  assert(n == 3);
  agent_message_view_t v;
  assert(agent_session_get_message(s, n - 1, &v) == AGENT_OK);
  assert(v.role == AGENT_ROLE_ASSISTANT);
  assert(v.content_len == 5);
  assert(strncmp(v.content, "OK_EX", 5) == 0);

  agent_session_destroy(s);
}

void test_runner_module(void) {
  test_run_once_appends_assistant();
  test_run_once_generate_ex_sees_parts();
}
