#include "openai_provider.h"
#include "openai_message_normalization.h"

#include "agent/agent.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sstream>

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


static void test_leading_system_wire_context() {
  const std::string content[] = {"HOST", "CLIENT", "PROJECT", "CURRENT_DATE=2026-11-13", "user question"};
  agent_message_view_t messages[5]{};
  for (size_t i = 0; i < 5; ++i) {
    messages[i].role = i < 4 ? AGENT_ROLE_SYSTEM : AGENT_ROLE_USER;
    messages[i].content = content[i].c_str();
    messages[i].content_len = content[i].size();
  }
  OpenAIProviderCtx ctx;
  ctx.cfg.model = "fixture";
  ctx.chat_fn = fake_capture_model_ok;
  agent_generate_request_t request{};
  request.messages = messages;
  request.message_count = 5;
  agent_generate_response_t response{};
  expect_eq(openai_provider_generate(&ctx, &request, &response), AGENT_OK, "plain context request status");
  agent_string_free(&response.assistant_text);
  Json::CharReaderBuilder reader;
  Json::Value captured;
  std::string error;
  std::istringstream input(ctx.last_request_body);
  expect_true(Json::parseFromStream(reader, input, &captured, &error), "parse plain request capture");
  expect_eq(captured["messages"].size(), 2, "plain wire system count");
  expect_true(captured["messages"][0]["content"] == "HOST\n\nCLIENT\n\nPROJECT\n\nCURRENT_DATE=2026-11-13",
      "plain wire retains every system block in order");
  expect_true(captured["messages"][1]["content"] == "user question", "plain user content preserved");
  expect_true(messages[3].role == AGENT_ROLE_SYSTEM && request.message_count == 5,
      "original plain transcript unchanged");

  // A multimodal system content array keeps its complete parts. Named system
  // envelopes and every message after a non-system boundary remain untouched.
  Json::Value sequence(Json::arrayValue), message(Json::objectValue);
  message["role"] = "system"; message["content"] = "first";
  sequence.append(message);
  message["content"] = Json::Value(Json::arrayValue);
  Json::Value part(Json::objectValue);
  part["type"] = "text"; part["text"] = "second";
  message["content"].append(part);
  part = Json::Value(Json::objectValue);
  part["type"] = "image_url"; part["image_url"]["url"] = "https://example.org/system-image.png";
  message["content"].append(part);
  sequence.append(message);
  message["name"] = "compaction_summary"; message["content"] = "summary";
  sequence.append(message);
  message.removeMember("name"); message["role"] = "user"; message["content"] = "question";
  sequence.append(message);
  message["role"] = "system"; message["content"] = "later";
  sequence.append(message);
  const auto original = sequence;
  openai_coalesce_leading_system_messages(sequence);
  expect_eq(sequence.size(), 4, "structured prefix coalesced");
  expect_true(sequence[0]["content"].isArray() && sequence[0]["content"].size() == 4,
      "system content part boundaries preserved");
  expect_true(sequence[0]["content"][3] == original[1]["content"][1], "system image part retained exactly");
  for (Json::ArrayIndex i = 1; i < sequence.size(); ++i)
    expect_true(sequence[i] == original[i + 1], "named system and conversation suffix unchanged");
  const auto once = sequence;
  openai_coalesce_leading_system_messages(sequence);
  expect_true(sequence == once, "normalization is idempotent");
}

int main() {
  test_leading_system_wire_context();
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
