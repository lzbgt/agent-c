#include "agent/session_codec.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

typedef struct agent_sbuf {
  char* data;
  size_t len; // excludes terminator
  size_t cap; // includes terminator space
} agent_sbuf_t;

static void agent_sbuf_free(agent_sbuf_t* b) {
  if (!b) {
    return;
  }
  if (b->data) {
    agent_free(b->data);
  }
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

static agent_status_t agent_sbuf_reserve(agent_sbuf_t* b, size_t need_cap) {
  if (!b) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (need_cap <= b->cap) {
    return AGENT_OK;
  }
  size_t new_cap = b->cap == 0 ? 256 : b->cap;
  while (new_cap < need_cap) {
    new_cap *= 2;
  }
  char* next = (char*)agent_malloc(new_cap);
  if (!next) {
    return AGENT_ERR_OOM;
  }
  if (b->data && b->len) {
    memcpy(next, b->data, b->len);
  }
  if (b->data) {
    agent_free(b->data);
  }
  b->data = next;
  b->cap = new_cap;
  if (b->data) {
    b->data[b->len] = '\0';
  }
  return AGENT_OK;
}

static agent_status_t agent_sbuf_append_bytes(agent_sbuf_t* b, const char* s, size_t n) {
  if (!b || (!s && n != 0)) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  agent_status_t st = agent_sbuf_reserve(b, b->len + n + 1);
  if (st != AGENT_OK) {
    return st;
  }
  if (n) {
    memcpy(b->data + b->len, s, n);
  }
  b->len += n;
  b->data[b->len] = '\0';
  return AGENT_OK;
}

static agent_status_t agent_sbuf_append_cstr(agent_sbuf_t* b, const char* s) {
  if (!s) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  return agent_sbuf_append_bytes(b, s, strlen(s));
}

static agent_status_t agent_sbuf_append_char(agent_sbuf_t* b, char c) {
  return agent_sbuf_append_bytes(b, &c, 1);
}

static agent_status_t agent_sbuf_append_hex_byte(agent_sbuf_t* b, uint8_t v) {
  static const char* kHex = "0123456789ABCDEF";
  char out[4];
  out[0] = '\\';
  out[1] = 'x';
  out[2] = kHex[(v >> 4) & 0xF];
  out[3] = kHex[v & 0xF];
  return agent_sbuf_append_bytes(b, out, sizeof(out));
}

static agent_status_t agent_escape_content(const char* s, size_t n, agent_sbuf_t* out) {
  if (!out || (!s && n != 0)) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < n; i++) {
    const uint8_t b = (uint8_t)s[i];
    if (b == '\\') {
      agent_status_t st = agent_sbuf_append_cstr(out, "\\\\");
      if (st != AGENT_OK) return st;
      continue;
    }
    if (b == '\n') {
      agent_status_t st = agent_sbuf_append_cstr(out, "\\n");
      if (st != AGENT_OK) return st;
      continue;
    }
    if (b == '\r') {
      agent_status_t st = agent_sbuf_append_cstr(out, "\\r");
      if (st != AGENT_OK) return st;
      continue;
    }
    if (b == '\t') {
      agent_status_t st = agent_sbuf_append_cstr(out, "\\t");
      if (st != AGENT_OK) return st;
      continue;
    }
    if (b < 0x20 || b == 0x7F) {
      agent_status_t st = agent_sbuf_append_hex_byte(out, b);
      if (st != AGENT_OK) return st;
      continue;
    }
    agent_status_t st = agent_sbuf_append_char(out, (char)b);
    if (st != AGENT_OK) return st;
  }
  return AGENT_OK;
}

static int agent_hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static agent_status_t agent_unescape_content(const char* s, size_t n, agent_sbuf_t* out) {
  if (!out || (!s && n != 0)) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  for (size_t i = 0; i < n; i++) {
    const char c = s[i];
    if (c != '\\') {
      agent_status_t st = agent_sbuf_append_char(out, c);
      if (st != AGENT_OK) return st;
      continue;
    }
    if (i + 1 >= n) {
      return AGENT_ERR_INTERNAL;
    }
    const char esc = s[++i];
    if (esc == '\\') {
      agent_status_t st = agent_sbuf_append_char(out, '\\');
      if (st != AGENT_OK) return st;
      continue;
    }
    if (esc == 'n') {
      agent_status_t st = agent_sbuf_append_char(out, '\n');
      if (st != AGENT_OK) return st;
      continue;
    }
    if (esc == 'r') {
      agent_status_t st = agent_sbuf_append_char(out, '\r');
      if (st != AGENT_OK) return st;
      continue;
    }
    if (esc == 't') {
      agent_status_t st = agent_sbuf_append_char(out, '\t');
      if (st != AGENT_OK) return st;
      continue;
    }
    if (esc == 'x') {
      if (i + 2 >= n) {
        return AGENT_ERR_INTERNAL;
      }
      const int hi = agent_hex_value(s[i + 1]);
      const int lo = agent_hex_value(s[i + 2]);
      if (hi < 0 || lo < 0) {
        return AGENT_ERR_INTERNAL;
      }
      const uint8_t v = (uint8_t)((hi << 4) | lo);
      i += 2;
      agent_status_t st = agent_sbuf_append_char(out, (char)v);
      if (st != AGENT_OK) return st;
      continue;
    }
    return AGENT_ERR_INTERNAL;
  }
  return AGENT_OK;
}

