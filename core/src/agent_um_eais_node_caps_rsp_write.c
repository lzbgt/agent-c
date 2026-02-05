#include "agent/um_eais_node_caps_rsp_write.h"

#include <string.h>

#include "agent/edge_interop.h"

static agent_status_t require_id(const agent_cbor_text_view_t* v) {
  if (!v) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(v->ptr, v->len)) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static int det_key_cmp(const agent_cbor_text_view_t* a, const agent_cbor_text_view_t* b) {
  if (a->len < b->len) return -1;
  if (a->len > b->len) return 1;
  const int c = memcmp(a->ptr, b->ptr, a->len);
  if (c < 0) return -1;
  if (c > 0) return 1;
  return 0;
}

static agent_status_t validate_manifest_map_v0_1(const agent_cbor_view_t* manifest, int enforce_det_keys) {
  if (!manifest || !manifest->ptr || manifest->len == 0) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, manifest->ptr, manifest->len);

  size_t n_pairs = 0;
  if (agent_cbor_read_map_start(&r, &n_pairs) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_text_view_t prev = {0};
  int has_prev = 0;

  for (size_t i = 0; i < n_pairs; i++) {
    agent_cbor_text_view_t k = {0};
    if (agent_cbor_read_text(&r, &k) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
    if (k.len == 0 || !k.ptr) return AGENT_ERR_INVALID_ARGUMENT;

    if (enforce_det_keys) {
      if (has_prev) {
        const int c = det_key_cmp(&prev, &k);
        if (c >= 0) return AGENT_ERR_INVALID_ARGUMENT; // unsorted or duplicate
      }
      prev = k;
      has_prev = 1;
    }

    // value: any CBOR item
    if (agent_cbor_skip_item(&r) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  }

  // Ensure the manifest map is exactly one item (no trailing bytes).
  if (agent_cbor_reader_offset(&r) != manifest->len) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static agent_status_t write_raw_cbor_item(agent_cbor_writer_t* w, const uint8_t* p, size_t n) {
  if (!w || !p) return AGENT_ERR_INVALID_ARGUMENT;
  if (n == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (w->len > w->cap) return AGENT_ERR_INTERNAL;
  if (n > (w->cap - w->len)) return AGENT_ERR_BOUNDS;
  memcpy(w->buf + w->len, p, n);
  w->len += n;
  return AGENT_OK;
}

typedef struct encode_manifest_ctx {
  agent_cbor_view_t v;
} encode_manifest_ctx_t;

static agent_status_t encode_manifest_raw(agent_cbor_writer_t* w, void* ctx) {
  const encode_manifest_ctx_t* m = (const encode_manifest_ctx_t*)ctx;
  if (!m) return AGENT_ERR_INVALID_ARGUMENT;
  return write_raw_cbor_item(w, m->v.ptr, m->v.len);
}

static agent_status_t encode_text_view(agent_cbor_writer_t* w, void* ctx) {
  const agent_cbor_text_view_t* v = (const agent_cbor_text_view_t*)ctx;
  if (!v || !v->ptr) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, v->ptr, v->len);
}

agent_status_t agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_node_caps_rsp_body_t* b = (const agent_um_eais_node_caps_rsp_body_t*)ctx;
  if (require_id(&b->node_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (validate_manifest_map_v0_1(&b->manifest_cbor, b->enforce_deterministic_keys) != AGENT_OK) {
    return AGENT_ERR_INVALID_ARGUMENT;
  }

  encode_manifest_ctx_t mctx = {.v = b->manifest_cbor};
  const agent_cbor_kv_t pairs[] = {
    (agent_cbor_kv_t){
      .key = "node_id",
      .key_len = 7,
      .encode_value = encode_text_view,
      .value_ctx = (void*)&b->node_id,
    },
    (agent_cbor_kv_t){
      .key = "manifest",
      .key_len = 8,
      .encode_value = encode_manifest_raw,
      .value_ctx = (void*)&mctx,
    },
  };

  return agent_cbor_write_map_sorted(w, pairs, sizeof(pairs) / sizeof(pairs[0]));
}

