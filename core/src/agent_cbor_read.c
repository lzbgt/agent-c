#include "agent/cbor_read.h"

#include <limits.h>
#include <string.h>

enum {
  CBOR_AI_INDEFINITE = 31,
};

static agent_status_t ensure(const agent_cbor_reader_t* r, size_t need) {
  if (!r || !r->buf) return AGENT_ERR_INVALID_ARGUMENT;
  if (r->off > r->len) return AGENT_ERR_INTERNAL;
  if (need > (r->len - r->off)) return AGENT_ERR_BOUNDS;
  return AGENT_OK;
}

static agent_status_t read_u8(agent_cbor_reader_t* r, uint8_t* out) {
  const agent_status_t st = ensure(r, 1);
  if (st != AGENT_OK) return st;
  *out = r->buf[r->off++];
  return AGENT_OK;
}

static agent_status_t read_be_u64(agent_cbor_reader_t* r, size_t n_bytes, uint64_t* out) {
  if (!out) return AGENT_ERR_INVALID_ARGUMENT;
  if (n_bytes != 1 && n_bytes != 2 && n_bytes != 4 && n_bytes != 8) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_status_t st = ensure(r, n_bytes);
  if (st != AGENT_OK) return st;
  uint64_t v = 0;
  for (size_t i = 0; i < n_bytes; i++) {
    v = (v << 8) | (uint64_t)r->buf[r->off++];
  }
  *out = v;
  return AGENT_OK;
}

static agent_status_t read_head(agent_cbor_reader_t* r, uint8_t* out_major, uint8_t* out_ai, uint64_t* out_arg) {
  if (!out_major || !out_ai || !out_arg) return AGENT_ERR_INVALID_ARGUMENT;

  uint8_t b = 0;
  agent_status_t st = read_u8(r, &b);
  if (st != AGENT_OK) return st;

  const uint8_t major = (uint8_t)(b >> 5);
  const uint8_t ai = (uint8_t)(b & 0x1f);
  *out_major = major;
  *out_ai = ai;

  if (ai < 24) {
    *out_arg = (uint64_t)ai;
    return AGENT_OK;
  }

  if (ai == 24) return read_be_u64(r, 1, out_arg);
  if (ai == 25) return read_be_u64(r, 2, out_arg);
  if (ai == 26) return read_be_u64(r, 4, out_arg);
  if (ai == 27) return read_be_u64(r, 8, out_arg);

  if (ai == CBOR_AI_INDEFINITE) return AGENT_ERR_INVALID_ARGUMENT;  // definite-length only
  return AGENT_ERR_INVALID_ARGUMENT;
}

static agent_status_t read_container_count(agent_cbor_reader_t* r, uint64_t arg, size_t* out) {
  if (!out) return AGENT_ERR_INVALID_ARGUMENT;
  if (arg > (uint64_t)SIZE_MAX) return AGENT_ERR_LIMIT;
  const size_t n = (size_t)arg;
  if (n > r->max_container_items) return AGENT_ERR_LIMIT;
  *out = n;
  return AGENT_OK;
}

void agent_cbor_reader_init(agent_cbor_reader_t* r, const uint8_t* buf, size_t len) {
  if (!r) return;
  r->buf = buf;
  r->len = len;
  r->off = 0;
  r->depth = 0;
  r->max_depth = 16;
  r->max_container_items = 4096;
}

void agent_cbor_reader_set_max_depth(agent_cbor_reader_t* r, uint8_t max_depth) {
  if (!r) return;
  r->max_depth = max_depth;
}

void agent_cbor_reader_set_max_container_items(agent_cbor_reader_t* r, size_t max_items) {
  if (!r) return;
  r->max_container_items = max_items;
}

size_t agent_cbor_reader_offset(const agent_cbor_reader_t* r) {
  if (!r) return 0;
  return r->off;
}

agent_status_t agent_cbor_read_uint(agent_cbor_reader_t* r, uint64_t* out_v) {
  if (!out_v) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)ai;
  if (st != AGENT_OK) return st;
  if (major != 0) return AGENT_ERR_INVALID_ARGUMENT;
  *out_v = arg;
  return AGENT_OK;
}

agent_status_t agent_cbor_read_int(agent_cbor_reader_t* r, int64_t* out_v) {
  if (!out_v) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)ai;
  if (st != AGENT_OK) return st;
  if (major == 0) {
    if (arg > (uint64_t)INT64_MAX) return AGENT_ERR_LIMIT;
    *out_v = (int64_t)arg;
    return AGENT_OK;
  }
  if (major == 1) {
    if (arg > (uint64_t)INT64_MAX) return AGENT_ERR_LIMIT;
    const int64_t n = (int64_t)arg;
    *out_v = -n - 1;
    return AGENT_OK;
  }
  return AGENT_ERR_INVALID_ARGUMENT;
}

agent_status_t agent_cbor_read_bool(agent_cbor_reader_t* r, int* out_true) {
  if (!out_true) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)arg;
  if (st != AGENT_OK) return st;
  if (major != 7) return AGENT_ERR_INVALID_ARGUMENT;
  if (ai == 20) {
    *out_true = 0;
    return AGENT_OK;
  }
  if (ai == 21) {
    *out_true = 1;
    return AGENT_OK;
  }
  return AGENT_ERR_INVALID_ARGUMENT;
}

