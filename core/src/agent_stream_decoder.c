#include "agent/stream_decoder.h"

#include "agent_alloc.h"

#include <string.h>

typedef struct sd_buf {
  char* data;
  size_t len;
  size_t cap;
} sd_buf_t;

static void sd_buf_free(sd_buf_t* b) {
  if (!b) return;
  if (b->data) agent_free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static agent_status_t sd_buf_reserve(sd_buf_t* b, size_t need) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  if (need <= b->cap) return AGENT_OK;
  size_t new_cap = b->cap == 0 ? 256 : b->cap;
  while (new_cap < need) new_cap *= 2;
  char* p = (char*)agent_malloc(new_cap);
  if (!p) return AGENT_ERR_OOM;
  if (b->data && b->len) memcpy(p, b->data, b->len);
  if (b->data) agent_free(b->data);
  b->data = p;
  b->cap = new_cap;
  return AGENT_OK;
}

static agent_status_t sd_buf_append_bytes(sd_buf_t* b, const char* s, size_t n) {
  if (!b || (!s && n)) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = sd_buf_reserve(b, b->len + n + 1);
  if (st != AGENT_OK) return st;
  if (n) memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return AGENT_OK;
}

static agent_status_t sd_buf_append_cstr(sd_buf_t* b, const char* s) {
  if (!s) s = "";
  return sd_buf_append_bytes(b, s, strlen(s));
}

static agent_status_t sd_buf_append_char(sd_buf_t* b, char c) {
  return sd_buf_append_bytes(b, &c, 1);
}

static char sd_hex_nibble(unsigned v) {
  return (v < 10u) ? (char)('0' + v) : (char)('a' + (v - 10u));
}

static agent_status_t sd_buf_append_escaped_u4(sd_buf_t* b, unsigned int x) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  char tmp[6];
  tmp[0] = '\\';
  tmp[1] = 'u';
  tmp[2] = sd_hex_nibble((x >> 12) & 0xFu);
  tmp[3] = sd_hex_nibble((x >> 8) & 0xFu);
  tmp[4] = sd_hex_nibble((x >> 4) & 0xFu);
  tmp[5] = sd_hex_nibble(x & 0xFu);
  return sd_buf_append_bytes(b, tmp, sizeof(tmp));
}

static agent_status_t sd_json_escape_into(sd_buf_t* b, const char* s, size_t n) {
  if (!b || (!s && n)) return AGENT_ERR_INVALID_ARGUMENT;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '\\': {
        agent_status_t st = sd_buf_append_cstr(b, "\\\\");
        if (st != AGENT_OK) return st;
        break;
      }
      case '"': {
        agent_status_t st = sd_buf_append_cstr(b, "\\\"");
        if (st != AGENT_OK) return st;
        break;
      }
      case '\n': {
        agent_status_t st = sd_buf_append_cstr(b, "\\n");
        if (st != AGENT_OK) return st;
        break;
      }
      case '\r': {
        agent_status_t st = sd_buf_append_cstr(b, "\\r");
        if (st != AGENT_OK) return st;
        break;
      }
      case '\t': {
        agent_status_t st = sd_buf_append_cstr(b, "\\t");
        if (st != AGENT_OK) return st;
        break;
      }
      default: {
        if (c < 0x20u) {
          agent_status_t st = sd_buf_append_escaped_u4(b, (unsigned int)c);
          if (st != AGENT_OK) return st;
        } else {
          agent_status_t st = sd_buf_append_char(b, (char)c);
          if (st != AGENT_OK) return st;
        }
        break;
      }
    }
  }
  return AGENT_OK;
}

static agent_status_t sd_buf_to_string(sd_buf_t* b, agent_string_t* out) {
  if (!b || !out) return AGENT_ERR_INVALID_ARGUMENT;
  agent_string_free(out);
  if (b->len == 0) {
    return agent_string_set_copy(out, "", 0);
  }
  return agent_string_set_copy(out, b->data, b->len);
}

