#include "agent/cbor_det.h"

#include <string.h>

// Max entries we support for key sorting without allocation.
// UM‑BMP envelopes are small; this keeps stack usage bounded for MCUs.
#define AGENT_CBOR_DET_MAX_MAP_PAIRS 64

static agent_status_t ensure(agent_cbor_writer_t* w, size_t n) {
  if (!w || !w->buf) return AGENT_ERR_INVALID_ARGUMENT;
  if (w->len > w->cap) return AGENT_ERR_INTERNAL;
  if (n > (w->cap - w->len)) return AGENT_ERR_BOUNDS;
  return AGENT_OK;
}

static void put_u8(agent_cbor_writer_t* w, uint8_t b) {
  w->buf[w->len++] = b;
}

static void put_be_u16(agent_cbor_writer_t* w, uint16_t v) {
  put_u8(w, (uint8_t)((v >> 8) & 0xff));
  put_u8(w, (uint8_t)(v & 0xff));
}

static void put_be_u32(agent_cbor_writer_t* w, uint32_t v) {
  put_u8(w, (uint8_t)((v >> 24) & 0xff));
  put_u8(w, (uint8_t)((v >> 16) & 0xff));
  put_u8(w, (uint8_t)((v >> 8) & 0xff));
  put_u8(w, (uint8_t)(v & 0xff));
}

static void put_be_u64(agent_cbor_writer_t* w, uint64_t v) {
  for (int i = 7; i >= 0; i--) {
    put_u8(w, (uint8_t)((v >> (i * 8)) & 0xff));
  }
}

static agent_status_t put_ai_u64(agent_cbor_writer_t* w, uint8_t major_shift, uint64_t n) {
  // major_shift is already (major << 5).
  if (n < 24) {
    const agent_status_t st = ensure(w, 1);
    if (st != AGENT_OK) return st;
    put_u8(w, (uint8_t)(major_shift | (uint8_t)n));
    return AGENT_OK;
  }
  if (n <= 0xff) {
    const agent_status_t st = ensure(w, 2);
    if (st != AGENT_OK) return st;
    put_u8(w, (uint8_t)(major_shift | 24));
    put_u8(w, (uint8_t)n);
    return AGENT_OK;
  }
  if (n <= 0xffff) {
    const agent_status_t st = ensure(w, 3);
    if (st != AGENT_OK) return st;
    put_u8(w, (uint8_t)(major_shift | 25));
    put_be_u16(w, (uint16_t)n);
    return AGENT_OK;
  }
  if (n <= 0xffffffffULL) {
    const agent_status_t st = ensure(w, 5);
    if (st != AGENT_OK) return st;
    put_u8(w, (uint8_t)(major_shift | 26));
    put_be_u32(w, (uint32_t)n);
    return AGENT_OK;
  }
  {
    const agent_status_t st = ensure(w, 9);
    if (st != AGENT_OK) return st;
    put_u8(w, (uint8_t)(major_shift | 27));
    put_be_u64(w, n);
    return AGENT_OK;
  }
}

void agent_cbor_writer_init(agent_cbor_writer_t* w, uint8_t* buf, size_t cap) {
  if (!w) return;
  w->buf = buf;
  w->cap = cap;
  w->len = 0;
}

const uint8_t* agent_cbor_writer_bytes(const agent_cbor_writer_t* w) {
  if (!w) return NULL;
  return w->buf;
}

size_t agent_cbor_writer_len(const agent_cbor_writer_t* w) {
  if (!w) return 0;
  return w->len;
}

agent_status_t agent_cbor_write_uint(agent_cbor_writer_t* w, uint64_t v) {
  return put_ai_u64(w, (uint8_t)(0u << 5), v);
}

agent_status_t agent_cbor_write_int(agent_cbor_writer_t* w, int64_t v) {
  if (v >= 0) return agent_cbor_write_uint(w, (uint64_t)v);
  // CBOR negative: value is -1 - n.
  const uint64_t n = (uint64_t)(-1 - v);
  return put_ai_u64(w, (uint8_t)(1u << 5), n);
}

agent_status_t agent_cbor_write_bool(agent_cbor_writer_t* w, int v_true) {
  const agent_status_t st = ensure(w, 1);
  if (st != AGENT_OK) return st;
  put_u8(w, (uint8_t)(v_true ? 0xf5 : 0xf4));
  return AGENT_OK;
}

agent_status_t agent_cbor_write_null(agent_cbor_writer_t* w) {
  const agent_status_t st = ensure(w, 1);
  if (st != AGENT_OK) return st;
  put_u8(w, (uint8_t)0xf6);
  return AGENT_OK;
}

