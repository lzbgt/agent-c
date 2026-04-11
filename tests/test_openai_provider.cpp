#include "openai_provider.h"

#include "agent/agent.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

static void expect_true(bool v, const char* what) {
  if (!v) {
    std::cerr << "FAILED: expected true: " << (what ? what : "") << "\n";
    std::abort();
  }
}

static void expect_eq(long a, long b, const char* what) {
  if (a != b) {
    std::cerr << "FAILED: expected equal: " << (what ? what : "") << " got=" << a << " want=" << b << "\n";
    std::abort();
  }
}

static std::string g_last_model;

static OpenAIChatResult fake_capture_model_ok(
  const OpenAIClientConfig& cfg,
  const struct agent_message_view* /*messages*/,
  size_t /*message_count*/
) {
  g_last_model = cfg.model;
  OpenAIChatResult r;
  r.http_status = 200;
  r.response_body = R"({"ok":true})";
  r.assistant_text = "hello";
  return r;
}

static OpenAIChatResult fake_context_too_long(
  const OpenAIClientConfig& /*cfg*/,
  const struct agent_message_view* /*messages*/,
  size_t /*message_count*/
) {
  OpenAIChatResult r;
  r.http_status = 413;
  r.response_body = R"({"error":{"message":"maximum context length exceeded"}})";
  r.assistant_text = "";
  r.error_message = "";
  return r;
}

static OpenAIChatResult fake_non_2xx_other(
  const OpenAIClientConfig& /*cfg*/,
  const struct agent_message_view* /*messages*/,
  size_t /*message_count*/
) {
  OpenAIChatResult r;
  r.http_status = 401;
  r.response_body = R"({"error":{"message":"invalid api key"}})";
  r.assistant_text = "";
  r.error_message = "";
  return r;
}

int main() {
  agent_message_view_t msgs[1]{};
  msgs[0].role = AGENT_ROLE_USER;
  msgs[0].content = "hi";
  msgs[0].content_len = 2;

  {
    OpenAIProviderCtx ctx;
    ctx.cfg.model = "base";
    ctx.cfg.max_completion_tokens = 7;
    ctx.chat_fn = fake_capture_model_ok;

    agent_generate_request_t req{};
    req.model = "override";
    req.messages = msgs;
    req.message_count = 1;
    agent_generate_response_t resp{};
    const agent_status_t st = openai_provider_generate(&ctx, &req, &resp);
    expect_eq((long)st, (long)AGENT_OK, "success status");
    expect_true(resp.assistant_text.data != nullptr, "assistant_text allocated");
    expect_true(std::string(resp.assistant_text.data, resp.assistant_text.len) == "hello", "assistant_text matches");
    expect_true(g_last_model == "override", "model override respected");
    expect_true(ctx.last_http_status == 200, "last_http_status captured");
    expect_true(!ctx.last_request_body.empty(), "request JSON captured");
    expect_true(ctx.last_request_body.find("override") != std::string::npos, "request JSON includes model");
    expect_true(ctx.last_request_body.find("\"max_completion_tokens\":7") != std::string::npos, "request JSON includes max_completion_tokens");
    agent_string_free(&resp.assistant_text);
  }

  {
    OpenAIProviderCtx ctx;
    ctx.cfg.model = "base";
    ctx.chat_fn = fake_context_too_long;

    agent_generate_request_t req{};
    req.model = "base";
    req.messages = msgs;
    req.message_count = 1;
    agent_generate_response_t resp{};
    const agent_status_t st = openai_provider_generate(&ctx, &req, &resp);
    expect_eq((long)st, (long)AGENT_ERR_CONTEXT_TOO_LONG, "context too long mapped");
    expect_eq(ctx.last_http_status, 413, "http status captured");
    expect_true(!ctx.last_error.empty(), "error string populated");
    agent_string_free(&resp.assistant_text);
  }

  {
    OpenAIProviderCtx ctx;
    ctx.cfg.model = "base";
    ctx.chat_fn = fake_non_2xx_other;

    agent_generate_request_t req{};
    req.model = "base";
    req.messages = msgs;
    req.message_count = 1;
    agent_generate_response_t resp{};
    const agent_status_t st = openai_provider_generate(&ctx, &req, &resp);
    expect_eq((long)st, (long)AGENT_ERR_INTERNAL, "non-context provider errors map to internal");
    expect_eq(ctx.last_http_status, 401, "http status captured");
    expect_true(!ctx.last_error.empty(), "error string populated");
    agent_string_free(&resp.assistant_text);
  }

  std::cout << "ok\n";
  return 0;
}
