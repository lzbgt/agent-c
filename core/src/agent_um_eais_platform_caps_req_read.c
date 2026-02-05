#include "agent/um_eais_platform_caps_req_read.h"

#include "agent/edge_interop.h"

#include <string.h>

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

agent_status_t agent_um_eais_platform_caps_req_body_read_cbor_v0_1(
  const uint8_t* body_item,
  size_t body_item_len,
  agent_um_eais_platform_caps_req_view_t* out
) {
  if (!body_item || body_item_len == 0 || !out) return AGENT_ERR_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, body_item, body_item_len);

  size_t n_pairs = 0;
  agent_status_t st = agent_cbor_read_map_start(&r, &n_pairs);
  if (st != AGENT_OK) return st;

  int has_node_id = 0;
  int has_want = 0;

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
      has_node_id = 1;
      continue;
    }

    if (text_eq(k, "want")) {
      agent_cbor_text_view_t v;
      st = agent_cbor_read_text(&r, &v);
      if (st != AGENT_OK) return st;
      if (!text_eq(v, "full") && !text_eq(v, "hash")) return AGENT_ERR_INVALID_ARGUMENT;
      out->want = v;
      has_want = 1;
      continue;
    }

    st = agent_cbor_skip_item(&r);
    if (st != AGENT_OK) return st;
  }

  if (!has_node_id || !has_want) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_cbor_reader_offset(&r) != body_item_len) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