static agent_status_t sd_append_kv_string(sd_buf_t* b, const char* key, const char* val, size_t val_len) {
  agent_status_t st = sd_buf_append_char(b, '"');
  if (st != AGENT_OK) return st;
  st = sd_buf_append_cstr(b, key);
  if (st != AGENT_OK) return st;
  st = sd_buf_append_cstr(b, "\":\"");
  if (st != AGENT_OK) return st;
  st = sd_json_escape_into(b, val ? val : "", val_len);
  if (st != AGENT_OK) return st;
  return sd_buf_append_char(b, '"');
}

static agent_status_t sd_append_kv_u64_pair(sd_buf_t* b, const char* key, uint64_t value) {
  agent_status_t st = sd_buf_append_char(b, '"');
  if (st != AGENT_OK) return st;
  st = sd_buf_append_cstr(b, key);
  if (st != AGENT_OK) return st;
  st = sd_buf_append_cstr(b, "\":");
  if (st != AGENT_OK) return st;
  char tmp[32];
  size_t n = 0;
  uint64_t x = value;
  do {
    tmp[n++] = (char)('0' + (x % 10));
    x /= 10;
  } while (x && n < sizeof(tmp));
  if (n == 0 || n >= sizeof(tmp)) return AGENT_ERR_INTERNAL;
  for (size_t i = 0; i < n / 2; i++) {
    char c = tmp[i];
    tmp[i] = tmp[n - 1 - i];
    tmp[n - 1 - i] = c;
  }
  return sd_buf_append_bytes(b, tmp, n);
}

static agent_status_t sd_build_assistant_delta_json(
  const agent_stream_decoder_t* dec,
  const char* delta,
  size_t delta_len,
  agent_string_t* out
) {
  sd_buf_t b = {0};
  agent_status_t st = sd_buf_append_char(&b, '{');
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_string(&b, "delta", delta, delta_len);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_cstr(&b, ",");
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_u64_pair(&b, "step", dec ? dec->cfg.step : 0);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_cstr(&b, ",");
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_u64_pair(&b, "epoch", dec ? dec->cfg.epoch : 0);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_char(&b, '}');
  if (st != AGENT_OK) goto done;
  st = sd_buf_to_string(&b, out);
done:
  sd_buf_free(&b);
  return st;
}

static agent_status_t sd_build_tool_call_delta_json(
  const agent_stream_tool_call_delta_t* delta,
  agent_string_t* out
) {
  sd_buf_t b = {0};
  agent_status_t st = sd_buf_append_char(&b, '{');
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_u64_pair(&b, "index", (uint64_t)delta->index);
  if (st != AGENT_OK) goto done;
  if (delta->id.data && delta->id.len) {
    st = sd_buf_append_cstr(&b, ",");
    if (st != AGENT_OK) goto done;
    st = sd_append_kv_string(&b, "tool_call_id", delta->id.data, delta->id.len);
    if (st != AGENT_OK) goto done;
  }
  if (delta->name.data && delta->name.len) {
    st = sd_buf_append_cstr(&b, ",");
    if (st != AGENT_OK) goto done;
    st = sd_append_kv_string(&b, "tool_name", delta->name.data, delta->name.len);
    if (st != AGENT_OK) goto done;
  }
  if (delta->arguments_delta.data && delta->arguments_delta.len) {
    st = sd_buf_append_cstr(&b, ",");
    if (st != AGENT_OK) goto done;
    st = sd_append_kv_string(&b, "arguments_delta", delta->arguments_delta.data, delta->arguments_delta.len);
    if (st != AGENT_OK) goto done;
  }
  st = sd_buf_append_char(&b, '}');
  if (st != AGENT_OK) goto done;
  st = sd_buf_to_string(&b, out);
done:
  sd_buf_free(&b);
  return st;
}

