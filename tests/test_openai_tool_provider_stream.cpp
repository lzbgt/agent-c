#include "openai_tool_provider.h"

#include "agent/tool_provider.h"
#include "agent/tools.h"

#include <json/json.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void expect_true(bool v, const char* what) {
  if (!v) {
    std::cerr << "FAILED: expected true: " << (what ? what : "") << "\n";
    std::abort();
  }
}

static void expect_eq(size_t a, size_t b, const char* what) {
  if (a != b) {
    std::cerr << "FAILED: expected equal: " << (what ? what : "") << " got=" << a << " want=" << b << "\n";
    std::abort();
  }
}

static void expect_streq(const std::string& a, const std::string& b, const char* what) {
  if (a != b) {
    std::cerr << "FAILED: expected string equal: " << (what ? what : "") << " got=" << a << " want=" << b << "\n";
    std::abort();
  }
}

struct CapturedEvents {
  int deltas = 0;
  std::string delta_concat;
  int retries = 0;
  std::string retry_reason;
};

static void on_event_capture(void* vctx, const char* type, const char* data_json) {
  auto* cap = static_cast<CapturedEvents*>(vctx);
  if (!cap || !type) return;
  if (!data_json || !data_json[0]) return;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(data_json);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) return;
  const std::string event_type(type);
  if (event_type == "retry") {
    cap->retries++;
    if (v.isMember("reason") && v["reason"].isString()) cap->retry_reason = v["reason"].asString();
    return;
  }
  if (event_type != "assistant_delta") return;
  const auto& d = v["delta"];
  if (!d.isString()) return;
  cap->deltas++;
  cap->delta_concat += d.asString();
}

static OpenAIStreamResult fake_stream_tool_call(
  const OpenAIClientConfig& /*cfg*/,
  const std::string& /*request_body_json*/,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t /*max_capture_bytes*/
) {
  // Fragmented tool-call arguments: {"path":"README.md", ...}
  const char* c1 =
    R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"fs_read","arguments":"{\"path\":\"README.md\","}}]}}]})";
  const char* c2 =
    R"({"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"max_lines\":5,\"max_chars\":20000}"}}]}}]})";

  if (on_chunk) {
    on_chunk(on_chunk_ctx, c1, std::strlen(c1));
    on_chunk(on_chunk_ctx, c2, std::strlen(c2));
  }

  OpenAIStreamResult r;
  r.http_status = 200;
  r.response_body = "stub_stream";
  r.saw_done = true;
  return r;
}

static OpenAIStreamResult fake_stream_assistant_text(
  const OpenAIClientConfig& /*cfg*/,
  const std::string& /*request_body_json*/,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t /*max_capture_bytes*/
) {
  const char* c1 = R"({"choices":[{"delta":{"content":"Hello "}}]})";
  const char* c2 = R"({"choices":[{"delta":{"content":"world"}}]})";
  if (on_chunk) {
    on_chunk(on_chunk_ctx, c1, std::strlen(c1));
    on_chunk(on_chunk_ctx, c2, std::strlen(c2));
  }
  OpenAIStreamResult r;
  r.http_status = 200;
  r.response_body = "stub_stream";
  r.saw_done = true;
  return r;
}

static OpenAIStreamResult fake_stream_assistant_text_requires_cap(
  const OpenAIClientConfig& /*cfg*/,
  const std::string& request_body_json,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t /*max_capture_bytes*/
) {
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(request_body_json);
  Json::Value req;
  expect_true(Json::parseFromStream(rb, iss, &req, &errs), "parse capped stream request");
  expect_true(req.isObject(), "capped stream request is object");
  expect_true(req.isMember("max_completion_tokens"), "capped stream request has max_completion_tokens");
  expect_eq((size_t)req["max_completion_tokens"].asUInt64(), 7u, "max_completion_tokens forwarded");
  return fake_stream_assistant_text(OpenAIClientConfig{}, request_body_json, on_chunk, on_chunk_ctx, 0);
}

static int g_stream_options_fallback_calls = 0;

static OpenAIStreamResult fake_stream_rejects_stream_options_once(
  const OpenAIClientConfig& /*cfg*/,
  const std::string& request_body_json,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t /*max_capture_bytes*/
) {
  g_stream_options_fallback_calls++;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(request_body_json);
  Json::Value req;
  expect_true(Json::parseFromStream(rb, iss, &req, &errs), "parse stream_options fallback request");
  expect_true(req.isObject(), "stream_options fallback request is object");

  if (req.isMember("stream_options")) {
    OpenAIStreamResult r;
    r.http_status = 400;
    r.response_body = R"({"error":{"message":"unknown field: stream_options.include_usage"}})";
    r.error_message = "unknown field: stream_options.include_usage";
    r.saw_done = false;
    return r;
  }

  return fake_stream_assistant_text(OpenAIClientConfig{}, request_body_json, on_chunk, on_chunk_ctx, 0);
}