agent_status_t agent_cbor_write_text(agent_cbor_writer_t* w, const char* s, size_t n) {
  if (!s && n != 0) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = put_ai_u64(w, (uint8_t)(3u << 5), (uint64_t)n);
  if (st != AGENT_OK) return st;
  st = ensure(w, n);
  if (st != AGENT_OK) return st;
  if (n) {
    memcpy(w->buf + w->len, s, n);
    w->len += n;
  }
  return AGENT_OK;
}

agent_status_t agent_cbor_write_bytes(agent_cbor_writer_t* w, const uint8_t* p, size_t n) {
  if (!p && n != 0) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = put_ai_u64(w, (uint8_t)(2u << 5), (uint64_t)n);
  if (st != AGENT_OK) return st;
  st = ensure(w, n);
  if (st != AGENT_OK) return st;
  if (n) {
    memcpy(w->buf + w->len, p, n);
    w->len += n;
  }
  return AGENT_OK;
}

agent_status_t agent_cbor_write_f64(agent_cbor_writer_t* w, double d) {
  agent_status_t st = ensure(w, 1 + 8);
  if (st != AGENT_OK) return st;
  put_u8(w, (uint8_t)0xfb);
  uint64_t bits = 0;
  // NOLINTNEXTLINE: memcpy-based type punning is portable.
  memcpy(&bits, &d, 8);
  put_be_u64(w, bits);
  return AGENT_OK;
}

agent_status_t agent_cbor_write_array_start(agent_cbor_writer_t* w, size_t n) {
  return put_ai_u64(w, (uint8_t)(4u << 5), (uint64_t)n);
}

agent_status_t agent_cbor_write_map_start(agent_cbor_writer_t* w, size_t n_pairs) {
  return put_ai_u64(w, (uint8_t)(5u << 5), (uint64_t)n_pairs);
}

static int key_less(const agent_cbor_kv_t* a, const agent_cbor_kv_t* b) {
  if (a->key_len != b->key_len) return a->key_len < b->key_len;
  if (a->key_len == 0) return 0;
  const int cmp = memcmp(a->key, b->key, a->key_len);
  return cmp < 0;
}

static int key_equal(const agent_cbor_kv_t* a, const agent_cbor_kv_t* b) {
  if (a->key_len != b->key_len) return 0;
  if (a->key_len == 0) return 1;
  return memcmp(a->key, b->key, a->key_len) == 0;
}

agent_status_t agent_cbor_write_map_sorted(agent_cbor_writer_t* w, const agent_cbor_kv_t* pairs, size_t n_pairs) {
  if (!w || !w->buf) return AGENT_ERR_INVALID_ARGUMENT;
  if (!pairs && n_pairs != 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (n_pairs > AGENT_CBOR_DET_MAX_MAP_PAIRS) return AGENT_ERR_LIMIT;

  for (size_t i = 0; i < n_pairs; i++) {
    if (!pairs[i].key || pairs[i].key_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
    if (!pairs[i].encode_value) return AGENT_ERR_INVALID_ARGUMENT;
  }

  // Sort indices to avoid mutating caller memory.
  uint8_t idx[AGENT_CBOR_DET_MAX_MAP_PAIRS];
  for (size_t i = 0; i < n_pairs; i++) idx[i] = (uint8_t)i;

  // Insertion sort: stable, tiny, good for small N.
  for (size_t i = 1; i < n_pairs; i++) {
    const uint8_t cur = idx[i];
    size_t j = i;
    while (j > 0) {
      const agent_cbor_kv_t* a = &pairs[cur];
      const agent_cbor_kv_t* b = &pairs[idx[j - 1]];
      if (!key_less(a, b)) break;
      idx[j] = idx[j - 1];
      j--;
    }
    idx[j] = cur;
  }

  // Reject duplicates (ambiguous map semantics).
  for (size_t i = 1; i < n_pairs; i++) {
    const agent_cbor_kv_t* a = &pairs[idx[i - 1]];
    const agent_cbor_kv_t* b = &pairs[idx[i]];
    if (key_equal(a, b)) return AGENT_ERR_INVALID_ARGUMENT;
  }

  agent_status_t st = agent_cbor_write_map_start(w, n_pairs);
  if (st != AGENT_OK) return st;

  for (size_t i = 0; i < n_pairs; i++) {
    const agent_cbor_kv_t* kv = &pairs[idx[i]];
    st = agent_cbor_write_text(w, kv->key, kv->key_len);
    if (st != AGENT_OK) return st;
    st = kv->encode_value(w, kv->value_ctx);
    if (st != AGENT_OK) return st;
  }
  return AGENT_OK;
}

