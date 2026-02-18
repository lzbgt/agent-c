#pragma once

#include "agent/stream_decoder.h"

#include <stddef.h>
#include <stdint.h>

typedef void (*OpenAIStreamDeltaFn)(
  void* ctx,
  const char* delta,
  size_t delta_len,
  uint64_t step,
  uint64_t epoch
);

typedef struct OpenAIStreamCoreConfig {
  size_t max_tool_calls_total;
  size_t max_tool_call_args_chars;
  size_t max_events_per_feed;
  size_t delta_flush_bytes;
  uint64_t step;
  uint64_t epoch;
} OpenAIStreamCoreConfig;

typedef struct OpenAIStreamCoreAdapter {
  agent_stream_decoder_t dec;
  uint64_t step;
  uint64_t epoch;
  size_t delta_flush_bytes;
  OpenAIStreamDeltaFn delta_fn;
  void* delta_ctx;

  uint8_t saw_delta;
  uint8_t saw_error;
  uint8_t has_usage;

  uint64_t prompt_tokens;
  uint64_t completion_tokens;
  uint64_t total_tokens;

  agent_string_t assistant;
  agent_string_t pending_delta;
  agent_string_t error_message;
} OpenAIStreamCoreAdapter;

void openai_stream_core_init(
  OpenAIStreamCoreAdapter* adapter,
  const OpenAIStreamCoreConfig* cfg,
  OpenAIStreamDeltaFn delta_fn,
  void* delta_ctx
);

void openai_stream_core_reset(OpenAIStreamCoreAdapter* adapter);
void openai_stream_core_free(OpenAIStreamCoreAdapter* adapter);

agent_status_t openai_stream_core_feed_chunk(
  OpenAIStreamCoreAdapter* adapter,
  const char* chunk_json,
  size_t chunk_len
);

void openai_stream_core_flush(OpenAIStreamCoreAdapter* adapter);
