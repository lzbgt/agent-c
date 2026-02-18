#include "agent_tool_loop_json.h"

#include "agent_alloc.h"

#include <string.h>

static size_t tl_u64_to_dec(char* out, size_t out_cap, unsigned long long x) {
  if (!out || out_cap == 0) return 0;
  // Max decimal digits for uint64 is 20.
  char tmp[32];
  size_t n = 0;
  do {
    const unsigned digit = (unsigned)(x % 10ull);
    tmp[n++] = (char)('0' + digit);
    x /= 10ull;
  } while (x && n + 1 < sizeof(tmp));
  if (n + 1 > out_cap) return 0;
  // Reverse into out.
  for (size_t i = 0; i < n; i++) {
    out[i] = tmp[n - 1 - i];
  }
  out[n] = '\0';
  return n;
}

static size_t tl_i64_to_dec(char* out, size_t out_cap, long long x) {
  if (!out || out_cap == 0) return 0;
  unsigned long long u = 0;
  size_t pos = 0;
  if (x < 0) {
    if (out_cap < 2) return 0;
    out[pos++] = '-';
    // Avoid overflow on LLONG_MIN.
    u = (unsigned long long)(-(x + 1ll)) + 1ull;
  } else {
    u = (unsigned long long)x;
  }
  const size_t wrote = tl_u64_to_dec(out + pos, out_cap - pos, u);
  if (wrote == 0) return 0;
  return pos + wrote;
}

static char tl_hex_nibble(unsigned v) {
  return (v < 10u) ? (char)('0' + v) : (char)('a' + (v - 10u));
}

void tl_buf_free(tl_buf_t* b) {
  if (!b) return;
  if (b->data) agent_free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

agent_status_t tl_buf_reserve(tl_buf_t* b, size_t need) {
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

agent_status_t tl_buf_append_bytes(tl_buf_t* b, const char* s, size_t n) {
  if (!b || (!s && n)) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = tl_buf_reserve(b, b->len + n + 1);
  if (st != AGENT_OK) return st;
  if (n) memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return AGENT_OK;
}

static agent_status_t tl_buf_append_escaped_u4(tl_buf_t* b, unsigned int x) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  // "\\u" + 4 hex digits
  char tmp[6];
  tmp[0] = '\\';
  tmp[1] = 'u';
  tmp[2] = tl_hex_nibble((x >> 12) & 0xFu);
  tmp[3] = tl_hex_nibble((x >> 8) & 0xFu);
  tmp[4] = tl_hex_nibble((x >> 4) & 0xFu);
  tmp[5] = tl_hex_nibble(x & 0xFu);
  return tl_buf_append_bytes(b, tmp, sizeof(tmp));
}

agent_status_t tl_buf_append_cstr(tl_buf_t* b, const char* s) {
  if (!s) s = "";
  return tl_buf_append_bytes(b, s, strlen(s));
}

agent_status_t tl_buf_append_char(tl_buf_t* b, char c) {
  return tl_buf_append_bytes(b, &c, 1);
}

agent_status_t tl_buf_append_u64(tl_buf_t* b, unsigned long long x) {
  char tmp[32];
  const size_t n = tl_u64_to_dec(tmp, sizeof(tmp), x);
  if (n == 0) return AGENT_ERR_INTERNAL;
  return tl_buf_append_bytes(b, tmp, n);
}

agent_status_t tl_buf_append_i64(tl_buf_t* b, long long x) {
  char tmp[32];
  const size_t n = tl_i64_to_dec(tmp, sizeof(tmp), x);
  if (n == 0) return AGENT_ERR_INTERNAL;
  return tl_buf_append_bytes(b, tmp, n);
}

agent_status_t tl_json_escape_into(tl_buf_t* b, const char* s, size_t n) {
  if (!b || (!s && n)) return AGENT_ERR_INVALID_ARGUMENT;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '\\') {
      agent_status_t st = tl_buf_append_cstr(b, "\\\\");
      if (st != AGENT_OK) return st;
    } else if (c == '"') {
      agent_status_t st = tl_buf_append_cstr(b, "\\\"");
      if (st != AGENT_OK) return st;
    } else if (c == '\n') {
      agent_status_t st = tl_buf_append_cstr(b, "\\n");
      if (st != AGENT_OK) return st;
    } else if (c == '\r') {
      agent_status_t st = tl_buf_append_cstr(b, "\\r");
      if (st != AGENT_OK) return st;
    } else if (c == '\t') {
      agent_status_t st = tl_buf_append_cstr(b, "\\t");
      if (st != AGENT_OK) return st;
    } else if (c < 0x20) {
      agent_status_t st = tl_buf_append_escaped_u4(b, (unsigned int)c);
      if (st != AGENT_OK) return st;
    } else {
      agent_status_t st = tl_buf_append_char(b, (char)c);
      if (st != AGENT_OK) return st;
    }
  }
  return AGENT_OK;
}

agent_status_t tl_json_append_string_field(
  tl_buf_t* b,
  const char* key,
  const char* value,
  size_t value_len,
  uint8_t* io_first
) {
  if (!b || !key || !io_first) return AGENT_ERR_INVALID_ARGUMENT;
  if (!value) value = "";
  if (!*io_first) {
    agent_status_t st = tl_buf_append_char(b, ',');
    if (st != AGENT_OK) return st;
  }
  *io_first = 0;
  agent_status_t st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, key); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, "\":\""); if (st != AGENT_OK) return st;
  st = tl_json_escape_into(b, value, value_len); if (st != AGENT_OK) return st;
  st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  return AGENT_OK;
}

agent_status_t tl_json_append_u64_field(
  tl_buf_t* b,
  const char* key,
  unsigned long long value,
  uint8_t* io_first
) {
  if (!b || !key || !io_first) return AGENT_ERR_INVALID_ARGUMENT;
  if (!*io_first) {
    agent_status_t st = tl_buf_append_char(b, ',');
    if (st != AGENT_OK) return st;
  }
  *io_first = 0;
  agent_status_t st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, key); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, "\":"); if (st != AGENT_OK) return st;
  st = tl_buf_append_u64(b, value); if (st != AGENT_OK) return st;
  return AGENT_OK;
}

agent_status_t tl_json_append_i64_field(
  tl_buf_t* b,
  const char* key,
  long long value,
  uint8_t* io_first
) {
  if (!b || !key || !io_first) return AGENT_ERR_INVALID_ARGUMENT;
  if (!*io_first) {
    agent_status_t st = tl_buf_append_char(b, ',');
    if (st != AGENT_OK) return st;
  }
  *io_first = 0;
  agent_status_t st = tl_buf_append_char(b, '"'); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, key); if (st != AGENT_OK) return st;
  st = tl_buf_append_cstr(b, "\":"); if (st != AGENT_OK) return st;
  st = tl_buf_append_i64(b, value); if (st != AGENT_OK) return st;
  return AGENT_OK;
}
