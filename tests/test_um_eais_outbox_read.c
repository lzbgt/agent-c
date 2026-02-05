#include "agent/um_eais_outbox_read.h"
#include "agent/um_eais_platform_caps_req_read.h"

#include "agent/cbor_det.h"
#include "agent/umbmp_auth.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

typedef struct {
  const char* s;
  size_t n;
} text_ctx_t;

typedef struct {
  uint64_t v;
} u64_ctx_t;

typedef struct {
  const agent_cbor_kv_t* pairs;
  size_t n_pairs;
} map_ctx_t;

static agent_status_t enc_text(agent_cbor_writer_t* w, void* ctx) {
  const text_ctx_t* t = (const text_ctx_t*)ctx;
  return agent_cbor_write_text(w, t->s, t->n);
}

static agent_status_t enc_u64(agent_cbor_writer_t* w, void* ctx) {
  const u64_ctx_t* u = (const u64_ctx_t*)ctx;
  return agent_cbor_write_uint(w, u->v);
}

static agent_status_t enc_bool(agent_cbor_writer_t* w, void* ctx) {
  const int* b = (const int*)ctx;
  return agent_cbor_write_bool(w, *b);
}

static agent_status_t enc_map(agent_cbor_writer_t* w, void* ctx) {
  const map_ctx_t* m = (const map_ctx_t*)ctx;
  assert(m);
  return agent_cbor_write_map_sorted(w, m->pairs, m->n_pairs);
}

typedef struct {
  size_t n;
  agent_status_t (*encode_item)(agent_cbor_writer_t* w, size_t idx, void* ctx);
  void* ctx;
} array_ctx_t;

static agent_status_t enc_array(agent_cbor_writer_t* w, void* ctx) {
  const array_ctx_t* a = (const array_ctx_t*)ctx;
  if (!a || !a->encode_item) return AGENT_ERR_INVALID_ARGUMENT;
  agent_status_t st = agent_cbor_write_array_start(w, a->n);
  if (st != AGENT_OK) return st;
  for (size_t i = 0; i < a->n; i++) {
    st = a->encode_item(w, i, a->ctx);
    if (st != AGENT_OK) return st;
  }
  return AGENT_OK;
}

typedef struct {
  const char* node_id;
} outbox_ctx_t;

static agent_status_t encode_platform_caps_req_body(agent_cbor_writer_t* w, void* ctx) {
  const outbox_ctx_t* o = (const outbox_ctx_t*)ctx;
  text_ctx_t nid = {o->node_id, strlen(o->node_id)};
  text_ctx_t want = {"full", strlen("full")};
  const agent_cbor_kv_t pairs[] = {
    {"node_id", strlen("node_id"), enc_text, &nid},
    {"want", strlen("want"), enc_text, &want},
  };
  map_ctx_t m = {pairs, sizeof(pairs) / sizeof(pairs[0])};
  return agent_cbor_write_map_sorted(w, m.pairs, m.n_pairs);
}

static agent_status_t encode_env_platform_caps_req(agent_cbor_writer_t* w, void* ctx) {
  const outbox_ctx_t* o = (const outbox_ctx_t*)ctx;
  agent_umbmp_envelope_cbor_params_t p;
  memset(&p, 0, sizeof(p));
  p.msg_id = "00000000-0000-4000-8000-0000000000aa";
  p.msg_id_len = strlen(p.msg_id);
  p.ts_utc_ms = 1700000000001LL;
  p.type = "PLATFORM_CAPS_REQ";
  p.type_len = strlen(p.type);
  p.from = "platform";
  p.from_len = strlen(p.from);
  char tobuf[128];
  (void)snprintf(tobuf, sizeof(tobuf), "node:%s", o->node_id);
  p.to = tobuf;
  p.to_len = strlen(p.to);
  p.encode_body = encode_platform_caps_req_body;
  p.body_ctx = (void*)o;
  // no auth on this message
  return agent_umbmp_envelope_cbor_v0_4(&p, w);
}

static agent_status_t encode_outbox_row(agent_cbor_writer_t* w, size_t idx, void* ctx) {
  (void)idx;
  const outbox_ctx_t* o = (const outbox_ctx_t*)ctx;
  u64_ctx_t outbox_id = {123};
  u64_ctx_t ts = {1700000000002ULL};

  const agent_cbor_kv_t row_pairs[] = {
    {"outbox_id", strlen("outbox_id"), enc_u64, &outbox_id},
    {"ts_utc_ms", strlen("ts_utc_ms"), enc_u64, &ts},
    {"msg", strlen("msg"), encode_env_platform_caps_req, (void*)o},
  };
  map_ctx_t row_map = {row_pairs, sizeof(row_pairs) / sizeof(row_pairs[0])};
  return agent_cbor_write_map_sorted(w, row_map.pairs, row_map.n_pairs);
}

static void test_outbox_read_decodes_platform_caps_req_envelope(void) {
  // Build a deterministic CBOR outbox response, then decode with agent_core outbox reader.
  const outbox_ctx_t octx = {"fixture_node_task_1"};

  int ok = 1;
  text_ctx_t node_id = {octx.node_id, strlen(octx.node_id)};
  u64_ctx_t cursor_base = {0};
  u64_ctx_t cursor_next = {123};

  array_ctx_t msgs;
  msgs.n = 1;
  msgs.encode_item = encode_outbox_row;
  msgs.ctx = (void*)&octx;

  const agent_cbor_kv_t top_pairs[] = {
    {"ok", strlen("ok"), enc_bool, &ok},
    {"node_id", strlen("node_id"), enc_text, &node_id},
    {"cursor_base", strlen("cursor_base"), enc_u64, &cursor_base},
    {"cursor_next", strlen("cursor_next"), enc_u64, &cursor_next},
    {"messages", strlen("messages"), enc_array, &msgs},
  };
  map_ctx_t top = {top_pairs, sizeof(top_pairs) / sizeof(top_pairs[0])};

  uint8_t buf[2048];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));
  assert(agent_cbor_write_map_sorted(&w, top.pairs, top.n_pairs) == AGENT_OK);

  agent_um_eais_outbox_row_view_t rows[4];
  agent_um_eais_outbox_view_t out;
  assert(agent_um_eais_outbox_read_cbor_v0_1(
           agent_cbor_writer_bytes(&w),
           agent_cbor_writer_len(&w),
           &out,
           rows,
           sizeof(rows) / sizeof(rows[0])) == AGENT_OK);

  assert(out.ok == 1);
  assert(text_eq(out.node_id, octx.node_id));
  assert(out.cursor_base == 0);
  assert(out.cursor_next == 123);
  assert(out.messages_total == 1);
  assert(out.messages_parsed == 1);

  assert(rows[0].has_msg == 1);
  assert(text_eq(rows[0].msg.type, "PLATFORM_CAPS_REQ"));
  assert(text_eq(rows[0].msg.from, "platform"));
  assert(rows[0].msg.has_body == 1);

  agent_um_eais_platform_caps_req_view_t req;
  assert(agent_um_eais_platform_caps_req_body_read_cbor_v0_1(
           rows[0].msg.body_item.ptr,
           rows[0].msg.body_item.len,
           &req) == AGENT_OK);
  assert(text_eq(req.node_id, octx.node_id));
  assert(text_eq(req.want, "full"));
}

void test_um_eais_outbox_read_module(void) {
  test_outbox_read_decodes_platform_caps_req_envelope();
}
