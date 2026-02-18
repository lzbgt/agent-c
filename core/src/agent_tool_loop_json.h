#pragma once

#include "agent/agent.h"

#include <stddef.h>
#include <stdint.h>

typedef struct tl_buf {
  char* data;
  size_t len;
  size_t cap;
} tl_buf_t;

void tl_buf_free(tl_buf_t* b);
agent_status_t tl_buf_reserve(tl_buf_t* b, size_t need);
agent_status_t tl_buf_append_bytes(tl_buf_t* b, const char* s, size_t n);
agent_status_t tl_buf_append_cstr(tl_buf_t* b, const char* s);
agent_status_t tl_buf_append_char(tl_buf_t* b, char c);
agent_status_t tl_buf_append_u64(tl_buf_t* b, unsigned long long x);
agent_status_t tl_buf_append_i64(tl_buf_t* b, long long x);

agent_status_t tl_json_escape_into(tl_buf_t* b, const char* s, size_t n);
agent_status_t tl_json_append_string_field(
  tl_buf_t* b,
  const char* key,
  const char* value,
  size_t value_len,
  uint8_t* io_first
);
agent_status_t tl_json_append_u64_field(
  tl_buf_t* b,
  const char* key,
  unsigned long long value,
  uint8_t* io_first
);
agent_status_t tl_json_append_i64_field(
  tl_buf_t* b,
  const char* key,
  long long value,
  uint8_t* io_first
);