static OpenAIStreamResult fake_stream_legacy_function_call(
  const OpenAIClientConfig& /*cfg*/,
  const std::string& /*request_body_json*/,
  OpenAIStreamChunkCallback on_chunk,
  void* on_chunk_ctx,
  size_t /*max_capture_bytes*/
) {
  // Legacy streaming shape: delta.function_call.{name,arguments}, with fragmented arguments.
  const char* c1 =
    R"({"choices":[{"delta":{"function_call":{"name":"fs_read","arguments":"{\"path\":\"README.md\","}}}]})";
  const char* c2 =
    R"({"choices":[{"delta":{"function_call":{"arguments":"\"max_lines\":5,\"max_chars\":20000}"}}}]})";

  if (on_chunk) {
    on_chunk(on_chunk_ctx, c1, std::strlen(c1));
    on_chunk(on_chunk_ctx, c2, std::strlen(c2));
  }

  OpenAIStreamResult r;
  r.http_status = 200;
  r.response_body = "stub_stream";
  r.saw_done = true;
  return r;
}

static int g_deepseek_reasoning_raw_calls = 0;

static OpenAIRawResult fake_raw_deepseek_v4_pro_reasoning_tool_call(
  const OpenAIClientConfig& /*cfg*/,
  const std::string& request_body_json
) {
  g_deepseek_reasoning_raw_calls++;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(request_body_json);
  Json::Value req;
  expect_true(Json::parseFromStream(rb, iss, &req, &errs), "parse DeepSeek request");
  expect_true(req.isObject(), "DeepSeek request is object");
  expect_streq(req["model"].asString(), "deepseek-v4-pro", "DeepSeek model forwarded");
  expect_true(!req.isMember("tool_choice"), "DeepSeek request omits forced tool_choice");

  OpenAIRawResult r;
  r.http_status = 200;
  if (g_deepseek_reasoning_raw_calls == 1) {
    r.response_body =
      R"({"choices":[{"message":{"role":"assistant","content":"","reasoning_content":"tool reasoning","tool_calls":[{"id":"call_reason","type":"function","function":{"name":"fs_read","arguments":"{}"}}]}}]})";
    return r;
  }

  expect_true(req.isMember("messages") && req["messages"].isArray(), "DeepSeek follow-up has messages");
  bool found_assistant = false;
  for (const auto& msg : req["messages"]) {
    if (!msg.isObject() || !msg.isMember("role") || !msg["role"].isString()) continue;
    if (msg["role"].asString() != "assistant") continue;
    if (!msg.isMember("tool_calls") || !msg["tool_calls"].isArray() || msg["tool_calls"].empty()) continue;
    found_assistant = true;
    expect_true(msg.isMember("reasoning_content"), "DeepSeek assistant tool turn carries reasoning_content");
    expect_streq(msg["reasoning_content"].asString(), "tool reasoning", "DeepSeek reasoning_content preserved");
  }
  expect_true(found_assistant, "DeepSeek follow-up assistant tool turn found");

  r.response_body = R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
  return r;
}

static agent_tool_registry_t* make_minimal_registry() {
  agent_tool_registry_t* r = nullptr;
  const agent_status_t st = agent_tool_registry_create(&r);
  expect_true(st == AGENT_OK && r != nullptr, "agent_tool_registry_create");

  // Minimal JSON schema object for parameters.
  const char* params = R"({"type":"object","properties":{},"required":[]})";
  expect_true(agent_tool_registry_add(r, "fs_read", "read file", params) == AGENT_OK, "add tool");
  return r;
}

static agent_tool_provider_request_t make_req(const agent_tool_registry_t* tools) {
  static const char* content = "hi";
  static agent_chat_message_view_t msg{};
  msg.role = AGENT_ROLE_USER;
  msg.content = content;
  msg.content_len = 2;
  msg.name = nullptr;
  msg.tool_call_id = nullptr;
  msg.tool_calls = nullptr;
  msg.tool_call_count = 0;

  agent_tool_provider_request_t req{};
  req.model = "stub";
  req.messages = &msg;
  req.message_count = 1;
  req.tools = tools;
  req.force_tool_or_null = nullptr;
  req.step = 0;
  req.epoch = 0;
  return req;
}

