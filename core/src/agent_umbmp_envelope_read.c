#include "agent/umbmp_envelope_read.h"

#include "agent/edge_interop.h"

#include <string.h>

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

static agent_status_t parse_trace(agent_cbor_view_t trace_item, agent_umbmp_trace_view_t* out) {
  if (!out) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!trace_item.ptr || trace_item.len == 0) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, trace_item.ptr, trace_item.len);
  size_t n_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &n_pairs);
  if (st != AGENT_OK) return st;

  for (size_t i = 0; i < n_pairs; i++) {
    agent_cbor_text_view_t k;
    st = agent_cbor_read_text(&r, &k);
    if (st != AGENT_OK) return st;
    if (text_eq(k, "trace_id")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      // Best-effort: only surface if it passes the shared id-safe character set.
      if (agent_umbmp_trace_id_is_safe(v.ptr, v.len)) {
        out->trace_id = v;
        out->has_trace_id = 1;
      }
      continue;
    }
    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return st;
  }

  if (agent_cbor_reader_offset(&r) != trace_item.len) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static agent_status_t parse_auth(agent_cbor_view_t auth_item, agent_umbmp_auth_view_t* out) {
  if (!out) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  if (!auth_item.ptr || auth_item.len == 0) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, auth_item.ptr, auth_item.len);
  size_t n_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &n_pairs);
  if (st != AGENT_OK) return st;

  for (size_t i = 0; i < n_pairs; i++) {
    agent_cbor_text_view_t k;
    st = agent_cbor_read_text(&r, &k);
    if (st != AGENT_OK) return st;

    if (text_eq(k, "alg")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      out->alg = v;
      continue;
    }

    if (text_eq(k, "kid")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      out->kid = v;
      continue;
    }

    if (text_eq(k, "seq")) {
      uint64_t v = 0;
      st = agent_cbor_read_uint(&r, &v);
      if (st != AGENT_OK) return st;
      out->seq = v;
      out->has_seq = 1;
      continue;
    }

    if (text_eq(k, "sig")) {
      // v0.1 wire profile uses text (base64); keep strict here.
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      out->sig = v;
      out->has_sig = 1;
      continue;
    }

    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return st;
  }

  if (agent_cbor_reader_offset(&r) != auth_item.len) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

agent_status_t agent_umbmp_envelope_read_cbor_v0_1(
  const uint8_t* buf,
  size_t len,
  agent_umbmp_envelope_view_t* out_env
) {
  if (!buf || len == 0 || !out_env) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out_env, 0, sizeof(*out_env));

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, buf, len);

  size_t n_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &n_pairs);
  if (st != AGENT_OK) return st;

  int has_msg_id = 0;
  int has_ts = 0;
  int has_type = 0;
  int has_from = 0;
  int has_to = 0;
  int has_body = 0;

  for (size_t i = 0; i < n_pairs; i++) {
    agent_cbor_text_view_t k;
    st = agent_cbor_read_text(&r, &k);
    if (st != AGENT_OK) return st;

    if (text_eq(k, "msg_id")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out_env->msg_id = v;
      has_msg_id = 1;
      continue;
    }

    if (text_eq(k, "ts_utc_ms")) {
      uint64_t v = 0;
      st = agent_cbor_read_uint(&r, &v);
      if (st != AGENT_OK) return st;
      out_env->ts_utc_ms = v;
      has_ts = 1;
      continue;
    }

    if (text_eq(k, "type")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out_env->type = v;
      has_type = 1;
      continue;
    }

    if (text_eq(k, "from")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out_env->from = v;
      has_from = 1;
      continue;
    }

    if (text_eq(k, "to")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out_env->to = v;
      has_to = 1;
      continue;
    }

    if (text_eq(k, "body")) {
      agent_cbor_view_t v;
      st = agent_cbor_read_item_view(&r, &v);
      if (st != AGENT_OK) return st;

      // Enforce v0.1 profile semantics: body must be a map (JSON object).
      agent_cbor_reader_t br;
      agent_cbor_reader_init(&br, v.ptr, v.len);
      size_t bp = 0;
      st = agent_cbor_read_map_start(&br, &bp);
      if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
      // Consume remaining pairs to ensure map is well-formed.
      for (size_t j = 0; j < bp; j++) {
        agent_cbor_text_view_t bk;
        st = agent_cbor_read_text(&br, &bk);
        if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
        st = agent_cbor_skip_item(&br);
        if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
      }
      if (agent_cbor_reader_offset(&br) != v.len) return AGENT_ERR_INVALID_ARGUMENT;

      out_env->body_item = v;
      out_env->has_body = 1;
      has_body = 1;
      continue;
    }

    if (text_eq(k, "auth")) {
      agent_cbor_view_t v;
      st = agent_cbor_read_item_view(&r, &v);
      if (st != AGENT_OK) return st;
      agent_umbmp_auth_view_t a;
      st = parse_auth(v, &a);
      if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
      // Auth without alg/kid is meaningless; treat as invalid.
      if (!a.alg.ptr || a.alg.len == 0) return AGENT_ERR_INVALID_ARGUMENT;
      if (!a.kid.ptr || a.kid.len == 0) return AGENT_ERR_INVALID_ARGUMENT;
      if (!agent_umbmp_id_is_safe(a.kid.ptr, a.kid.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out_env->auth = a;
      out_env->has_auth = 1;
      continue;
    }

    if (text_eq(k, "trace")) {
      agent_cbor_view_t v;
      st = agent_cbor_read_item_view(&r, &v);
      if (st != AGENT_OK) return st;
      agent_umbmp_trace_view_t t;
      st = parse_trace(v, &t);
      if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
      out_env->trace = t;
      out_env->has_trace = 1;
      continue;
    }

    // Unknown key: skip value (forward compatible).
    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return st;
  }

  if (!has_msg_id || !has_ts || !has_type || !has_from || !has_to || !has_body) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_cbor_reader_offset(&r) != len) return AGENT_ERR_INVALID_ARGUMENT;

  return AGENT_OK;
}
