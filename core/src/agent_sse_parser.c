#include "agent/sse_parser.h"

#include "agent_alloc.h"

#include <string.h>

static void sse_string_clear(agent_string_t* s) {
  if (!s) return;
  agent_string_free(s);
}

static agent_status_t sse_string_append(agent_string_t* s, const char* data, size_t len) {
  if (!s || (!data && len != 0)) return AGENT_ERR_INVALID_ARGUMENT;
  if (len == 0) return AGENT_OK;
  const size_t old_len = s->len;
  const size_t new_len = old_len + len;
  char* buf = (char*)agent_malloc(new_len + 1);
  if (!buf) return AGENT_ERR_OOM;
  if (s->data && old_len > 0) {
    memcpy(buf, s->data, old_len);
  }
  memcpy(buf + old_len, data, len);
  buf[new_len] = '\0';
  if (s->data) agent_free(s->data);
  s->data = buf;
  s->len = new_len;
  return AGENT_OK;
}

static int sse_field_equals(const char* field, size_t len, const char* target) {
  const size_t tlen = strlen(target);
  if (len != tlen) return 0;
  return memcmp(field, target, len) == 0;
}

static void sse_trim_cr(const char** line, size_t* len) {
  if (!line || !*line || !len || *len == 0) return;
  if ((*line)[*len - 1] == '\r') {
    *len = *len - 1;
  }
}

static agent_status_t sse_emit_event(
  agent_sse_parser_t* parser,
  agent_sse_event_t* out_events,
  size_t events_cap,
  size_t* out_events_count
) {
  if (!parser || !out_events_count) return AGENT_ERR_INVALID_ARGUMENT;
  if (parser->cur_event.len == 0 && parser->cur_id.len == 0 && parser->cur_data.len == 0) {
    return AGENT_OK;
  }
  if (out_events && events_cap > 0) {
    if (*out_events_count >= events_cap) {
      return AGENT_ERR_LIMIT;
    }
    // Strip trailing newline inserted between data lines.
    if (parser->cur_data.len > 0 && parser->cur_data.data[parser->cur_data.len - 1] == '\n') {
      parser->cur_data.data[parser->cur_data.len - 1] = '\0';
      parser->cur_data.len -= 1;
    }
    const size_t idx = *out_events_count;
    out_events[idx] = (agent_sse_event_t){0};
    out_events[idx].event = parser->cur_event;
    out_events[idx].id = parser->cur_id;
    out_events[idx].data = parser->cur_data;
    parser->cur_event.data = NULL;
    parser->cur_event.len = 0;
    parser->cur_id.data = NULL;
    parser->cur_id.len = 0;
    parser->cur_data.data = NULL;
    parser->cur_data.len = 0;
    *out_events_count = idx + 1;
  }
  sse_string_clear(&parser->cur_event);
  sse_string_clear(&parser->cur_id);
  sse_string_clear(&parser->cur_data);
  return AGENT_OK;
}

static agent_status_t sse_handle_line(
  agent_sse_parser_t* parser,
  const char* line,
  size_t line_len,
  agent_sse_event_t* out_events,
  size_t events_cap,
  size_t* out_events_count
) {
  if (!parser || !line || !out_events_count) return AGENT_ERR_INVALID_ARGUMENT;

  sse_trim_cr(&line, &line_len);
  if (line_len == 0) {
    return sse_emit_event(parser, out_events, events_cap, out_events_count);
  }
  if (line[0] == ':') {
    return AGENT_OK;
  }

  const char* colon = (const char*)memchr(line, ':', line_len);
  const char* field = line;
  size_t field_len = line_len;
  const char* value = NULL;
  size_t value_len = 0;

  if (colon) {
    field_len = (size_t)(colon - line);
    value = colon + 1;
    value_len = line_len - field_len - 1;
    if (value_len > 0 && value[0] == ' ') {
      value += 1;
      value_len -= 1;
    }
  } else {
    value = "";
    value_len = 0;
  }

  if (sse_field_equals(field, field_len, "event")) {
    return agent_string_set_copy(&parser->cur_event, value, value_len);
  }
  if (sse_field_equals(field, field_len, "id")) {
    return agent_string_set_copy(&parser->cur_id, value, value_len);
  }
  if (sse_field_equals(field, field_len, "data")) {
    agent_status_t st = sse_string_append(&parser->cur_data, value, value_len);
    if (st != AGENT_OK) return st;
    return sse_string_append(&parser->cur_data, "\n", 1);
  }
  return AGENT_OK;
}

void agent_sse_parser_init(agent_sse_parser_t* parser) {
  if (!parser) return;
  memset(parser, 0, sizeof(*parser));
}

void agent_sse_parser_reset(agent_sse_parser_t* parser) {
  if (!parser) return;
  sse_string_clear(&parser->buf);
  sse_string_clear(&parser->cur_event);
  sse_string_clear(&parser->cur_id);
  sse_string_clear(&parser->cur_data);
}

void agent_sse_parser_free(agent_sse_parser_t* parser) {
  if (!parser) return;
  agent_sse_parser_reset(parser);
}

void agent_sse_event_free(agent_sse_event_t* ev) {
  if (!ev) return;
  agent_string_free(&ev->event);
  agent_string_free(&ev->id);
  agent_string_free(&ev->data);
  ev->event.data = NULL;
  ev->event.len = 0;
  ev->id.data = NULL;
  ev->id.len = 0;
  ev->data.data = NULL;
  ev->data.len = 0;
}

agent_status_t agent_sse_parser_feed(
  agent_sse_parser_t* parser,
  const char* bytes,
  size_t len,
  agent_sse_event_t* out_events,
  size_t events_cap,
  size_t* out_events_count
) {
  if (!parser || (!bytes && len != 0) || !out_events_count) return AGENT_ERR_INVALID_ARGUMENT;
  *out_events_count = 0;
  if (!bytes || len == 0) return AGENT_OK;

  agent_status_t st = sse_string_append(&parser->buf, bytes, len);
  if (st != AGENT_OK) return st;

  for (;;) {
    if (!parser->buf.data || parser->buf.len == 0) break;
    const char* nl = (const char*)memchr(parser->buf.data, '\n', parser->buf.len);
    if (!nl) break;
    const size_t line_len = (size_t)(nl - parser->buf.data);
    st = sse_handle_line(parser, parser->buf.data, line_len, out_events, events_cap, out_events_count);
    if (st != AGENT_OK) {
      return st;
    }
    const size_t remaining = parser->buf.len - (line_len + 1);
    if (remaining > 0) {
      memmove(parser->buf.data, parser->buf.data + line_len + 1, remaining);
    }
    parser->buf.len = remaining;
    if (parser->buf.data) {
      parser->buf.data[parser->buf.len] = '\0';
    }
  }

  return AGENT_OK;
}
