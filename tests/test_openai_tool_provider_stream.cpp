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
};

static void on_event_capture(void* vctx, const char* type, const char* data_json) {
  auto* cap = static_cast<CapturedEvents*>(vctx);
  if (!cap || !type) return;
  if (std::string(type) != "assistant_delta") return;
  if (!data_json || !data_json[0]) return;

  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(data_json);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) return;
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

  std::cout << "ok\n";
  return 0;
}
