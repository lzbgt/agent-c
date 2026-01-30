#include "openai_stream_decoder.h"

#include <cstring>
#include <cstdio>

static void expect_true(bool cond, const char* msg) {
  if (cond) return;
  std::fprintf(stderr, "EXPECT_TRUE failed: %s\n", msg ? msg : "(null)");
  std::abort();
}

int main() {
  {
    const char* chunk = R"({"choices":[{"delta":{"content":"Hello "}}]})";
    OpenAIStreamChunk c;
    expect_true(openai_stream_parse_chunk_json(chunk, std::strlen(chunk), &c), "parse content chunk");
    expect_true(c.content_delta == "Hello ", "content_delta");
    expect_true(c.tool_call_deltas.empty(), "no tool_call_deltas");
  }

  {
    // Tool calls with fragmented arguments; second chunk omits index so decoder uses array index 0.
    const char* c1 = R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"fs_read","arguments":"{\"path\":\"README.md\","}}]}}]})";
    const char* c2 = R"({"choices":[{"delta":{"tool_calls":[{"function":{"arguments":"\"max_lines\":5}"}}]}}]})";
    OpenAIStreamChunk a1, a2;
    expect_true(openai_stream_parse_chunk_json(c1, std::strlen(c1), &a1), "parse tool_call chunk 1");
    expect_true(openai_stream_parse_chunk_json(c2, std::strlen(c2), &a2), "parse tool_call chunk 2");

    OpenAIToolCallStreamAccumulator acc;
    acc.apply(a1.tool_call_deltas);
    acc.apply(a2.tool_call_deltas);
    expect_true(acc.calls().size() == 1, "one reconstructed tool call");
    expect_true(acc.calls()[0].id == "call_1", "tool call id");
    expect_true(acc.calls()[0].name == "fs_read", "tool call name");
    expect_true(acc.calls()[0].arguments.find("\"path\"") != std::string::npos, "arguments contains path");
    expect_true(acc.calls()[0].arguments.find("\"max_lines\"") != std::string::npos, "arguments contains max_lines");
  }

  {
    // Legacy function_call streaming shape (no tool_calls array).
    const char* c1 = R"({"choices":[{"delta":{"function_call":{"name":"calculator","arguments":"{\"expression\":\"1+1\"}"}}}]})";
    OpenAIStreamChunk c;
    expect_true(openai_stream_parse_chunk_json(c1, std::strlen(c1), &c), "parse legacy function_call chunk");
    expect_true(c.tool_call_deltas.size() == 1, "one legacy tool_call_delta");
    OpenAIToolCallStreamAccumulator acc;
    acc.apply(c.tool_call_deltas);
    expect_true(acc.calls().size() == 1, "one reconstructed legacy tool call");
    expect_true(acc.calls()[0].name == "calculator", "legacy tool call name");
    expect_true(acc.calls()[0].arguments.find("expression") != std::string::npos, "legacy arguments");
  }

  return 0;
}
