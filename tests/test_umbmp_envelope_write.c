#include "agent/umbmp_auth.h"

#include "agent/cbor_read.h"
#include "agent/umbmp_envelope_read.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

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

static agent_status_t enc_map(agent_cbor_writer_t* w, void* ctx) {
  const map_ctx_t* m = (const map_ctx_t*)ctx;
  assert(m);
  return agent_cbor_write_map_sorted(w, m->pairs, m->n_pairs);
}

static int text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  if (v.len != n) return 0;
  if (n == 0) return 1;
  return memcmp(v.ptr, s, n) == 0;
}

static void test_umbmp_envelope_full_cbor_includes_auth_sig(void) {
  // Build the node_hello_minimal signing input and then attach a dummy auth.sig.
  text_ctx_t node_id = {"vector_node_1", strlen("vector_node_1")};
  text_ctx_t model = {"esp32sim_stub", strlen("esp32sim_stub")};
  text_ctx_t fw = {"deadbeef", strlen("deadbeef")};
  text_ctx_t caps = {
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    strlen("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
  };

  const agent_cbor_kv_t body_pairs[] = {
    {"node_id", strlen("node_id"), enc_text, &node_id},
    {"model", strlen("model"), enc_text, &model},
    {"fw_git_sha", strlen("fw_git_sha"), enc_text, &fw},
    {"caps_sha256", strlen("caps_sha256"), enc_text, &caps},
  };
  map_ctx_t body_map = {body_pairs, sizeof(body_pairs) / sizeof(body_pairs[0])};

  uint8_t buf[1024];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  agent_umbmp_envelope_cbor_params_t p;
  memset(&p, 0, sizeof(p));
  p.msg_id = "00000000-0000-4000-8000-000000000001";
  p.msg_id_len = strlen(p.msg_id);
  p.ts_utc_ms = 1700000000123LL;
  p.type = "NODE_HELLO";
  p.type_len = strlen(p.type);
  p.from = "node:vector_node_1";
  p.from_len = strlen(p.from);
  p.to = "platform";
  p.to_len = strlen(p.to);
  p.encode_body = enc_map;
  p.body_ctx = &body_map;
  p.auth_alg = "ed25519-cbor";
  p.auth_alg_len = strlen(p.auth_alg);
  p.auth_kid = "vector_node_1";
  p.auth_kid_len = strlen(p.auth_kid);
  p.auth_seq = 1;
  p.auth_has_seq = 1;
  p.auth_sig_b64 = "AAAA";
  p.auth_sig_b64_len = strlen("AAAA");

  assert(agent_umbmp_envelope_cbor_v0_4(&p, &w) == AGENT_OK);

  agent_umbmp_envelope_view_t env;
  assert(agent_umbmp_envelope_read_cbor_v0_1(agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w), &env) == AGENT_OK);
  assert(text_eq(env.type, "NODE_HELLO"));
  assert(env.has_auth == 1);
  assert(text_eq(env.auth.alg, "ed25519-cbor"));
  assert(text_eq(env.auth.kid, "vector_node_1"));
  assert(env.auth.has_seq == 1 && env.auth.seq == 1);
  assert(env.auth.has_sig == 1);
  assert(text_eq(env.auth.sig, "AAAA"));
}

void test_umbmp_envelope_write_module(void) {
  test_umbmp_envelope_full_cbor_includes_auth_sig();
}

