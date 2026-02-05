#include "agent/json_c14n.h"

#include "agent_sha256.h"
#include "agent_alloc.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum jv_type {
  JV_NULL = 0,
  JV_BOOL = 1,
  JV_NUMBER = 2,  // raw token; normalized at emit time
  JV_STRING = 3,  // decoded UTF-8 bytes
  JV_ARRAY = 4,
  JV_OBJECT = 5,
} jv_type_t;

typedef struct jv_string {
  char* data;
  size_t len;
} jv_string_t;

typedef struct jv_value jv_value_t;

typedef struct jv_array {
  jv_value_t** items;
  size_t n;
  size_t cap;
} jv_array_t;

typedef struct jv_pair {
  jv_string_t key;
  jv_value_t* val;
} jv_pair_t;

typedef struct jv_object {
  jv_pair_t* items;
  size_t n;
  size_t cap;
} jv_object_t;

struct jv_value {
  jv_type_t t;
  union {
    int b;
    jv_string_t s;
    jv_string_t num;
    jv_array_t arr;
    jv_object_t obj;
  } u;
};

typedef struct buf {
  char* data;
  size_t len;
  size_t cap;
} buf_t;

static void err_set(char* err_buf, size_t err_cap, const char* msg) {
  if (!err_buf || err_cap == 0) return;
  if (!msg) msg = "error";
  (void)snprintf(err_buf, err_cap, "%s", msg);
}

static agent_status_t buf_reserve(buf_t* b, size_t add) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  if (add == 0) return AGENT_OK;
  const size_t need = b->len + add;
  if (need <= b->cap) return AGENT_OK;
  size_t ncap = b->cap ? b->cap : 256;
  while (ncap < need) {
    if (ncap > (size_t)(1u << 30)) return AGENT_ERR_BOUNDS;
    ncap *= 2;
  }
  char* nd = (char*)agent_malloc(ncap);
  if (!nd) return AGENT_ERR_OOM;
  if (b->data && b->len) memcpy(nd, b->data, b->len);
  if (b->data) agent_free(b->data);
  b->data = nd;
  b->cap = ncap;
  return AGENT_OK;
}

static agent_status_t buf_append_bytes(buf_t* b, const char* p, size_t n) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  if (!p && n) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = buf_reserve(b, n);
  if (st != AGENT_OK) return st;
  if (n) memcpy(b->data + b->len, p, n);
  b->len += n;
  return AGENT_OK;
}

static agent_status_t buf_append_char(buf_t* b, char c) {
  return buf_append_bytes(b, &c, 1);
}

