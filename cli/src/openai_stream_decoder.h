#pragma once

#include <string>
#include <vector>

// Shared helpers for decoding OpenAI-compatible streaming (SSE) JSON chunks from Chat Completions.
//
// The SSE framing (`data: ...`) is handled by `SseParser`. These helpers operate on the JSON payload for each `data:` event.

struct OpenAIStreamToolCallDelta {
  int index = -1;               // tool call index (best-effort; -1 if unknown)
  std::string id;               // optional
  std::string name;             // optional
  std::string arguments_delta;  // arguments fragment (may be empty)
};

struct OpenAIStreamChunk {
  std::string content_delta;  // choices[0].delta.content
  std::vector<OpenAIStreamToolCallDelta> tool_call_deltas;  // choices[0].delta.tool_calls
  std::string finish_reason;  // choices[0].finish_reason (optional)
};

// Parses a single OpenAI-compatible streamed JSON chunk (from one SSE `data:` event).
// Returns false if the payload isn't valid JSON or doesn't match the expected object shape.
bool openai_stream_parse_chunk_json(const char* chunk_json, size_t chunk_len, OpenAIStreamChunk* out_chunk);

struct OpenAIStreamToolCall {
  std::string id;
  std::string name;
  std::string arguments;  // reconstructed full JSON string (best-effort); may be partial if provider ends early
};

// Incrementally reconstructs tool calls from streamed `delta.tool_calls` fragments.
class OpenAIToolCallStreamAccumulator {
 public:
  void reset();
  void apply(const std::vector<OpenAIStreamToolCallDelta>& deltas);
  const std::vector<OpenAIStreamToolCall>& calls() const { return calls_; }

 private:
  std::vector<OpenAIStreamToolCall> calls_;
};

