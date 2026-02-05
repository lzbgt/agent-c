#include "agent/umbmp_envelope_read.h"

#include "agent/cbor_det.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static size_t hex_to_bytes(const char* hex, uint8_t* out, size_t out_cap) {
  const size_t n = strlen(hex);
  assert((n % 2) == 0);
  const size_t need = n / 2;
  assert(need <= out_cap);
  for (size_t i = 0; i < need; i++) {
    const int hi = hex_val(hex[2 * i + 0]);
    const int lo = hex_val(hex[2 * i + 1]);
    assert(hi >= 0 && lo >= 0);
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return need;
}

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
  const agent_cbor_kv_t* pairs;
  size_t n_pairs;
} map_ctx_t;

static agent_status_t enc_text(agent_cbor_writer_t* w, void* ctx) {
  const text_ctx_t* t = (const text_ctx_t*)ctx;
  return agent_cbor_write_text(w, t->s, t->n);
}

static agent_status_t enc_map(agent_cbor_writer_t* w, void* ctx) {
  const map_ctx_t* m = (const map_ctx_t*)ctx;
  assert(m);
  return agent_cbor_write_map_sorted(w, m->pairs, m->n_pairs);
}

static agent_status_t enc_u64(agent_cbor_writer_t* w, void* ctx) {
  const uint64_t* v = (const uint64_t*)ctx;
  return agent_cbor_write_uint(w, *v);
}

static void test_umbmp_envelope_read_decodes_canonical_node_hello_no_sig(void) {
  // Canonical CBOR bytes for the signing input (auth.sig omitted) from:
  // docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json (node_hello_minimal).
  static const char* kCanonCborHex =
    "a762746f68706c6174666f726d6461757468a363616c676c656432353531392d63626f72636b69646d766563746f725f6e6f64655f31637365710164626f6479a4656d6f64656c6d657370333273696d5f73747562676e6f64655f69646d766563746f725f6e6f64655f316a66775f6769745f7368616864656164626565666b636170735f73686132353678477368613235363a616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616466726f6d726e6f64653a766563746f725f6e6f64655f3164747970656a4e4f44455f48454c4c4f666d73675f6964782430303030303030302d303030302d343030302d383030302d3030303030303030303030316974735f7574635f6d731b0000018bcfe5687b";

  uint8_t bytes[1024];
  const size_t n = hex_to_bytes(kCanonCborHex, bytes, sizeof(bytes));

  agent_umbmp_envelope_view_t env;
  assert(agent_umbmp_envelope_read_cbor_v0_1(bytes, n, &env) == AGENT_OK);

  assert(text_eq(env.to, "platform"));
  assert(text_eq(env.from, "node:vector_node_1"));
  assert(text_eq(env.type, "NODE_HELLO"));
  assert(text_eq(env.msg_id, "00000000-0000-4000-8000-000000000001"));
  assert(env.ts_utc_ms == 1700000000123ULL);

  assert(env.has_auth == 1);
  assert(text_eq(env.auth.alg, "ed25519-cbor"));
  assert(text_eq(env.auth.kid, "vector_node_1"));
  assert(env.auth.has_seq == 1);
  assert(env.auth.seq == 1);
  assert(env.auth.has_sig == 0);  // signing input omits sig

  assert(env.has_trace == 0);
  assert(env.has_body == 1);
  assert(env.body_item.ptr != NULL);
  assert(env.body_item.len > 0);
}

static void test_umbmp_envelope_read_supports_trace_and_sig(void) {
  // Build a small envelope with trace + auth.sig and ensure the decoder extracts them.
  text_ctx_t v_to = {"platform", strlen("platform")};
  text_ctx_t v_from = {"node:demo_node_1", strlen("node:demo_node_1")};
  text_ctx_t v_type = {"TASK_DONE", strlen("TASK_DONE")};
  text_ctx_t v_msg_id = {"0f3a4a50-9999-4000-8000-000000000001", strlen("0f3a4a50-9999-4000-8000-000000000001")};
  uint64_t v_ts = 1700000000456ULL;

  text_ctx_t v_trace_id = {"trace:demo_trace_1", strlen("trace:demo_trace_1")};
  const agent_cbor_kv_t trace_pairs[] = {
    {"trace_id", strlen("trace_id"), enc_text, &v_trace_id},
  };
  map_ctx_t trace_map = {trace_pairs, sizeof(trace_pairs) / sizeof(trace_pairs[0])};

  text_ctx_t v_alg = {"hmac-sha256-cbor", strlen("hmac-sha256-cbor")};
  text_ctx_t v_kid = {"demo_node_1", strlen("demo_node_1")};
  uint64_t v_seq = 7;
  text_ctx_t v_sig = {"AAAA", strlen("AAAA")};
  const agent_cbor_kv_t auth_pairs[] = {
    {"alg", strlen("alg"), enc_text, &v_alg},
    {"kid", strlen("kid"), enc_text, &v_kid},
    {"seq", strlen("seq"), enc_u64, &v_seq},
    {"sig", strlen("sig"), enc_text, &v_sig},
  };
  map_ctx_t auth_map = {auth_pairs, sizeof(auth_pairs) / sizeof(auth_pairs[0])};

  text_ctx_t v_status = {"SUCCEEDED", strlen("SUCCEEDED")};
  const agent_cbor_kv_t body_pairs[] = {
    {"status", strlen("status"), enc_text, &v_status},
  };
  map_ctx_t body_map = {body_pairs, sizeof(body_pairs) / sizeof(body_pairs[0])};

  const agent_cbor_kv_t env_pairs[] = {
    {"msg_id", strlen("msg_id"), enc_text, &v_msg_id},
    {"ts_utc_ms", strlen("ts_utc_ms"), enc_u64, &v_ts},
    {"type", strlen("type"), enc_text, &v_type},
    {"from", strlen("from"), enc_text, &v_from},
    {"to", strlen("to"), enc_text, &v_to},
    {"trace", strlen("trace"), enc_map, &trace_map},
    {"auth", strlen("auth"), enc_map, &auth_map},
    {"body", strlen("body"), enc_map, &body_map},
  };

  uint8_t buf[512];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));
  assert(agent_cbor_write_map_sorted(&w, env_pairs, sizeof(env_pairs) / sizeof(env_pairs[0])) == AGENT_OK);

  agent_umbmp_envelope_view_t env;
  assert(agent_umbmp_envelope_read_cbor_v0_1(agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w), &env) == AGENT_OK);

  assert(text_eq(env.to, "platform"));
  assert(text_eq(env.from, "node:demo_node_1"));
  assert(text_eq(env.type, "TASK_DONE"));
  assert(text_eq(env.msg_id, "0f3a4a50-9999-4000-8000-000000000001"));
  assert(env.ts_utc_ms == 1700000000456ULL);

  assert(env.has_trace == 1);
  assert(env.trace.has_trace_id == 1);
  assert(text_eq(env.trace.trace_id, "trace:demo_trace_1"));

  assert(env.has_auth == 1);
  assert(text_eq(env.auth.alg, "hmac-sha256-cbor"));
  assert(text_eq(env.auth.kid, "demo_node_1"));
  assert(env.auth.has_seq == 1 && env.auth.seq == 7);
  assert(env.auth.has_sig == 1);
  assert(text_eq(env.auth.sig, "AAAA"));

  assert(env.has_body == 1);
}

void test_umbmp_envelope_read_module(void) {
  test_umbmp_envelope_read_decodes_canonical_node_hello_no_sig();
  test_umbmp_envelope_read_supports_trace_and_sig();
}

