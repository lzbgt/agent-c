#pragma once

#include "agent/agent.h"
#include "agent/sse_parser.h"
#include "agent/tool_provider.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_stream_tool_call_delta {
  size_t index;
  agent_string_t id;
  agent_string_t name;
  agent_string_t arguments_delta;
} agent_stream_tool_call_delta_t;

typedef struct agent_stream_chunk {
  uint8_t has_delta_text;
  agent_string_t delta_text;

  agent_stream_tool_call_delta_t* tool_calls;
  size_t tool_calls_count;

  uint8_t has_finish_reason;
  agent_string_t finish_reason;

  uint8_t has_usage;
  uint64_t prompt_tokens;
  uint64_t completion_tokens;
  uint64_t total_tokens;

  uint8_t has_error;
  agent_string_t error_message;
} agent_stream_chunk_t;

void agent_stream_chunk_free(agent_stream_chunk_t* chunk);

typedef agent_status_t (*agent_stream_decode_fn)(
  void* ctx,
  const char* data,
  size_t len,
  agent_stream_chunk_t* out_chunk
);

typedef void (*agent_stream_event_fn)(
  void* ctx,
  const char* type,
  const char* data_json
);

typedef struct agent_stream_config {
  size_t max_tool_calls_total;
  size_t max_tool_call_args_chars;
  size_t max_events_per_feed;
  uint64_t step;
  uint64_t epoch;
} agent_stream_config_t;

typedef struct agent_stream_decoder {
  agent_sse_parser_t parser;
  agent_stream_config_t cfg;
  agent_stream_decode_fn decode_fn;
  void* decode_ctx;
  agent_stream_event_fn event_fn;
  void* event_ctx;

  agent_tool_call_t* tool_calls;
  size_t tool_call_count;
  size_t tool_call_cap;
} agent_stream_decoder_t;

void agent_stream_decoder_init(
  agent_stream_decoder_t* dec,
  const agent_stream_config_t* cfg,
  agent_stream_decode_fn decode_fn,
  void* decode_ctx,
  agent_stream_event_fn event_fn,
  void* event_ctx
);

void agent_stream_decoder_reset(agent_stream_decoder_t* dec);
void agent_stream_decoder_free(agent_stream_decoder_t* dec);

agent_status_t agent_stream_decoder_feed(
  agent_stream_decoder_t* dec,
  const char* bytes,
  size_t len
);

agent_status_t agent_stream_decoder_copy_tool_calls(
  const agent_stream_decoder_t* dec,
  agent_tool_call_t** out_calls,
  size_t* out_count
);

#ifdef __cplusplus
}  // extern "C"
#endif
