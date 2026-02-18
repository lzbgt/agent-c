#include "agent/stream_decoder.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 16

typedef struct event_capture {
  size_t count;
  char types[MAX_EVENTS][32];
  agent_string_t payloads[MAX_EVENTS];
} event_capture_t;

static void capture_event(void* ctx, const char* type, const char* data_json) {
  event_capture_t* cap = (event_capture_t*)ctx;
  assert(cap);
  assert(cap->count < MAX_EVENTS);
  size_t idx = cap->count++;
  strncpy(cap->types[idx], type ? type : "", sizeof(cap->types[idx]) - 1);
  cap->types[idx][sizeof(cap->types[idx]) - 1] = '\0';
  assert(agent_string_set_copy(&cap->payloads[idx], data_json ? data_json : "",
                               data_json ? strlen(data_json) : 0) == AGENT_OK);
}

static int sd_has_prefix(const char* data, size_t len, const char* prefix) {
  const size_t plen = strlen(prefix);
  return len >= plen && memcmp(data, prefix, plen) == 0;
}

static agent_status_t sd_copy_null_terminated(const char* data, size_t len, char** out) {
  if (!out) return AGENT_ERR_INVALID_ARGUMENT;
  char* p = (char*)malloc(len + 1);
  if (!p) return AGENT_ERR_OOM;
  memcpy(p, data, len);
  p[len] = '\0';
  *out = p;
  return AGENT_OK;
}