int main() {
  {
    // Streaming tool_call reconstruction (delta.tool_calls) without assistant text.
    agent_tool_registry_t* tools = make_minimal_registry();

    OpenAIToolProviderCtx ctx;
    ctx.cfg.base_url = "http://stub";
    ctx.cfg.api_key = "dummy";
    ctx.cfg.model = "stub";
    ctx.stream_assistant = true;
    ctx.chat_stream_fn = fake_stream_tool_call;

    agent_tool_provider_t p = openai_make_tool_provider(&ctx);

    agent_tool_provider_request_t req = make_req(tools);
    agent_tool_provider_response_t resp{};
    const agent_status_t st = p.generate(p.ctx, &req, &resp);
    expect_true(st == AGENT_OK, "stream tool_call status");
    expect_streq(std::string(resp.assistant_content.data ? resp.assistant_content.data : ""), "", "assistant content empty");
    expect_eq(resp.tool_call_count, 1u, "tool_call_count");
    expect_true(resp.tool_calls != nullptr, "tool_calls allocated");
    expect_streq(std::string(resp.tool_calls[0].name.data, resp.tool_calls[0].name.len), "fs_read", "tool name");
    expect_streq(
      std::string(resp.tool_calls[0].arguments_json.data, resp.tool_calls[0].arguments_json.len),
      R"({"path":"README.md","max_lines":5,"max_chars":20000})",
      "tool arguments reconstructed"
    );

    agent_tool_provider_response_free(&resp);
    agent_tool_registry_destroy(tools);
  }

  {
    // Streaming legacy function_call reconstruction (delta.function_call) without assistant text.
    agent_tool_registry_t* tools = make_minimal_registry();

    OpenAIToolProviderCtx ctx;
    ctx.cfg.base_url = "http://stub";
    ctx.cfg.api_key = "dummy";
    ctx.cfg.model = "stub";
    ctx.stream_assistant = true;
    ctx.chat_stream_fn = fake_stream_legacy_function_call;

    agent_tool_provider_t p = openai_make_tool_provider(&ctx);

    agent_tool_provider_request_t req = make_req(tools);
    agent_tool_provider_response_t resp{};
    const agent_status_t st = p.generate(p.ctx, &req, &resp);
    expect_true(st == AGENT_OK, "stream legacy function_call status");
    expect_streq(std::string(resp.assistant_content.data ? resp.assistant_content.data : ""), "", "assistant content empty");
    expect_eq(resp.tool_call_count, 1u, "tool_call_count");
    expect_true(resp.tool_calls != nullptr, "tool_calls allocated");
    expect_streq(std::string(resp.tool_calls[0].name.data, resp.tool_calls[0].name.len), "fs_read", "tool name");
    expect_streq(
      std::string(resp.tool_calls[0].arguments_json.data, resp.tool_calls[0].arguments_json.len),
      R"({"path":"README.md","max_lines":5,"max_chars":20000})",
      "tool arguments reconstructed"
    );

    agent_tool_provider_response_free(&resp);
    agent_tool_registry_destroy(tools);
  }

  {
    // Streaming assistant deltas produce assistant_delta events and a final assistant_content string.
    agent_tool_registry_t* tools = make_minimal_registry();

    CapturedEvents cap;
    OpenAIToolProviderCtx ctx;
    ctx.cfg.base_url = "http://stub";
    ctx.cfg.api_key = "dummy";
    ctx.cfg.model = "stub";
    ctx.stream_assistant = true;
    ctx.chat_stream_fn = fake_stream_assistant_text;
    ctx.on_event = on_event_capture;
    ctx.on_event_ctx = &cap;

    agent_tool_provider_t p = openai_make_tool_provider(&ctx);
    agent_tool_provider_request_t req = make_req(tools);
    agent_tool_provider_response_t resp{};
    const agent_status_t st = p.generate(p.ctx, &req, &resp);
    expect_true(st == AGENT_OK, "stream assistant status");
    expect_eq(resp.tool_call_count, 0u, "no tool calls");
    expect_true(resp.assistant_content.data != nullptr, "assistant_content allocated");
    expect_streq(std::string(resp.assistant_content.data, resp.assistant_content.len), "Hello world", "assistant content");
    expect_true(cap.deltas >= 1, "at least one assistant_delta event emitted");
    expect_streq(cap.delta_concat, "Hello world", "assistant_delta concatenation matches");

    agent_tool_provider_response_free(&resp);
    agent_tool_registry_destroy(tools);
  }

  {
    // Configured provider caps are forwarded into streaming tool-provider requests.
    agent_tool_registry_t* tools = make_minimal_registry();

    OpenAIToolProviderCtx ctx;
    ctx.cfg.base_url = "http://stub";
    ctx.cfg.api_key = "dummy";
    ctx.cfg.model = "stub";
    ctx.cfg.max_completion_tokens = 7;
    ctx.stream_assistant = true;
    ctx.chat_stream_fn = fake_stream_assistant_text_requires_cap;

    agent_tool_provider_t p = openai_make_tool_provider(&ctx);
    agent_tool_provider_request_t req = make_req(tools);
    agent_tool_provider_response_t resp{};
    const agent_status_t st = p.generate(p.ctx, &req, &resp);
    expect_true(st == AGENT_OK, "stream assistant capped status");
    agent_tool_provider_response_free(&resp);
    agent_tool_registry_destroy(tools);
  }

  {
    // stream_options compatibility fallback is visible to run/event consumers.
    agent_tool_registry_t* tools = make_minimal_registry();

    CapturedEvents cap;
    OpenAIToolProviderCtx ctx;
    ctx.cfg.base_url = "http://stub";
    ctx.cfg.api_key = "dummy";
    ctx.cfg.model = "stub";
    ctx.stream_assistant = true;
    ctx.chat_stream_fn = fake_stream_rejects_stream_options_once;
    ctx.on_event = on_event_capture;
    ctx.on_event_ctx = &cap;
    g_stream_options_fallback_calls = 0;

    agent_tool_provider_t p = openai_make_tool_provider(&ctx);
    agent_tool_provider_request_t req = make_req(tools);
    agent_tool_provider_response_t resp{};
    const agent_status_t st = p.generate(p.ctx, &req, &resp);
    expect_true(st == AGENT_OK, "stream assistant stream_options fallback status");
    expect_eq((size_t)g_stream_options_fallback_calls, 2u, "stream_options fallback called twice");
    expect_true(cap.retries >= 1, "stream_options retry event emitted");
    expect_streq(cap.retry_reason, "stream_options_rejected", "stream_options retry reason");
    agent_tool_provider_response_free(&resp);
    agent_tool_registry_destroy(tools);
  }

  {
    // DeepSeek V4 Pro can return reasoning_content in tool-calling turns and requires it on follow-up requests.
    agent_tool_registry_t* tools = make_minimal_registry();

    OpenAIToolProviderCtx ctx;
    ctx.cfg.base_url = "https://api.deepseek.com";
    ctx.cfg.api_key = "dummy";
    ctx.cfg.model = "deepseek-v4-pro";
    ctx.stream_assistant = false;
    ctx.chat_raw_fn = fake_raw_deepseek_v4_pro_reasoning_tool_call;
    g_deepseek_reasoning_raw_calls = 0;

    agent_tool_provider_t p = openai_make_tool_provider(&ctx);
    agent_tool_provider_request_t first = make_req(tools);
    first.model = "deepseek-v4-pro";
    first.force_tool_or_null = "fs_read";
    agent_tool_provider_response_t resp1{};
    const agent_status_t st1 = p.generate(p.ctx, &first, &resp1);
    expect_true(st1 == AGENT_OK, "DeepSeek first tool-call status");
    expect_eq(resp1.tool_call_count, 1u, "DeepSeek first tool-call count");
    expect_streq(std::string(resp1.tool_calls[0].id.data, resp1.tool_calls[0].id.len), "call_reason", "DeepSeek tool id");

    const char* assistant_content = "";
    agent_chat_tool_call_view_t assistant_call{};
    assistant_call.id = "call_reason";
    assistant_call.name = "fs_read";
    assistant_call.arguments_json = "{}";

    agent_chat_message_view_t follow_messages[3]{};
    follow_messages[0].role = AGENT_ROLE_USER;
    follow_messages[0].content = "hi";
    follow_messages[0].content_len = 2;
    follow_messages[1].role = AGENT_ROLE_ASSISTANT;
    follow_messages[1].content = assistant_content;
    follow_messages[1].content_len = 0;
    follow_messages[1].tool_calls = &assistant_call;
    follow_messages[1].tool_call_count = 1;
    follow_messages[2].role = AGENT_ROLE_TOOL;
    follow_messages[2].content = "file";
    follow_messages[2].content_len = 4;
    follow_messages[2].tool_call_id = "call_reason";

    agent_tool_provider_request_t second{};
    second.model = "deepseek-v4-pro";
    second.messages = follow_messages;
    second.message_count = 3;
    second.tools = tools;
    second.step = 1;
    second.epoch = 0;

    agent_tool_provider_response_t resp2{};
    const agent_status_t st2 = p.generate(p.ctx, &second, &resp2);
    expect_true(st2 == AGENT_OK, "DeepSeek follow-up status");
    expect_streq(std::string(resp2.assistant_content.data, resp2.assistant_content.len), "ok", "DeepSeek follow-up assistant text");
    expect_eq((size_t)g_deepseek_reasoning_raw_calls, 2u, "DeepSeek raw call count");

    agent_tool_provider_response_free(&resp2);
    agent_tool_provider_response_free(&resp1);
    agent_tool_registry_destroy(tools);
  }

  std::cout << "ok\n";
  return 0;
}
