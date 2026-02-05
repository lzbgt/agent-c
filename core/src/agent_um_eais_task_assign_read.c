#include "agent/um_eais_task_assign_read.h"

#include "agent/edge_interop.h"

#include <string.h>

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

static agent_status_t require_map_item(agent_cbor_view_t item) {
  if (!item.ptr || item.len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, item.ptr, item.len);
  size_t n_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &n_pairs);
  if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  for (size_t i = 0; i < n_pairs; i++) {
    agent_cbor_text_view_t k;
    st = agent_cbor_read_text(&r, &k);
    if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (agent_cbor_reader_offset(&r) != item.len) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

agent_status_t agent_um_eais_task_assign_body_read_cbor_v0_1(
  const uint8_t* body_item,
  size_t body_item_len,
  agent_um_eais_task_assign_view_t* out
) {
  if (!body_item || body_item_len == 0 || !out) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, body_item, body_item_len);

  size_t n_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &n_pairs);
  if (st != AGENT_OK) return st;

  int has_task_id = 0;
  int has_step_id = 0;
  int has_idem = 0;
  int has_mode = 0;
  int has_deadline = 0;
  int has_payload = 0;

  for (size_t i = 0; i < n_pairs; i++) {
    agent_cbor_text_view_t k;
    st = agent_cbor_read_text(&r, &k);
    if (st != AGENT_OK) return st;

    if (text_eq(k, "node_id")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out->node_id = v;
      out->has_node_id = 1;
      continue;
    }

    if (text_eq(k, "task_id")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out->task_id = v;
      has_task_id = 1;
      continue;
    }

    if (text_eq(k, "step_id")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out->step_id = v;
      has_step_id = 1;
      continue;
    }

    if (text_eq(k, "idempotency_key")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!agent_umbmp_id_is_safe(v.ptr, v.len)) return AGENT_ERR_INVALID_ARGUMENT;
      out->idempotency_key = v;
      has_idem = 1;
      continue;
    }

    if (text_eq(k, "mode")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!text_eq(v, "invoke") && !text_eq(v, "agent")) return AGENT_ERR_INVALID_ARGUMENT;
      out->mode = v;
      has_mode = 1;
      continue;
    }

    if (text_eq(k, "deadline_utc_ms")) {
      uint64_t v = 0;
      st = agent_cbor_read_uint(&r, &v);
      if (st != AGENT_OK) return st;
      out->deadline_utc_ms = v;
      has_deadline = 1;
      continue;
    }

    if (text_eq(k, "attempt")) {
      uint64_t v = 0;
      st = agent_cbor_read_uint(&r, &v);
      if (st != AGENT_OK) return st;
      out->attempt = v;
      out->has_attempt = 1;
      continue;
    }

    if (text_eq(k, "payload")) {
      agent_cbor_view_t v;
      st = agent_cbor_read_item_view(&r, &v);
      if (st != AGENT_OK) return st;
      // v0.1 profile: payload must be a map (JSON object).
      st = require_map_item(v);
      if (st != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
      out->payload_item = v;
      has_payload = 1;
      continue;
    }

    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return st;
  }

  if (!has_task_id || !has_step_id || !has_idem || !has_mode || !has_deadline || !has_payload) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (agent_cbor_reader_offset(&r) != body_item_len) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}