agent_status_t agent_session_codec_encode_v1(const agent_session_t* session, agent_string_t* out_text) {
  if (!session || !out_text) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  agent_string_free(out_text);

  agent_sbuf_t b = {0};
  agent_status_t st = agent_sbuf_append_cstr(&b, "AGENT_SESSION\t1\n");
  if (st != AGENT_OK) {
    agent_sbuf_free(&b);
    return st;
  }

  const size_t n = agent_session_message_count(session);
  for (size_t i = 0; i < n; i++) {
    agent_message_view_t v = {0};
    st = agent_session_get_message(session, i, &v);
    if (st != AGENT_OK) {
      agent_sbuf_free(&b);
      return st;
    }
    st = agent_sbuf_append_cstr(&b, "M\t");
    if (st != AGENT_OK) {
      agent_sbuf_free(&b);
      return st;
    }
    st = agent_sbuf_append_cstr(&b, agent_role_to_string(v.role));
    if (st != AGENT_OK) {
      agent_sbuf_free(&b);
      return st;
    }
    st = agent_sbuf_append_char(&b, '\t');
    if (st != AGENT_OK) {
      agent_sbuf_free(&b);
      return st;
    }
    st = agent_escape_content(v.content, v.content_len, &b);
    if (st != AGENT_OK) {
      agent_sbuf_free(&b);
      return st;
    }
    st = agent_sbuf_append_char(&b, '\n');
    if (st != AGENT_OK) {
      agent_sbuf_free(&b);
      return st;
    }
  }

  out_text->data = b.data;
  out_text->len = b.len;
  return AGENT_OK;
}

static size_t agent_line_trim_cr(const char* line, size_t len) {
  if (len && line[len - 1] == '\r') {
    return len - 1;
  }
  return len;
}

static int agent_is_blank_line(const char* line, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (!isspace((unsigned char)line[i])) {
      return 0;
    }
  }
  return 1;
}

agent_status_t agent_session_codec_decode_v1(const char* data, size_t len, agent_session_t** out_session) {
  if ((!data && len != 0) || !out_session) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  *out_session = NULL;

  agent_session_t* s = NULL;
  agent_status_t st = agent_session_create(&s);
  if (st != AGENT_OK) {
    return st;
  }

  size_t i = 0;

  // Read first line (header).
  size_t header_start = 0;
  while (i < len && data[i] != '\n') i++;
  size_t header_len = i - header_start;
  if (i < len && data[i] == '\n') i++;
  header_len = agent_line_trim_cr(data + header_start, header_len);

  const char* kHeader = "AGENT_SESSION\t1";
  if (header_len != strlen(kHeader) || memcmp(data + header_start, kHeader, header_len) != 0) {
    agent_session_destroy(s);
    return AGENT_ERR_INTERNAL;
  }

  while (i < len) {
    const size_t line_start = i;
    while (i < len && data[i] != '\n') i++;
    size_t line_len = i - line_start;
    if (i < len && data[i] == '\n') i++;
    line_len = agent_line_trim_cr(data + line_start, line_len);

    const char* line = data + line_start;
    if (line_len == 0 || agent_is_blank_line(line, line_len)) {
      continue;
    }
    if (line[0] == '#') {
      continue;
    }
    if (line_len < 4 || line[0] != 'M' || line[1] != '\t') {
      agent_session_destroy(s);
      return AGENT_ERR_INTERNAL;
    }

    // Split: "M\t<role>\t<escaped_content>"
    const char* role_start = line + 2;
    const char* tab = (const char*)memchr(role_start, '\t', line_len - 2);
    if (!tab) {
      agent_session_destroy(s);
      return AGENT_ERR_INTERNAL;
    }
    const size_t role_len = (size_t)(tab - role_start);
    const char* content_start = tab + 1;
    const size_t content_len = (size_t)((line + line_len) - content_start);

    // Role is tiny; copy to a stack buffer.
    if (role_len == 0 || role_len > 32) {
      agent_session_destroy(s);
      return AGENT_ERR_INTERNAL;
    }
    char role_buf[33];
    memcpy(role_buf, role_start, role_len);
    role_buf[role_len] = '\0';

    agent_role_t role;
    if (agent_role_from_string(role_buf, &role) != AGENT_OK) {
      agent_session_destroy(s);
      return AGENT_ERR_INTERNAL;
    }

    agent_sbuf_t content = {0};
    st = agent_unescape_content(content_start, content_len, &content);
    if (st != AGENT_OK) {
      agent_sbuf_free(&content);
      agent_session_destroy(s);
      return st;
    }
    st = agent_session_add_message(s, role, content.data ? content.data : "");
    agent_sbuf_free(&content);
    if (st != AGENT_OK) {
      agent_session_destroy(s);
      return st;
    }
  }

  *out_session = s;
  return AGENT_OK;
}