static agent_status_t sd_build_tool_call_json(
  const agent_tool_call_t* call,
  agent_string_t* out
) {
  sd_buf_t b = {0};
  agent_status_t st = sd_buf_append_char(&b, '{');
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_string(&b, "tool_name", call->name.data, call->name.len);
  if (st != AGENT_OK) goto done;
  if (call->id.data && call->id.len) {
    st = sd_buf_append_cstr(&b, ",");
    if (st != AGENT_OK) goto done;
    st = sd_append_kv_string(&b, "tool_call_id", call->id.data, call->id.len);
    if (st != AGENT_OK) goto done;
  }
  st = sd_buf_append_cstr(&b, ",");
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_string(&b, "arguments_json", call->arguments_json.data, call->arguments_json.len);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_char(&b, '}');
  if (st != AGENT_OK) goto done;
  st = sd_buf_to_string(&b, out);
done:
  sd_buf_free(&b);
  return st;
}

static agent_status_t sd_build_usage_json(
  const agent_stream_chunk_t* chunk,
  agent_string_t* out
) {
  sd_buf_t b = {0};
  agent_status_t st = sd_buf_append_char(&b, '{');
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_u64_pair(&b, "prompt_tokens", chunk->prompt_tokens);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_cstr(&b, ",");
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_u64_pair(&b, "completion_tokens", chunk->completion_tokens);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_cstr(&b, ",");
  if (st != AGENT_OK) goto done;
  st = sd_append_kv_u64_pair(&b, "total_tokens", chunk->total_tokens);
  if (st != AGENT_OK) goto done;
  st = sd_buf_append_char(&b, '}');
  if (st != AGENT_OK) goto done;
  st = sd_buf_to_string(&b, out);
done:
  sd_buf_free(&b);
  return st;
}

static void sd_emit_event(agent_stream_decoder_t* dec, const char* type, const agent_string_t* data_json) {
  if (!dec || !dec->event_fn || !type) return;
  dec->event_fn(dec->event_ctx, type, data_json && data_json->data ? data_json->data : "");
}

void agent_stream_chunk_free(agent_stream_chunk_t* chunk) {
  if (!chunk) return;
  agent_string_free(&chunk->delta_text);
  agent_string_free(&chunk->finish_reason);
  agent_string_free(&chunk->error_message);
  if (chunk->tool_calls) {
    for (size_t i = 0; i < chunk->tool_calls_count; i++) {
      agent_string_free(&chunk->tool_calls[i].id);
      agent_string_free(&chunk->tool_calls[i].name);
      agent_string_free(&chunk->tool_calls[i].arguments_delta);
    }
    agent_free(chunk->tool_calls);
  }
  memset(chunk, 0, sizeof(*chunk));
}

static agent_status_t sd_tool_calls_reserve(agent_stream_decoder_t* dec, size_t need) {
  if (need <= dec->tool_call_cap) return AGENT_OK;
  size_t new_cap = dec->tool_call_cap == 0 ? 4 : dec->tool_call_cap;
  while (new_cap < need) new_cap *= 2;
  agent_tool_call_t* p = (agent_tool_call_t*)agent_malloc(new_cap * sizeof(agent_tool_call_t));
  if (!p) return AGENT_ERR_OOM;
  memset(p, 0, new_cap * sizeof(agent_tool_call_t));
  if (dec->tool_calls && dec->tool_call_count) {
    memcpy(p, dec->tool_calls, dec->tool_call_count * sizeof(agent_tool_call_t));
    agent_free(dec->tool_calls);
  } else if (dec->tool_calls) {
    agent_free(dec->tool_calls);
  }
  dec->tool_calls = p;
  dec->tool_call_cap = new_cap;
  return AGENT_OK;
}