static void buf_free(buf_t* b) {
  if (!b) return;
  if (b->data) agent_free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

typedef struct parser {
  const char* s;
  size_t len;
  size_t pos;
} parser_t;

static void skip_ws(parser_t* p) {
  while (p && p->pos < p->len) {
    const char c = p->s[p->pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') p->pos++;
    else break;
  }
}

static int peek(parser_t* p) {
  if (!p || p->pos >= p->len) return -1;
  return (unsigned char)p->s[p->pos];
}

static int take(parser_t* p) {
  if (!p || p->pos >= p->len) return -1;
  return (unsigned char)p->s[p->pos++];
}

static jv_value_t* jv_alloc(jv_type_t t) {
  jv_value_t* v = (jv_value_t*)agent_malloc(sizeof(jv_value_t));
  if (!v) return NULL;
  memset(v, 0, sizeof(*v));
  v->t = t;
  return v;
}

static void jv_free(jv_value_t* v) {
  if (!v) return;
  switch (v->t) {
    case JV_STRING:
      agent_free(v->u.s.data);
      break;
    case JV_NUMBER:
      agent_free(v->u.num.data);
      break;
    case JV_ARRAY:
      for (size_t i = 0; i < v->u.arr.n; i++) jv_free(v->u.arr.items[i]);
      agent_free(v->u.arr.items);
      break;
    case JV_OBJECT:
      for (size_t i = 0; i < v->u.obj.n; i++) {
        agent_free(v->u.obj.items[i].key.data);
        jv_free(v->u.obj.items[i].val);
      }
      agent_free(v->u.obj.items);
      break;
    default:
      break;
  }
  agent_free(v);
}

static agent_status_t arr_push(jv_array_t* a, jv_value_t* v) {
  if (!a || !v) return AGENT_ERR_INVALID_ARGUMENT;
  if (a->n + 1 > a->cap) {
    size_t ncap = a->cap ? a->cap * 2 : 8;
    jv_value_t** ni = (jv_value_t**)agent_malloc(ncap * sizeof(jv_value_t*));
    if (!ni) return AGENT_ERR_OOM;
    if (a->items && a->n) memcpy(ni, a->items, a->n * sizeof(jv_value_t*));
    agent_free(a->items);
    a->items = ni;
    a->cap = ncap;
  }
  a->items[a->n++] = v;
  return AGENT_OK;
}

static agent_status_t obj_push(jv_object_t* o, const jv_string_t* key, jv_value_t* v) {
  if (!o || !key || !key->data || !v) return AGENT_ERR_INVALID_ARGUMENT;
  if (o->n + 1 > o->cap) {
    size_t ncap = o->cap ? o->cap * 2 : 8;
    jv_pair_t* ni = (jv_pair_t*)agent_malloc(ncap * sizeof(jv_pair_t));
    if (!ni) return AGENT_ERR_OOM;
    if (o->items && o->n) memcpy(ni, o->items, o->n * sizeof(jv_pair_t));
    agent_free(o->items);
    o->items = ni;
    o->cap = ncap;
  }
  o->items[o->n].key = *key;
  o->items[o->n].val = v;
  o->n++;
  return AGENT_OK;
}

static agent_status_t buf_append_hex_u16(buf_t* b, uint16_t v) {
  char tmp[6];
  tmp[0] = '\\';
  tmp[1] = 'u';
  static const char* hex = "0123456789abcdef";
  tmp[2] = hex[(v >> 12) & 0xF];
  tmp[3] = hex[(v >> 8) & 0xF];
  tmp[4] = hex[(v >> 4) & 0xF];
  tmp[5] = hex[(v >> 0) & 0xF];
  return buf_append_bytes(b, tmp, sizeof(tmp));
}

static agent_status_t utf8_append_codepoint(buf_t* b, uint32_t cp) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  char out[4];
  size_t n = 0;
  if (cp <= 0x7F) {
    out[0] = (char)cp;
    n = 1;
  } else if (cp <= 0x7FF) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp <= 0xFFFF) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    n = 3;
  } else if (cp <= 0x10FFFF) {
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    n = 4;
  } else {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  return buf_append_bytes(b, out, n);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static agent_status_t parse_json_string(parser_t* p, jv_string_t* out, char* err_buf, size_t err_cap) {
  if (!p || !out) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (take(p) != '"') {
    err_set(err_buf, err_cap, "expected string");
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  buf_t b;
  memset(&b, 0, sizeof(b));
  while (p->pos < p->len) {
    const int c = take(p);
    if (c < 0) break;
    if (c == '"') {
      // done
      char* s = (char*)agent_malloc(b.len + 1);
      if (!s) {
        buf_free(&b);
        return AGENT_ERR_OOM;
      }
      if (b.len) memcpy(s, b.data, b.len);
      s[b.len] = 0;
      out->data = s;
      out->len = b.len;
      buf_free(&b);
      return AGENT_OK;
    }
    if (c == '\\') {
      const int e = take(p);
      if (e < 0) {
        buf_free(&b);
        err_set(err_buf, err_cap, "unterminated escape");
        return AGENT_ERR_INVALID_ARGUMENT;
      }
      switch (e) {
        case '"':
        case '\\':
        case '/': {
          const char ch = (char)e;
          agent_status_t st = buf_append_char(&b, ch);
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        case 'b': {
          agent_status_t st = buf_append_char(&b, '\b');
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        case 'f': {
          agent_status_t st = buf_append_char(&b, '\f');
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        case 'n': {
          agent_status_t st = buf_append_char(&b, '\n');
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        case 'r': {
          agent_status_t st = buf_append_char(&b, '\r');
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        case 't': {
          agent_status_t st = buf_append_char(&b, '\t');
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        case 'u': {
          if (p->pos + 4 > p->len) {
            buf_free(&b);
            err_set(err_buf, err_cap, "short \\u escape");
            return AGENT_ERR_INVALID_ARGUMENT;
          }
          int h0 = hex_val(p->s[p->pos + 0]);
          int h1 = hex_val(p->s[p->pos + 1]);
          int h2 = hex_val(p->s[p->pos + 2]);
          int h3 = hex_val(p->s[p->pos + 3]);
          if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) {
            buf_free(&b);
            err_set(err_buf, err_cap, "invalid \\u escape");
            return AGENT_ERR_INVALID_ARGUMENT;
          }
          uint16_t u = (uint16_t)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
          p->pos += 4;
          uint32_t cp = u;
          // surrogate pair
          if (u >= 0xD800 && u <= 0xDBFF) {
            if (p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
              int g0 = hex_val(p->s[p->pos + 2]);
              int g1 = hex_val(p->s[p->pos + 3]);
              int g2 = hex_val(p->s[p->pos + 4]);
              int g3 = hex_val(p->s[p->pos + 5]);
              if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0) {
                uint16_t v = (uint16_t)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                if (v >= 0xDC00 && v <= 0xDFFF) {
                  p->pos += 6;
                  cp = 0x10000u + (((uint32_t)u - 0xD800u) << 10) + ((uint32_t)v - 0xDC00u);
                }
              }
            }
          }
          agent_status_t st = utf8_append_codepoint(&b, cp);
          if (st != AGENT_OK) {
            buf_free(&b);
            return st;
          }
          break;
        }
        default:
          buf_free(&b);
          err_set(err_buf, err_cap, "invalid escape");
          return AGENT_ERR_INVALID_ARGUMENT;
      }
    } else {
      if (c < 0x20) {
        buf_free(&b);
        err_set(err_buf, err_cap, "control char in string");
        return AGENT_ERR_INVALID_ARGUMENT;
      }
      const char ch = (char)c;
      agent_status_t st = buf_append_char(&b, ch);
      if (st != AGENT_OK) {
        buf_free(&b);
        return st;
      }
    }
  }

  buf_free(&b);
  err_set(err_buf, err_cap, "unterminated string");
  return AGENT_ERR_INVALID_ARGUMENT;
}

static agent_status_t parse_number_token(parser_t* p, jv_string_t* out, char* err_buf, size_t err_cap) {
  if (!p || !out) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  const size_t start = p->pos;
  int c = peek(p);
  if (c == '-') take(p);
  c = peek(p);
  if (c < 0) {
    err_set(err_buf, err_cap, "expected number");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (c == '0') {
    take(p);
  } else if (c >= '1' && c <= '9') {
    take(p);
    while ((c = peek(p)) >= 0 && c >= '0' && c <= '9') take(p);
  } else {
    err_set(err_buf, err_cap, "invalid number");
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  if (peek(p) == '.') {
    take(p);
    c = peek(p);
    if (!(c >= '0' && c <= '9')) {
      err_set(err_buf, err_cap, "invalid number fraction");
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    while ((c = peek(p)) >= 0 && c >= '0' && c <= '9') take(p);
  }

  c = peek(p);
  if (c == 'e' || c == 'E') {
    take(p);
    c = peek(p);
    if (c == '+' || c == '-') take(p);
    c = peek(p);
    if (!(c >= '0' && c <= '9')) {
      err_set(err_buf, err_cap, "invalid number exponent");
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    while ((c = peek(p)) >= 0 && c >= '0' && c <= '9') take(p);
  }

  const size_t end = p->pos;
  if (end <= start) {
    err_set(err_buf, err_cap, "invalid number");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  const size_t n = end - start;
  char* s = (char*)agent_malloc(n + 1);
  if (!s) return AGENT_ERR_OOM;
  memcpy(s, p->s + start, n);
  s[n] = 0;
  out->data = s;
  out->len = n;
  return AGENT_OK;
}

static agent_status_t parse_value(parser_t* p, jv_value_t** out, char* err_buf, size_t err_cap);

static agent_status_t parse_array(parser_t* p, jv_value_t** out, char* err_buf, size_t err_cap) {
  if (!p || !out) return AGENT_ERR_INVALID_ARGUMENT;
  if (take(p) != '[') return AGENT_ERR_INVALID_ARGUMENT;
  jv_value_t* v = jv_alloc(JV_ARRAY);
  if (!v) return AGENT_ERR_OOM;
  skip_ws(p);
  if (peek(p) == ']') {
    take(p);
    *out = v;
    return AGENT_OK;
  }
  while (1) {
    skip_ws(p);
    jv_value_t* it = NULL;
    agent_status_t st = parse_value(p, &it, err_buf, err_cap);
    if (st != AGENT_OK) {
      jv_free(v);
      return st;
    }
    st = arr_push(&v->u.arr, it);
    if (st != AGENT_OK) {
      jv_free(it);
      jv_free(v);
      return st;
    }
    skip_ws(p);
    const int c = peek(p);
    if (c == ',') {
      take(p);
      continue;
    }
    if (c == ']') {
      take(p);
      *out = v;
      return AGENT_OK;
    }
    jv_free(v);
    err_set(err_buf, err_cap, "expected , or ] in array");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
}

static agent_status_t parse_object(parser_t* p, jv_value_t** out, char* err_buf, size_t err_cap) {
  if (!p || !out) return AGENT_ERR_INVALID_ARGUMENT;
  if (take(p) != '{') return AGENT_ERR_INVALID_ARGUMENT;
  jv_value_t* v = jv_alloc(JV_OBJECT);
  if (!v) return AGENT_ERR_OOM;
  skip_ws(p);
  if (peek(p) == '}') {
    take(p);
    *out = v;
    return AGENT_OK;
  }
  while (1) {
    skip_ws(p);
    if (peek(p) != '"') {
      jv_free(v);
      err_set(err_buf, err_cap, "expected object key string");
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    jv_string_t key;
    agent_status_t st = parse_json_string(p, &key, err_buf, err_cap);
    if (st != AGENT_OK) {
      jv_free(v);
      return st;
    }
    skip_ws(p);
    if (take(p) != ':') {
      agent_free(key.data);
      jv_free(v);
      err_set(err_buf, err_cap, "expected : in object");
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    skip_ws(p);
    jv_value_t* val = NULL;
    st = parse_value(p, &val, err_buf, err_cap);
    if (st != AGENT_OK) {
      agent_free(key.data);
      jv_free(v);
      return st;
    }
    st = obj_push(&v->u.obj, &key, val);
    if (st != AGENT_OK) {
      agent_free(key.data);
      jv_free(val);
      jv_free(v);
      return st;
    }
    skip_ws(p);
    const int c = peek(p);
    if (c == ',') {
      take(p);
      continue;
    }
    if (c == '}') {
      take(p);
      *out = v;
      return AGENT_OK;
    }
    jv_free(v);
    err_set(err_buf, err_cap, "expected , or } in object");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
}

static agent_status_t parse_value(parser_t* p, jv_value_t** out, char* err_buf, size_t err_cap) {
  if (!p || !out) return AGENT_ERR_INVALID_ARGUMENT;
  skip_ws(p);
  const int c = peek(p);
  if (c < 0) {
    err_set(err_buf, err_cap, "unexpected end of input");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (c == '"') {
    jv_string_t s;
    agent_status_t st = parse_json_string(p, &s, err_buf, err_cap);
    if (st != AGENT_OK) return st;
    jv_value_t* v = jv_alloc(JV_STRING);
    if (!v) {
      agent_free(s.data);
      return AGENT_ERR_OOM;
    }
    v->u.s = s;
    *out = v;
    return AGENT_OK;
  }
  if (c == '{') return parse_object(p, out, err_buf, err_cap);
  if (c == '[') return parse_array(p, out, err_buf, err_cap);
  if (c == 't') {
    if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "true", 4) == 0) {
      p->pos += 4;
      jv_value_t* v = jv_alloc(JV_BOOL);
      if (!v) return AGENT_ERR_OOM;
      v->u.b = 1;
      *out = v;
      return AGENT_OK;
    }
    err_set(err_buf, err_cap, "invalid literal");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (c == 'f') {
    if (p->pos + 5 <= p->len && memcmp(p->s + p->pos, "false", 5) == 0) {
      p->pos += 5;
      jv_value_t* v = jv_alloc(JV_BOOL);
      if (!v) return AGENT_ERR_OOM;
      v->u.b = 0;
      *out = v;
      return AGENT_OK;
    }
    err_set(err_buf, err_cap, "invalid literal");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (c == 'n') {
    if (p->pos + 4 <= p->len && memcmp(p->s + p->pos, "null", 4) == 0) {
      p->pos += 4;
      jv_value_t* v = jv_alloc(JV_NULL);
      if (!v) return AGENT_ERR_OOM;
      *out = v;
      return AGENT_OK;
    }
    err_set(err_buf, err_cap, "invalid literal");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    jv_string_t num;
    agent_status_t st = parse_number_token(p, &num, err_buf, err_cap);
    if (st != AGENT_OK) return st;
    jv_value_t* v = jv_alloc(JV_NUMBER);
    if (!v) {
      agent_free(num.data);
      return AGENT_ERR_OOM;
    }
    v->u.num = num;
    *out = v;
    return AGENT_OK;
  }
  err_set(err_buf, err_cap, "unexpected token");
  return AGENT_ERR_INVALID_ARGUMENT;
}

static int pair_cmp(const void* a, const void* b) {
  const jv_pair_t* pa = (const jv_pair_t*)a;
  const jv_pair_t* pb = (const jv_pair_t*)b;
  const size_t na = pa->key.len;
  const size_t nb = pb->key.len;
  const size_t n = na < nb ? na : nb;
  const int r = memcmp(pa->key.data, pb->key.data, n);
  if (r != 0) return r;
  if (na < nb) return -1;
  if (na > nb) return 1;
  return 0;
}

static agent_status_t emit_string(buf_t* b, const char* s, size_t n) {
  if (!b) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = buf_append_char(b, '"');
  if (st != AGENT_OK) return st;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      st = buf_append_char(b, '\\');
      if (st != AGENT_OK) return st;
      st = buf_append_char(b, (char)c);
      if (st != AGENT_OK) return st;
      continue;
    }
    switch (c) {
      case '\b':
        st = buf_append_bytes(b, "\\b", 2);
        if (st != AGENT_OK) return st;
        continue;
      case '\f':
        st = buf_append_bytes(b, "\\f", 2);
        if (st != AGENT_OK) return st;
        continue;
      case '\n':
        st = buf_append_bytes(b, "\\n", 2);
        if (st != AGENT_OK) return st;
        continue;
      case '\r':
        st = buf_append_bytes(b, "\\r", 2);
        if (st != AGENT_OK) return st;
        continue;
      case '\t':
        st = buf_append_bytes(b, "\\t", 2);
        if (st != AGENT_OK) return st;
        continue;
      default:
        break;
    }
    if (c < 0x20) {
      st = buf_append_hex_u16(b, (uint16_t)c);
      if (st != AGENT_OK) return st;
      continue;
    }
    st = buf_append_char(b, (char)c);
    if (st != AGENT_OK) return st;
  }
  return buf_append_char(b, '"');
}

static int64_t parse_i64_clamped(const char* s, size_t n, int* ok) {
  if (ok) *ok = 0;
  if (!s || n == 0) return 0;
  int neg = 0;
  size_t i = 0;
  if (s[i] == '-') {
    neg = 1;
    i++;
  } else if (s[i] == '+') {
    return 0;
  }
  if (i >= n) return 0;
  int64_t v = 0;
  for (; i < n; i++) {
    const char c = s[i];
    if (c < '0' || c > '9') return 0;
    const int d = c - '0';
    if (v > (INT64_MAX - d) / 10) return 0;
    v = v * 10 + d;
  }
  if (neg) v = -v;
  if (ok) *ok = 1;
  return v;
}

static agent_status_t emit_number_c14n(buf_t* b, const char* tok, size_t n, char* err_buf, size_t err_cap) {
  // Normalizes number tokens into a plain decimal form (no exponent).
  // Caps output growth to avoid pathological exponents.
  if (!b || !tok || n == 0) return AGENT_ERR_INVALID_ARGUMENT;
  const size_t kMaxDigits = 4096;
  const size_t kMaxOut = 8192;

  size_t i = 0;
  int neg = 0;
  if (tok[i] == '-') {
    neg = 1;
    i++;
  }
  if (i >= n) {
    err_set(err_buf, err_cap, "invalid number");
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  const size_t int_start = i;
  size_t int_end = i;
  if (tok[i] == '0') {
    i++;
    int_end = i;
  } else {
    while (i < n && tok[i] >= '0' && tok[i] <= '9') i++;
    int_end = i;
  }

  size_t frac_start = 0, frac_end = 0;
  if (i < n && tok[i] == '.') {
    i++;
    frac_start = i;
    while (i < n && tok[i] >= '0' && tok[i] <= '9') i++;
    frac_end = i;
  }

  int64_t exp_val = 0;
  if (i < n && (tok[i] == 'e' || tok[i] == 'E')) {
    i++;
    if (i >= n) {
      err_set(err_buf, err_cap, "invalid exponent");
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    int exp_neg = 0;
    if (tok[i] == '+' || tok[i] == '-') {
      exp_neg = (tok[i] == '-');
      i++;
    }
    const size_t exp_start = i;
    while (i < n && tok[i] >= '0' && tok[i] <= '9') i++;
    const size_t exp_end = i;
    int ok = 0;
    const int64_t ev = parse_i64_clamped(tok + exp_start, exp_end - exp_start, &ok);
    if (!ok) {
      err_set(err_buf, err_cap, "invalid exponent");
      return AGENT_ERR_INVALID_ARGUMENT;
    }
    exp_val = exp_neg ? -ev : ev;
  }

  if (i != n) {
    err_set(err_buf, err_cap, "invalid number");
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  const size_t int_len = int_end - int_start;
  const size_t frac_len = frac_end > frac_start ? (frac_end - frac_start) : 0;
  const size_t digits_len = int_len + frac_len;
  if (digits_len == 0 || digits_len > kMaxDigits) {
    err_set(err_buf, err_cap, "number too large");
    return AGENT_ERR_BOUNDS;
  }

  char* digits = (char*)agent_malloc(digits_len);
  if (!digits) return AGENT_ERR_OOM;
  memcpy(digits, tok + int_start, int_len);
  if (frac_len) memcpy(digits + int_len, tok + frac_start, frac_len);

  // All-zero collapses to "0" (no sign).
  int any_nonzero = 0;
  for (size_t k = 0; k < digits_len; k++) {
    if (digits[k] != '0') {
      any_nonzero = 1;
      break;
    }
  }
  if (!any_nonzero) {
    agent_free(digits);
    return buf_append_char(b, '0');
  }

  // Decimal point position relative to digits[].
  int64_t dot_pos = (int64_t)int_len + exp_val;

  // Guard huge expansions.
  int64_t out_len_est = 0;
  if (dot_pos <= 0) out_len_est = 2 + (-dot_pos) + (int64_t)digits_len;       // "0." + zeros + digits
  else if (dot_pos >= (int64_t)digits_len) out_len_est = dot_pos;             // digits + zeros
  else out_len_est = (int64_t)digits_len + 1;                                 // digits + dot
  if (out_len_est < 0 || out_len_est > (int64_t)kMaxOut) {
    agent_free(digits);
    err_set(err_buf, err_cap, "number expansion too large");
    return AGENT_ERR_BOUNDS;
  }

  // Build integer and fraction views.
  const char* int_p = NULL;
  size_t int_n = 0;
  const char* frac_p = NULL;
  size_t frac_n = 0;

  char* frac_tmp = NULL;
  size_t frac_tmp_len = 0;

  if (dot_pos <= 0) {
    int_p = "0";
    int_n = 1;
    const size_t z = (size_t)(-dot_pos);
    frac_tmp_len = z + digits_len;
    frac_tmp = (char*)agent_malloc(frac_tmp_len);
    if (!frac_tmp) {
      agent_free(digits);
      return AGENT_ERR_OOM;
    }
    memset(frac_tmp, '0', z);
    memcpy(frac_tmp + z, digits, digits_len);
    frac_p = frac_tmp;
    frac_n = frac_tmp_len;
  } else if (dot_pos >= (int64_t)digits_len) {
    // Integer part is digits plus zeros appended.
    int_p = digits;
    int_n = digits_len;
    // trailing zeros are implicit; handled during emit.
    frac_p = NULL;
    frac_n = 0;
  } else {
    int_p = digits;
    int_n = (size_t)dot_pos;
    frac_p = digits + dot_pos;
    frac_n = digits_len - (size_t)dot_pos;
  }

  // Trim leading zeros in integer part (keep at least one digit).
  size_t int_off = 0;
  while (int_off + 1 < int_n && int_p[int_off] == '0') int_off++;
  int_p += int_off;
  int_n -= int_off;

  // Trim trailing zeros in fraction.
  while (frac_n > 0 && frac_p[frac_n - 1] == '0') frac_n--;

  // Emit sign (not for 0).
  agent_status_t st = AGENT_OK;
  if (neg) {
    st = buf_append_char(b, '-');
    if (st != AGENT_OK) goto done;
  }

  // Emit integer digits.
  st = buf_append_bytes(b, int_p, int_n);
  if (st != AGENT_OK) goto done;

  // Emit appended zeros if dot_pos is beyond digits.
  if (dot_pos > (int64_t)digits_len) {
    const size_t z = (size_t)(dot_pos - (int64_t)digits_len);
    for (size_t k = 0; k < z; k++) {
      st = buf_append_char(b, '0');
      if (st != AGENT_OK) goto done;
    }
  }

  if (frac_n > 0) {
    st = buf_append_char(b, '.');
    if (st != AGENT_OK) goto done;
    st = buf_append_bytes(b, frac_p, frac_n);
    if (st != AGENT_OK) goto done;
  }

done:
  agent_free(frac_tmp);
  agent_free(digits);
  return st;
}

static agent_status_t emit_value(buf_t* b, const jv_value_t* v, char* err_buf, size_t err_cap);

static agent_status_t emit_array(buf_t* b, const jv_array_t* a, char* err_buf, size_t err_cap) {
  agent_status_t st = buf_append_char(b, '[');
  if (st != AGENT_OK) return st;
  for (size_t i = 0; i < a->n; i++) {
    if (i) {
      st = buf_append_char(b, ',');
      if (st != AGENT_OK) return st;
    }
    st = emit_value(b, a->items[i], err_buf, err_cap);
    if (st != AGENT_OK) return st;
  }
  return buf_append_char(b, ']');
}

static agent_status_t emit_object(buf_t* b, const jv_object_t* o, char* err_buf, size_t err_cap) {
  agent_status_t st = buf_append_char(b, '{');
  if (st != AGENT_OK) return st;
  if (o->n == 0) return buf_append_char(b, '}');

  // Sort for canonical output.
  jv_pair_t* tmp = (jv_pair_t*)agent_malloc(o->n * sizeof(jv_pair_t));
  if (!tmp) return AGENT_ERR_OOM;
  memcpy(tmp, o->items, o->n * sizeof(jv_pair_t));
  qsort(tmp, o->n, sizeof(jv_pair_t), pair_cmp);

  for (size_t i = 0; i < o->n; i++) {
    if (i) {
      st = buf_append_char(b, ',');
      if (st != AGENT_OK) {
        agent_free(tmp);
        return st;
      }
    }
    st = emit_string(b, tmp[i].key.data, tmp[i].key.len);
    if (st != AGENT_OK) {
      agent_free(tmp);
      return st;
    }
    st = buf_append_char(b, ':');
    if (st != AGENT_OK) {
      agent_free(tmp);
      return st;
    }
    st = emit_value(b, tmp[i].val, err_buf, err_cap);
    if (st != AGENT_OK) {
      agent_free(tmp);
      return st;
    }
  }

  agent_free(tmp);
  return buf_append_char(b, '}');
}

static agent_status_t emit_value(buf_t* b, const jv_value_t* v, char* err_buf, size_t err_cap) {
  if (!b || !v) return AGENT_ERR_INVALID_ARGUMENT;
  switch (v->t) {
    case JV_NULL:
      return buf_append_bytes(b, "null", 4);
    case JV_BOOL:
      return v->u.b ? buf_append_bytes(b, "true", 4) : buf_append_bytes(b, "false", 5);
    case JV_STRING:
      return emit_string(b, v->u.s.data, v->u.s.len);
    case JV_NUMBER:
      return emit_number_c14n(b, v->u.num.data, v->u.num.len, err_buf, err_cap);
    case JV_ARRAY:
      return emit_array(b, &v->u.arr, err_buf, err_cap);
    case JV_OBJECT:
      return emit_object(b, &v->u.obj, err_buf, err_cap);
    default:
      err_set(err_buf, err_cap, "internal type");
      return AGENT_ERR_INTERNAL;
  }
}

agent_status_t agent_json_c14n_canonicalize(
  const char* json,
  size_t json_len,
  char** out_json,
  size_t* out_len,
  char* err_buf,
  size_t err_cap
) {
  if (err_buf && err_cap) err_buf[0] = 0;
  if (out_json) *out_json = NULL;
  if (out_len) *out_len = 0;
  if (!json || json_len == 0 || !out_json || !out_len) {
    err_set(err_buf, err_cap, "invalid arguments");
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  parser_t p;
  p.s = json;
  p.len = json_len;
  p.pos = 0;
  jv_value_t* root = NULL;
  agent_status_t st = parse_value(&p, &root, err_buf, err_cap);
  if (st != AGENT_OK) return st;
  skip_ws(&p);
  if (p.pos != p.len) {
    jv_free(root);
    err_set(err_buf, err_cap, "trailing junk after JSON value");
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  buf_t out;
  memset(&out, 0, sizeof(out));
  st = emit_value(&out, root, err_buf, err_cap);
  if (st != AGENT_OK) {
    jv_free(root);
    buf_free(&out);
    return st;
  }
  // NUL terminator.
  st = buf_append_char(&out, 0);
  if (st != AGENT_OK) {
    jv_free(root);
    buf_free(&out);
    return st;
  }

  jv_free(root);
  *out_json = out.data;
  *out_len = out.len - 1;
  return AGENT_OK;
}

agent_status_t agent_json_c14n_sha256_token(
  const char* json,
  size_t json_len,
  char out_token[80],
  char* err_buf,
  size_t err_cap
) {
  if (err_buf && err_cap) err_buf[0] = 0;
  if (!json || json_len == 0 || !out_token) {
    err_set(err_buf, err_cap, "invalid arguments");
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  char* c14n = NULL;
  size_t n = 0;
  agent_status_t st = agent_json_c14n_canonicalize(json, json_len, &c14n, &n, err_buf, err_cap);
  if (st != AGENT_OK) return st;
  char hex[65];
  memset(hex, 0, sizeof(hex));
  agent_sha256_hex_of_bytes(c14n, n, hex);
  agent_free(c14n);
  (void)snprintf(out_token, 80, "sha256:%s", hex);
  return AGENT_OK;
}
