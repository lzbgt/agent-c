#include "agent/cbor_read.h"

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

static void test_cbor_read_decodes_umbmp_node_hello_vector(void) {
  // KAT: must match docs/spec/um-eais/fixtures/umbmp_envelope_auth_vectors_v0.4.json (node_hello_minimal).
  static const char* kCanonCborHex =
    "a762746f68706c6174666f726d6461757468a363616c676c656432353531392d63626f72636b69646d766563746f725f6e6f64655f31637365710164626f6479a4656d6f64656c6d657370333273696d5f73747562676e6f64655f69646d766563746f725f6e6f64655f316a66775f6769745f7368616864656164626565666b636170735f73686132353678477368613235363a616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616161616466726f6d726e6f64653a766563746f725f6e6f64655f3164747970656a4e4f44455f48454c4c4f666d73675f6964782430303030303030302d303030302d343030302d383030302d3030303030303030303030316974735f7574635f6d731b0000018bcfe5687b";

  uint8_t bytes[1024];
  const size_t n = hex_to_bytes(kCanonCborHex, bytes, sizeof(bytes));

  agent_cbor_reader_t r;
  agent_cbor_reader_init(&r, bytes, n);

  size_t top_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &top_pairs) == AGENT_OK);
  assert(top_pairs == 7);

  int seen_to = 0;
  int seen_from = 0;
  int seen_type = 0;
  int seen_msg_id = 0;
  int seen_ts = 0;
  int seen_auth = 0;
  int seen_body = 0;

  for (size_t i = 0; i < top_pairs; i++) {
    agent_cbor_text_view_t k;
    assert(agent_cbor_read_text(&r, &k) == AGENT_OK);

    if (text_eq(k, "to")) {
      agent_cbor_text_view_t v;
      assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
      assert(text_eq(v, "platform"));
      seen_to = 1;
      continue;
    }

    if (text_eq(k, "from")) {
      agent_cbor_text_view_t v;
      assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
      assert(text_eq(v, "node:vector_node_1"));
      seen_from = 1;
      continue;
    }

    if (text_eq(k, "type")) {
      agent_cbor_text_view_t v;
      assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
      assert(text_eq(v, "NODE_HELLO"));
      seen_type = 1;
      continue;
    }

    if (text_eq(k, "msg_id")) {
      agent_cbor_text_view_t v;
      assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
      assert(text_eq(v, "00000000-0000-4000-8000-000000000001"));
      seen_msg_id = 1;
      continue;
    }

    if (text_eq(k, "ts_utc_ms")) {
      uint64_t v = 0;
      assert(agent_cbor_read_uint(&r, &v) == AGENT_OK);
      assert(v == 1700000000123ULL);
      seen_ts = 1;
      continue;
    }

    if (text_eq(k, "auth")) {
      size_t auth_pairs = 0;
      assert(agent_cbor_read_map_start(&r, &auth_pairs) == AGENT_OK);
      assert(auth_pairs == 3);

      int seen_alg = 0;
      int seen_kid = 0;
      int seen_seq = 0;
      for (size_t j = 0; j < auth_pairs; j++) {
        agent_cbor_text_view_t ak;
        assert(agent_cbor_read_text(&r, &ak) == AGENT_OK);
        if (text_eq(ak, "alg")) {
          agent_cbor_text_view_t v;
          assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
          assert(text_eq(v, "ed25519-cbor"));
          seen_alg = 1;
          continue;
        }
        if (text_eq(ak, "kid")) {
          agent_cbor_text_view_t v;
          assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
          assert(text_eq(v, "vector_node_1"));
          seen_kid = 1;
          continue;
        }
        if (text_eq(ak, "seq")) {
          uint64_t v = 0;
          assert(agent_cbor_read_uint(&r, &v) == AGENT_OK);
          assert(v == 1);
          seen_seq = 1;
          continue;
        }
        assert(0 && "unexpected auth key");
      }

      assert(seen_alg && seen_kid && seen_seq);
      seen_auth = 1;
      continue;
    }

    if (text_eq(k, "body")) {
      size_t body_pairs = 0;
      assert(agent_cbor_read_map_start(&r, &body_pairs) == AGENT_OK);
      assert(body_pairs == 4);

      int seen_model = 0;
      int seen_node_id = 0;
      int seen_fw = 0;
      int seen_caps = 0;
      for (size_t j = 0; j < body_pairs; j++) {
        agent_cbor_text_view_t bk;
        assert(agent_cbor_read_text(&r, &bk) == AGENT_OK);
        if (text_eq(bk, "model")) {
          agent_cbor_text_view_t v;
          assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
          assert(text_eq(v, "esp32sim_stub"));
          seen_model = 1;
          continue;
        }
        if (text_eq(bk, "node_id")) {
          agent_cbor_text_view_t v;
          assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
          assert(text_eq(v, "vector_node_1"));
          seen_node_id = 1;
          continue;
        }
        if (text_eq(bk, "fw_git_sha")) {
          agent_cbor_text_view_t v;
          assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
          assert(text_eq(v, "deadbeef"));
          seen_fw = 1;
          continue;
        }
        if (text_eq(bk, "caps_sha256")) {
          agent_cbor_text_view_t v;
          assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
          assert(text_eq(v, "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
          seen_caps = 1;
          continue;
        }
        assert(0 && "unexpected body key");
      }

      assert(seen_model && seen_node_id && seen_fw && seen_caps);
      seen_body = 1;
      continue;
    }

    assert(0 && "unexpected top-level key");
  }

  assert(seen_to && seen_from && seen_type && seen_msg_id && seen_ts && seen_auth && seen_body);
  assert(agent_cbor_reader_offset(&r) == n);
}

void test_cbor_read_module(void) {
  test_cbor_read_decodes_umbmp_node_hello_vector();
}