static agent_status_t sd_string_append(agent_string_t* dst, const char* data, size_t len) {
  if (!dst || (!data && len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (len == 0) return AGENT_OK;
  size_t new_len = dst->len + len;
  char* p = (char*)agent_malloc(new_len + 1);
  if (!p) return AGENT_ERR_OOM;
  if (dst->data && dst->len) memcpy(p, dst->data, dst->len);
  memcpy(p + dst->len, data, len);
  p[new_len] = '\0';
  if (dst->data) agent_free(dst->data);
  dst->data = p;
  dst->len = new_len;
  return AGENT_OK;
}

static agent_status_t sd_apply_tool_call_delta(
  agent_stream_decoder_t* dec,
  const agent_stream_tool_call_delta_t* delta
) {
  if (!dec || !delta) return AGENT_ERR_INVALID_ARGUMENT;
  const size_t idx = delta->index;
  if (dec->cfg.max_tool_calls_total > 0 && idx >= dec->cfg.max_tool_calls_total) {
    return AGENT_ERR_LIMIT;
  }
  agent_status_t st = sd_tool_calls_reserve(dec, idx + 1);
  if (st != AGENT_OK) return st;
  if (idx + 1 > dec->tool_call_count) dec->tool_call_count = idx + 1;
  agent_tool_call_t* dst = &dec->tool_calls[idx];
  if (delta->id.data && delta->id.len) {
    agent_string_free(&dst->id);
    st = agent_string_set_copy(&dst->id, delta->id.data, delta->id.len);
    if (st != AGENT_OK) return st;
  }
  if (delta->name.data && delta->name.len) {
    agent_string_free(&dst->name);
    st = agent_string_set_copy(&dst->name, delta->name.data, delta->name.len);
    if (st != AGENT_OK) return st;
  }
  if (delta->arguments_delta.data && delta->arguments_delta.len) {
    const size_t next_len = dst->arguments_json.len + delta->arguments_delta.len;
    if (dec->cfg.max_tool_call_args_chars > 0 &&
        next_len > dec->cfg.max_tool_call_args_chars) {
      return AGENT_ERR_LIMIT;
    }
    st = sd_string_append(&dst->arguments_json, delta->arguments_delta.data, delta->arguments_delta.len);
    if (st != AGENT_OK) return st;
  }
  return AGENT_OK;
}

static agent_status_t sd_emit_tool_calls(agent_stream_decoder_t* dec) {
  if (!dec) return AGENT_ERR_INVALID_ARGUMENT;
  for (size_t i = 0; i < dec->tool_call_count; i++) {
    agent_tool_call_t* call = &dec->tool_calls[i];
    if (!call->name.data || call->name.len == 0) continue;
    agent_string_t payload = {0};
    agent_status_t st = sd_build_tool_call_json(call, &payload);
    if (st != AGENT_OK) {
      agent_string_free(&payload);
      return st;
    }
    sd_emit_event(dec, "tool_call", &payload);
    agent_string_free(&payload);
  }
  return AGENT_OK;
}

void agent_stream_decoder_init(
  agent_stream_decoder_t* dec,
  const agent_stream_config_t* cfg,
  agent_stream_decode_fn decode_fn,
  void* decode_ctx,
  agent_stream_event_fn event_fn,
  void* event_ctx
) {
  if (!dec) return;
  memset(dec, 0, sizeof(*dec));
  agent_sse_parser_init(&dec->parser);
  if (cfg) dec->cfg = *cfg;
  dec->decode_fn = decode_fn;
  dec->decode_ctx = decode_ctx;
  dec->event_fn = event_fn;
  dec->event_ctx = event_ctx;
}

void agent_stream_decoder_reset(agent_stream_decoder_t* dec) {
  if (!dec) return;
  agent_sse_parser_reset(&dec->parser);
  if (dec->tool_calls) {
    for (size_t i = 0; i < dec->tool_call_count; i++) {
      agent_string_free(&dec->tool_calls[i].id);
      agent_string_free(&dec->tool_calls[i].name);
      agent_string_free(&dec->tool_calls[i].arguments_json);
    }
    agent_free(dec->tool_calls);
  }
  dec->tool_calls = NULL;
  dec->tool_call_count = 0;
  dec->tool_call_cap = 0;
}

void agent_stream_decoder_free(agent_stream_decoder_t* dec) {
  if (!dec) return;
  agent_stream_decoder_reset(dec);
  agent_sse_parser_free(&dec->parser);
}

agent_status_t agent_stream_decoder_feed(
  agent_stream_decoder_t* dec,
  const char* bytes,
  size_t len
) {
  if (!dec || (!bytes && len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!dec->decode_fn) return AGENT_ERR_INVALID_ARGUMENT;

  const size_t cap = dec->cfg.max_events_per_feed > 0 ? dec->cfg.max_events_per_feed : 32;
  agent_sse_event_t* events = (agent_sse_event_t*)agent_malloc(sizeof(agent_sse_event_t) * cap);
  if (!events) return AGENT_ERR_OOM;
  memset(events, 0, sizeof(agent_sse_event_t) * cap);

  size_t event_count = 0;
  agent_status_t st = agent_sse_parser_feed(&dec->parser, bytes, len, events, cap, &event_count);
  if (st != AGENT_OK) {
    for (size_t i = 0; i < event_count; i++) agent_sse_event_free(&events[i]);
    agent_free(events);
    return st;
  }

  for (size_t i = 0; i < event_count; i++) {
    agent_sse_event_t* ev = &events[i];
    if (!ev->data.data || ev->data.len == 0) {
      agent_sse_event_free(ev);
      continue;
    }

    agent_stream_chunk_t chunk;
    memset(&chunk, 0, sizeof(chunk));
    st = dec->decode_fn(dec->decode_ctx, ev->data.data, ev->data.len, &chunk);
    if (st != AGENT_OK) {
      agent_stream_chunk_free(&chunk);
      agent_sse_event_free(ev);
      break;
    }

    if (chunk.has_error && chunk.error_message.data && chunk.error_message.len) {
      agent_string_t payload = {0};
      sd_buf_t b = {0};
      agent_status_t st2 = sd_buf_append_char(&b, '{');
      if (st2 == AGENT_OK) st2 = sd_append_kv_string(&b, "error", chunk.error_message.data, chunk.error_message.len);
      if (st2 == AGENT_OK) st2 = sd_buf_append_char(&b, '}');
      if (st2 == AGENT_OK) st2 = sd_buf_to_string(&b, &payload);
      if (st2 == AGENT_OK) sd_emit_event(dec, "error", &payload);
      agent_string_free(&payload);
      sd_buf_free(&b);
      if (st2 != AGENT_OK) {
        agent_stream_chunk_free(&chunk);
        agent_sse_event_free(ev);
        st = st2;
        break;
      }
    }

    if (chunk.has_delta_text && chunk.delta_text.data) {
      agent_string_t payload = {0};
      st = sd_build_assistant_delta_json(dec, chunk.delta_text.data, chunk.delta_text.len, &payload);
      if (st == AGENT_OK) sd_emit_event(dec, "assistant_delta", &payload);
      agent_string_free(&payload);
      if (st != AGENT_OK) {
        agent_stream_chunk_free(&chunk);
        agent_sse_event_free(ev);
        break;
      }
    }

    for (size_t j = 0; j < chunk.tool_calls_count; j++) {
      agent_stream_tool_call_delta_t* d = &chunk.tool_calls[j];
      st = sd_apply_tool_call_delta(dec, d);
      if (st != AGENT_OK) break;
      agent_string_t payload = {0};
      agent_status_t st2 = sd_build_tool_call_delta_json(d, &payload);
      if (st2 == AGENT_OK) sd_emit_event(dec, "tool_call_delta", &payload);
      agent_string_free(&payload);
      if (st2 != AGENT_OK) {
        st = st2;
        break;
      }
    }
    if (st != AGENT_OK) {
      agent_stream_chunk_free(&chunk);
      agent_sse_event_free(ev);
      break;
    }

    if (chunk.has_usage) {
      agent_string_t payload = {0};
      st = sd_build_usage_json(&chunk, &payload);
      if (st == AGENT_OK) sd_emit_event(dec, "llm_usage", &payload);
      agent_string_free(&payload);
      if (st != AGENT_OK) {
        agent_stream_chunk_free(&chunk);
        agent_sse_event_free(ev);
        break;
      }
    }

    if (chunk.has_finish_reason) {
      st = sd_emit_tool_calls(dec);
      if (st != AGENT_OK) {
        agent_stream_chunk_free(&chunk);
        agent_sse_event_free(ev);
        break;
      }
      agent_string_t payload = {0};
      sd_buf_t b = {0};
      agent_status_t st2 = sd_buf_append_char(&b, '{');
      if (st2 == AGENT_OK) st2 = sd_append_kv_string(&b, "finish_reason", chunk.finish_reason.data, chunk.finish_reason.len);
      if (st2 == AGENT_OK) st2 = sd_buf_append_char(&b, '}');
      if (st2 == AGENT_OK) st2 = sd_buf_to_string(&b, &payload);
      if (st2 == AGENT_OK) sd_emit_event(dec, "end", &payload);
      agent_string_free(&payload);
      sd_buf_free(&b);
      if (st2 != AGENT_OK) {
        agent_stream_chunk_free(&chunk);
        agent_sse_event_free(ev);
        st = st2;
        break;
      }
    }

    agent_stream_chunk_free(&chunk);
    agent_sse_event_free(ev);
  }

  for (size_t i = 0; i < event_count; i++) {
    agent_sse_event_free(&events[i]);
  }
  agent_free(events);
  return st;
}

agent_status_t agent_stream_decoder_copy_tool_calls(
  const agent_stream_decoder_t* dec,
  agent_tool_call_t** out_calls,
  size_t* out_count
) {
  if (!dec || !out_calls || !out_count) return AGENT_ERR_INVALID_ARGUMENT;
  *out_calls = NULL;
  *out_count = 0;

  size_t count = 0;
  for (size_t i = 0; i < dec->tool_call_count; i++) {
    const agent_tool_call_t* call = &dec->tool_calls[i];
    if (call->name.data && call->name.len) count++;
  }
  if (count == 0) return AGENT_OK;

  agent_tool_call_t* out = (agent_tool_call_t*)agent_malloc(count * sizeof(agent_tool_call_t));
  if (!out) return AGENT_ERR_OOM;
  memset(out, 0, count * sizeof(agent_tool_call_t));

  size_t idx = 0;
  agent_status_t st = AGENT_OK;
  for (size_t i = 0; i < dec->tool_call_count; i++) {
    const agent_tool_call_t* call = &dec->tool_calls[i];
    if (!call->name.data || call->name.len == 0) continue;
    if (call->id.data && call->id.len) {
      st = agent_string_set_copy(&out[idx].id, call->id.data, call->id.len);
      if (st != AGENT_OK) break;
    }
    st = agent_string_set_copy(&out[idx].name, call->name.data, call->name.len);
    if (st != AGENT_OK) break;
    if (call->arguments_json.data && call->arguments_json.len) {
      st = agent_string_set_copy(&out[idx].arguments_json, call->arguments_json.data, call->arguments_json.len);
    } else {
      st = agent_string_set_copy(&out[idx].arguments_json, "", 0);
    }
    if (st != AGENT_OK) break;
    idx++;
  }

  if (idx != count) {
    for (size_t i = 0; i < count; i++) {
      agent_string_free(&out[i].id);
      agent_string_free(&out[i].name);
      agent_string_free(&out[i].arguments_json);
    }
    agent_free(out);
    return st == AGENT_OK ? AGENT_ERR_OOM : st;
  }

  *out_calls = out;
  *out_count = count;
  return AGENT_OK;
}