agent_status_t agent_cbor_read_null(agent_cbor_reader_t* r) {
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)arg;
  if (st != AGENT_OK) return st;
  if (major != 7) return AGENT_ERR_INVALID_ARGUMENT;
  if (ai != 22) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

agent_status_t agent_cbor_read_bytes(agent_cbor_reader_t* r, agent_cbor_view_t* out_view) {
  if (!out_view) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)ai;
  if (st != AGENT_OK) return st;
  if (major != 2) return AGENT_ERR_INVALID_ARGUMENT;
  if (arg > (uint64_t)SIZE_MAX) return AGENT_ERR_LIMIT;
  st = ensure(r, (size_t)arg);
  if (st != AGENT_OK) return st;
  out_view->ptr = r->buf + r->off;
  out_view->len = (size_t)arg;
  r->off += (size_t)arg;
  return AGENT_OK;
}

agent_status_t agent_cbor_read_text(agent_cbor_reader_t* r, agent_cbor_text_view_t* out_view) {
  if (!out_view) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)ai;
  if (st != AGENT_OK) return st;
  if (major != 3) return AGENT_ERR_INVALID_ARGUMENT;
  if (arg > (uint64_t)SIZE_MAX) return AGENT_ERR_LIMIT;
  st = ensure(r, (size_t)arg);
  if (st != AGENT_OK) return st;
  out_view->ptr = (const char*)(r->buf + r->off);
  out_view->len = (size_t)arg;
  r->off += (size_t)arg;
  return AGENT_OK;
}

agent_status_t agent_cbor_read_f64(agent_cbor_reader_t* r, double* out_v) {
  if (!out_v) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  if (st != AGENT_OK) return st;
  if (major != 7) return AGENT_ERR_INVALID_ARGUMENT;
  if (ai != 27) return AGENT_ERR_INVALID_ARGUMENT;
  const uint64_t bits = arg;
  double d = 0.0;
  // NOLINTNEXTLINE: memcpy-based type punning is portable.
  memcpy(&d, &bits, 8);
  *out_v = d;
  return AGENT_OK;
}

agent_status_t agent_cbor_read_array_start(agent_cbor_reader_t* r, size_t* out_n_items) {
  if (!out_n_items) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)ai;
  if (st != AGENT_OK) return st;
  if (major != 4) return AGENT_ERR_INVALID_ARGUMENT;
  return read_container_count(r, arg, out_n_items);
}

agent_status_t agent_cbor_read_map_start(agent_cbor_reader_t* r, size_t* out_n_pairs) {
  if (!out_n_pairs) return AGENT_ERR_INVALID_ARGUMENT;
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  const agent_status_t st = read_head(r, &major, &ai, &arg);
  (void)ai;
  if (st != AGENT_OK) return st;
  if (major != 5) return AGENT_ERR_INVALID_ARGUMENT;
  return read_container_count(r, arg, out_n_pairs);
}

static agent_status_t skip_n(agent_cbor_reader_t* r, size_t n) {
  const agent_status_t st = ensure(r, n);
  if (st != AGENT_OK) return st;
  r->off += n;
  return AGENT_OK;
}

agent_status_t agent_cbor_skip_item(agent_cbor_reader_t* r) {
  uint8_t major = 0;
  uint8_t ai = 0;
  uint64_t arg = 0;
  agent_status_t st = read_head(r, &major, &ai, &arg);
  if (st != AGENT_OK) return st;

  if (major == 0 || major == 1) {
    return AGENT_OK;
  }

  if (major == 2 || major == 3) {
    if (arg > (uint64_t)SIZE_MAX) return AGENT_ERR_LIMIT;
    return skip_n(r, (size_t)arg);
  }

  if (major == 4) {
    size_t n = 0;
    st = read_container_count(r, arg, &n);
    if (st != AGENT_OK) return st;
    if (r->depth >= r->max_depth) return AGENT_ERR_LIMIT;
    r->depth++;
    for (size_t i = 0; i < n; i++) {
      st = agent_cbor_skip_item(r);
      if (st != AGENT_OK) {
        r->depth--;
        return st;
      }
    }
    r->depth--;
    return AGENT_OK;
  }

  if (major == 5) {
    size_t n_pairs = 0;
    st = read_container_count(r, arg, &n_pairs);
    if (st != AGENT_OK) return st;
    if (r->depth >= r->max_depth) return AGENT_ERR_LIMIT;
    r->depth++;
    for (size_t i = 0; i < n_pairs; i++) {
      st = agent_cbor_skip_item(r);  // key
      if (st != AGENT_OK) {
        r->depth--;
        return st;
      }
      st = agent_cbor_skip_item(r);  // value
      if (st != AGENT_OK) {
        r->depth--;
        return st;
      }
    }
    r->depth--;
    return AGENT_OK;
  }

  if (major == 6) {
    // Tag value is in `arg`; ignore it, skip the tagged item.
    (void)arg;
    if (r->depth >= r->max_depth) return AGENT_ERR_LIMIT;
    r->depth++;
    st = agent_cbor_skip_item(r);
    r->depth--;
    return st;
  }

  if (major == 7) {
    // Simple / float: read_head already consumed any payload bytes (ai=24/25/26/27).
    // Reject break (ai=31) earlier.
    (void)ai;
    (void)arg;
    return AGENT_OK;
  }

  return AGENT_ERR_INVALID_ARGUMENT;
}

