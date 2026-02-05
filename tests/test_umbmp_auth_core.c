#include "agent/umbmp_auth.h"

#include "agent/base64.h"
#include "agent/cbor_det.h"
#include "agent/ed25519.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

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

static void test_umbmp_envelope_auth_vectors_v0_4(void) {
  // KAT: must match docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json (node_hello_minimal).
  static const char* kCanonCborHex =
    "a762746f68706c6174666f726d6461757468a363616c676c656432353531392d63626f72636b69646d766563746f725f6e6f64655f31637365710164626f6479a4656d6f64656c6d657370333273696d5f73747562676e6f64655f69646d766563746f725f6e6f64655f316a66775f6769745f7368616864656164626565666b636170735f73686132353678477368613235363a616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616466726f6d726e6f64653a766563746f725f6e6f64655f3164747970656a4e4f44455f48454c4c4f666d73675f6964782430303030303030302d303030302d343030302d383030302d3030303030303030303030316974735f7574635f6d731b0000018bcfe5687b";

  static const char* kSigHmacB64 = "VgWYaEBRyes5rtnPVSPNHKkrRm0R2D6f4HAxrKk+7SE=";
  static const char* kSigEd25519B64 = "5ike4uD5oekOB5+ZSZPJkXFqpcH09K18v9eLSXx20/jAmxioJ7boeTCZLvxsX5/fAeXzRSeN13CgJ1em4PcVAw==";

  // Body encoder: deterministic map, same as the vector.
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
  p.auth_kid = "vector_node_1";
  p.auth_kid_len = strlen(p.auth_kid);
  p.auth_seq = 1;
  p.auth_has_seq = 1;

  // Canonical CBOR bytes in the vector are computed with auth.alg="ed25519-cbor".
  p.auth_alg = "ed25519-cbor";
  p.auth_alg_len = strlen(p.auth_alg);
  assert(agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &w) == AGENT_OK);

  uint8_t expected[1024];
  const size_t expected_len = hex_to_bytes(kCanonCborHex, expected, sizeof(expected));
  assert(agent_cbor_writer_len(&w) == expected_len);
  assert(memcmp(agent_cbor_writer_bytes(&w), expected, expected_len) == 0);

  // HMAC over canonical CBOR bytes: NOTE the signing input includes auth.alg, so for HMAC we must
  // re-encode with auth.alg="hmac-sha256-cbor".
  uint8_t buf_hmac[1024];
  agent_cbor_writer_t w_hmac;
  agent_cbor_writer_init(&w_hmac, buf_hmac, sizeof(buf_hmac));
  p.auth_alg = "hmac-sha256-cbor";
  p.auth_alg_len = strlen(p.auth_alg);
  assert(agent_umbmp_envelope_no_sig_cbor_v0_4(&p, &w_hmac) == AGENT_OK);

  char mac_b64[128];
  size_t mac_len = 0;
  assert(agent_umbmp_auth_hmac_sha256_cbor_sig_b64(
           "test_secret_node_123", strlen("test_secret_node_123"),
           agent_cbor_writer_bytes(&w_hmac), agent_cbor_writer_len(&w_hmac),
           mac_b64, sizeof(mac_b64),
           &mac_len) == AGENT_OK);
  assert(strcmp(mac_b64, kSigHmacB64) == 0);

  // Ed25519 signature over canonical CBOR bytes (vector uses RFC8032 test seed #1).
  const uint8_t sk_seed[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
    0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
    0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
  };
  uint8_t pk[32];
  agent_ed25519_publickey(sk_seed, pk);

  char sig_b64[128];
  size_t sig_len = 0;
  assert(agent_umbmp_auth_ed25519_cbor_sig_b64(
           sk_seed, pk,
           agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w),
           sig_b64, sizeof(sig_b64),
           &sig_len) == AGENT_OK);
  assert(strcmp(sig_b64, kSigEd25519B64) == 0);
}

void test_umbmp_auth_core_module(void) {
  test_umbmp_envelope_auth_vectors_v0_4();
}
