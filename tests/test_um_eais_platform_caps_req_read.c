#include "agent/um_eais_platform_caps_req_read.h"

#include "agent/cbor_det.h"
#include "agent/umbmp_envelope_read.h"
#include "agent/umbmp_auth.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

typedef struct {
  const char* node_id;
  const char* want;
} caps_ctx_t;

static agent_status_t enc_caps_node_id(agent_cbor_writer_t* w, void* ctx) {
  const caps_ctx_t* c = (const caps_ctx_t*)ctx;
  if (!c || !c->node_id) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, c->node_id, strlen(c->node_id));
}

static agent_status_t enc_caps_want(agent_cbor_writer_t* w, void* ctx) {
  const caps_ctx_t* c = (const caps_ctx_t*)ctx;
  if (!c || !c->want) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, c->want, strlen(c->want));
}

static agent_status_t enc_caps_body(agent_cbor_writer_t* w, void* ctx) {
  const caps_ctx_t* c = (const caps_ctx_t*)ctx;
  if (!c || !c->node_id || !c->want) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_cbor_kv_t pairs[] = {
    {"node_id", strlen("node_id"), enc_caps_node_id, ctx},
    {"want", strlen("want"), enc_caps_want, ctx},
  };
  return agent_cbor_write_map_sorted(w, pairs, sizeof(pairs) / sizeof(pairs[0]));
}

static void test_platform_caps_req_body_decode_from_envelope(void) {
  caps_ctx_t c = {"fixture_node_task_1", "full"};

  uint8_t buf[512];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  agent_umbmp_envelope_cbor_params_t p;
  memset(&p, 0, sizeof(p));
  p.msg_id = "00000000-0000-4000-8000-0000000000bb";
  p.msg_id_len = strlen(p.msg_id);
  p.ts_utc_ms = 1700000000000LL;
  p.type = "PLATFORM_CAPS_REQ";
  p.type_len = strlen(p.type);
  p.from = "platform";
  p.from_len = strlen(p.from);
  p.to = "node:fixture_node_task_1";
  p.to_len = strlen(p.to);
  p.encode_body = enc_caps_body;
  p.body_ctx = &c;

  assert(agent_umbmp_envelope_cbor_v0_4(&p, &w) == AGENT_OK);

  agent_umbmp_envelope_view_t env;
  assert(agent_umbmp_envelope_read_cbor_v0_1(agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w), &env) == AGENT_OK);
  assert(text_eq(env.type, "PLATFORM_CAPS_REQ"));
  assert(env.has_body == 1);

  agent_um_eais_platform_caps_req_view_t req;
  assert(agent_um_eais_platform_caps_req_body_read_cbor_v0_1(env.body_item.ptr, env.body_item.len, &req) == AGENT_OK);
  assert(text_eq(req.node_id, "fixture_node_task_1"));
  assert(text_eq(req.want, "full"));
}

void test_um_eais_platform_caps_req_read_module(void) {
  test_platform_caps_req_body_decode_from_envelope();
}
