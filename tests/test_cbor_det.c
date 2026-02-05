#include "agent/cbor_det.h"

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
  return agent_cbor_write_map_sorted(w, m->pairs, m->n_pairs);
}

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

static void test_cbor_det_matches_platform_vector_node_hello(void) {
  // Expected deterministic CBOR bytes for the "node_hello_minimal" vector from:
  // docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json
  static const char* kExpectedHex =
    "a762746f68706c6174666f726d6461757468a363616c676c656432353531392d63626f72636b69646d766563746f725f6e6f64655f31637365710164626f6479a4656d6f64656c6d657370333273696d5f73747562676e6f64655f69646d766563746f725f6e6f64655f316a66775f6769745f7368616864656164626565666b636170735f73686132353678477368613235363a616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616466726f6d726e6f64653a766563746f725f6e6f64655f3164747970656a4e4f44455f48454c4c4f666d73675f6964782430303030303030302d303030302d343030302d383030302d3030303030303030303030316974735f7574635f6d731b0000018bcfe5687b";

  // Build the same envelope with agent_core deterministic CBOR encoding.
  text_ctx_t v_to = {"platform", strlen("platform")};
  text_ctx_t v_alg = {"ed25519-cbor", strlen("ed25519-cbor")};
  text_ctx_t v_kid = {"vector_node_1", strlen("vector_node_1")};
  u64_ctx_t v_seq = {1};

  const agent_cbor_kv_t auth_pairs[] = {
    {"kid", strlen("kid"), enc_text, &v_kid},
    {"seq", strlen("seq"), enc_u64, &v_seq},
    {"alg", strlen("alg"), enc_text, &v_alg},
  };
  map_ctx_t auth_map = {auth_pairs, sizeof(auth_pairs) / sizeof(auth_pairs[0])};

  text_ctx_t v_model = {"esp32sim_stub", strlen("esp32sim_stub")};
  text_ctx_t v_node_id = {"vector_node_1", strlen("vector_node_1")};
  text_ctx_t v_fw = {"deadbeef", strlen("deadbeef")};
  text_ctx_t v_caps =
    {"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     strlen("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")};

  const agent_cbor_kv_t body_pairs[] = {
    {"node_id", strlen("node_id"), enc_text, &v_node_id},
    {"model", strlen("model"), enc_text, &v_model},
    {"fw_git_sha", strlen("fw_git_sha"), enc_text, &v_fw},
    {"caps_sha256", strlen("caps_sha256"), enc_text, &v_caps},
  };
  map_ctx_t body_map = {body_pairs, sizeof(body_pairs) / sizeof(body_pairs[0])};

  text_ctx_t v_from = {"node:vector_node_1", strlen("node:vector_node_1")};
  text_ctx_t v_type = {"NODE_HELLO", strlen("NODE_HELLO")};
  text_ctx_t v_msg_id = {"00000000-0000-4000-8000-000000000001", strlen("00000000-0000-4000-8000-000000000001")};
  u64_ctx_t v_ts = {1700000000123ULL};

  const agent_cbor_kv_t env_pairs[] = {
    {"ts_utc_ms", strlen("ts_utc_ms"), enc_u64, &v_ts},
    {"type", strlen("type"), enc_text, &v_type},
    {"from", strlen("from"), enc_text, &v_from},
    {"to", strlen("to"), enc_text, &v_to},
    {"msg_id", strlen("msg_id"), enc_text, &v_msg_id},
    {"body", strlen("body"), enc_map, &body_map},
    {"auth", strlen("auth"), enc_map, &auth_map},
  };

  uint8_t buf[1024];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));
  assert(agent_cbor_write_map_sorted(&w, env_pairs, sizeof(env_pairs) / sizeof(env_pairs[0])) == AGENT_OK);

  uint8_t expected[1024];
  const size_t expected_len = hex_to_bytes(kExpectedHex, expected, sizeof(expected));

  assert(agent_cbor_writer_len(&w) == expected_len);
  assert(memcmp(agent_cbor_writer_bytes(&w), expected, expected_len) == 0);
}

static void test_cbor_det_rejects_duplicate_keys(void) {
  text_ctx_t v = {"x", 1};
  const agent_cbor_kv_t pairs[] = {
    {"a", 1, enc_text, &v},
    {"a", 1, enc_text, &v},
  };
  uint8_t buf[16];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));
  assert(agent_cbor_write_map_sorted(&w, pairs, 2) == AGENT_ERR_INVALID_ARGUMENT);
}

void test_cbor_det_module(void) {
  test_cbor_det_matches_platform_vector_node_hello();
  test_cbor_det_rejects_duplicate_keys();
}