static agent_status_t decode_test_chunk(
  void* ctx,
  const char* data,
  size_t len,
  agent_stream_chunk_t* out
) {
  (void)ctx;
  if (!data || !out) return AGENT_ERR_INVALID_ARGUMENT;

  char* buf = NULL;
  agent_status_t st = sd_copy_null_terminated(data, len, &buf);
  if (st != AGENT_OK) return st;

  if (sd_has_prefix(buf, len, "delta:")) {
    const char* payload = buf + strlen("delta:");
    out->has_delta_text = 1;
    st = agent_string_set_copy(&out->delta_text, payload, strlen(payload));
  } else if (sd_has_prefix(buf, len, "error:")) {
    const char* payload = buf + strlen("error:");
    out->has_error = 1;
    st = agent_string_set_copy(&out->error_message, payload, strlen(payload));
  } else if (sd_has_prefix(buf, len, "usage:")) {
    const char* payload = buf + strlen("usage:");
    char* end = NULL;
    unsigned long long prompt = strtoull(payload, &end, 10);
    if (!end || *end != ',') {
      free(buf);
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    unsigned long long completion = strtoull(end + 1, &end, 10);
    if (!end || *end != ',') {
      free(buf);
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    unsigned long long total = strtoull(end + 1, &end, 10);
    if (!end || *end != '\0') {
      free(buf);
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    out->has_usage = 1;
    out->prompt_tokens = (uint64_t)prompt;
    out->completion_tokens = (uint64_t)completion;
    out->total_tokens = (uint64_t)total;
  } else if (sd_has_prefix(buf, len, "finish:")) {
    const char* payload = buf + strlen("finish:");
    out->has_finish_reason = 1;
    st = agent_string_set_copy(&out->finish_reason, payload, strlen(payload));
  } else if (sd_has_prefix(buf, len, "tool_delta:")) {
    char* p = buf + strlen("tool_delta:");
    char* fields[4] = {0};
    for (int i = 0; i < 3; i++) {
      char* colon = strchr(p, ':');
      if (!colon) {
        free(buf);
        return AGENT_ERR_INVALID_ARGUMENT;
      }
      *colon = '\0';
      fields[i] = p;
      p = colon + 1;
    }
    fields[3] = p;
    char* end = NULL;
    unsigned long long idx = strtoull(fields[0], &end, 10);
    if (!end || *end != '\0') {
      free(buf);
      return AGENT_ERR_INVALID_ARGUMENT;
    }

    out->tool_calls_count = 1;
    out->tool_calls = (agent_stream_tool_call_delta_t*)agent_malloc(sizeof(agent_stream_tool_call_delta_t));
    if (!out->tool_calls) {
      free(buf);
      return AGENT_ERR_OOM;
    }
    memset(out->tool_calls, 0, sizeof(agent_stream_tool_call_delta_t));
    out->tool_calls[0].index = (size_t)idx;
    if (fields[1] && fields[1][0]) {
      st = agent_string_set_copy(&out->tool_calls[0].id, fields[1], strlen(fields[1]));
    }
    if (st == AGENT_OK && fields[2] && fields[2][0]) {
      st = agent_string_set_copy(&out->tool_calls[0].name, fields[2], strlen(fields[2]));
    }
    if (st == AGENT_OK && fields[3] && fields[3][0]) {
      st = agent_string_set_copy(&out->tool_calls[0].arguments_delta, fields[3], strlen(fields[3]));
    }
  } else {
    st = AGENT_ERR_INVALID_ARGUMENT;
  }

  free(buf);
  return st;
}

static void test_stream_decoder_happy_path(void) {
  agent_stream_decoder_t dec;
  agent_stream_config_t cfg = {
    .max_tool_calls_total = 4,
    .max_tool_call_args_chars = 2048,
    .max_events_per_feed = 16,
    .step = 7,
    .epoch = 42,
  };

  event_capture_t cap;
  memset(&cap, 0, sizeof(cap));

  agent_stream_decoder_init(&dec, &cfg, decode_test_chunk, NULL, capture_event, &cap);

  const char* sse =
    "data: delta:hello\n\n"
    "data: error:oops\n\n"
    "data: tool_delta:0:call_1:weather:{\"city\":\n\n"
    "data: tool_delta:0:::\"LA\"}\n\n"
    "data: usage:1,2,3\n\n"
    "data: finish:stop\n\n";

  assert(agent_stream_decoder_feed(&dec, sse, strlen(sse)) == AGENT_OK);

  assert(cap.count == 7);
  assert(strcmp(cap.types[0], "assistant_delta") == 0);
  assert(strcmp(cap.payloads[0].data, "{\"delta\":\"hello\",\"step\":7,\"epoch\":42}") == 0);

  assert(strcmp(cap.types[1], "error") == 0);
  assert(strcmp(cap.payloads[1].data, "{\"error\":\"oops\"}") == 0);

  assert(strcmp(cap.types[2], "tool_call_delta") == 0);
  assert(strcmp(cap.payloads[2].data,
                "{\"index\":0,\"tool_call_id\":\"call_1\",\"tool_name\":\"weather\",\"arguments_delta\":\"{\\\"city\\\":\"}") == 0);

  assert(strcmp(cap.types[3], "tool_call_delta") == 0);
  assert(strcmp(cap.payloads[3].data,
                "{\"index\":0,\"arguments_delta\":\"\\\"LA\\\"}\"}") == 0);

  assert(strcmp(cap.types[4], "llm_usage") == 0);
  assert(strcmp(cap.payloads[4].data,
                "{\"prompt_tokens\":1,\"completion_tokens\":2,\"total_tokens\":3}") == 0);

  assert(strcmp(cap.types[5], "tool_call") == 0);
  assert(strcmp(cap.payloads[5].data,
                "{\"tool_name\":\"weather\",\"tool_call_id\":\"call_1\",\"arguments_json\":\"{\\\"city\\\":\\\"LA\\\"}\"}") == 0);

  assert(strcmp(cap.types[6], "end") == 0);
  assert(strcmp(cap.payloads[6].data, "{\"finish_reason\":\"stop\"}") == 0);

  for (size_t i = 0; i < cap.count; i++) {
    agent_string_free(&cap.payloads[i]);
  }
  agent_stream_decoder_free(&dec);
}

void test_stream_decoder_module(void) {
  test_stream_decoder_happy_path();
  printf("test_stream_decoder_module OK\n");
}
