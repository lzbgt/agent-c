#include <assert.h>
#include <string.h>

#include "agent/cbor_det.h"
#include "agent/cbor_read.h"
#include "agent/um_eais_node_caps_rsp_write.h"

static void assert_text_eq(agent_cbor_text_view_t v, const char* s) {
  const size_t n = strlen(s);
  assert(v.len == n);
  assert(memcmp(v.ptr, s, n) == 0);
}

static agent_status_t encode_true(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_bool(w, 1);
}

static agent_status_t encode_uint_one(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_uint(w, 1);
}

static void test_caps_rsp_wraps_manifest_and_orders_keys(void) {
  uint8_t man_buf[128];
  agent_cbor_writer_t mw = {0};
  agent_cbor_writer_init(&mw, man_buf, sizeof(man_buf));

  // Manifest: {"a":1,"ok":true} -> deterministic order: "a"(1) then "ok"(2)
  const agent_cbor_kv_t mkv[] = {
    (agent_cbor_kv_t){.key = "a", .key_len = 1, .encode_value = encode_uint_one, .value_ctx = NULL},
    (agent_cbor_kv_t){.key = "ok", .key_len = 2, .encode_value = encode_true, .value_ctx = NULL},
  };
  assert(agent_cbor_write_map_sorted(&mw, mkv, 2) == AGENT_OK);

  uint8_t body_buf[256];
  agent_cbor_writer_t bw = {0};
  agent_cbor_writer_init(&bw, body_buf, sizeof(body_buf));

  const agent_um_eais_node_caps_rsp_body_t body = {
    .node_id = {.ptr = "node1", .len = 5},
    .manifest_cbor = {.ptr = agent_cbor_writer_bytes(&mw), .len = agent_cbor_writer_len(&mw)},
    .enforce_deterministic_keys = 1,
  };

  assert(agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1(&bw, (void*)&body) == AGENT_OK);

  // Decode and assert body-level key order: node_id(7) then manifest(8)
  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&bw), agent_cbor_writer_len(&bw));
  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 2);

  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "node_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "node1");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "manifest");
  size_t m_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &m_pairs) == AGENT_OK);
  assert(m_pairs == 2);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "a");
  uint64_t u64 = 0;
  assert(agent_cbor_read_uint(&r, &u64) == AGENT_OK);
  assert(u64 == 1);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "ok");
  int bval = 0;
  assert(agent_cbor_read_bool(&r, &bval) == AGENT_OK);
  assert(bval == 1);
}

static void test_caps_rsp_rejects_unsorted_manifest_when_enforced(void) {
  uint8_t man_buf[128];
  agent_cbor_writer_t mw = {0};
  agent_cbor_writer_init(&mw, man_buf, sizeof(man_buf));

  // Write map of 2 pairs with UNSORTED keys: "ok"(2) then "a"(1).
  assert(agent_cbor_write_map_start(&mw, 2) == AGENT_OK);
  assert(agent_cbor_write_text(&mw, "ok", 2) == AGENT_OK);
  assert(agent_cbor_write_bool(&mw, 1) == AGENT_OK);
  assert(agent_cbor_write_text(&mw, "a", 1) == AGENT_OK);
  assert(agent_cbor_write_uint(&mw, 1) == AGENT_OK);

  uint8_t body_buf[256];
  agent_cbor_writer_t bw = {0};
  agent_cbor_writer_init(&bw, body_buf, sizeof(body_buf));

  const agent_um_eais_node_caps_rsp_body_t body = {
    .node_id = {.ptr = "node1", .len = 5},
    .manifest_cbor = {.ptr = agent_cbor_writer_bytes(&mw), .len = agent_cbor_writer_len(&mw)},
    .enforce_deterministic_keys = 1,
  };

  assert(agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1(&bw, (void*)&body) != AGENT_OK);
}

static void test_caps_rsp_allows_unsorted_manifest_when_not_enforced(void) {
  uint8_t man_buf[128];
  agent_cbor_writer_t mw = {0};
  agent_cbor_writer_init(&mw, man_buf, sizeof(man_buf));

  assert(agent_cbor_write_map_start(&mw, 2) == AGENT_OK);
  assert(agent_cbor_write_text(&mw, "ok", 2) == AGENT_OK);
  assert(agent_cbor_write_bool(&mw, 1) == AGENT_OK);
  assert(agent_cbor_write_text(&mw, "a", 1) == AGENT_OK);
  assert(agent_cbor_write_uint(&mw, 1) == AGENT_OK);

  uint8_t body_buf[256];
  agent_cbor_writer_t bw = {0};
  agent_cbor_writer_init(&bw, body_buf, sizeof(body_buf));

  const agent_um_eais_node_caps_rsp_body_t body = {
    .node_id = {.ptr = "node1", .len = 5},
    .manifest_cbor = {.ptr = agent_cbor_writer_bytes(&mw), .len = agent_cbor_writer_len(&mw)},
    .enforce_deterministic_keys = 0,
  };

  assert(agent_um_eais_node_caps_rsp_body_encode_cbor_v0_1(&bw, (void*)&body) == AGENT_OK);
}

void test_um_eais_node_caps_rsp_write_module(void) {
  test_caps_rsp_wraps_manifest_and_orders_keys();
  test_caps_rsp_rejects_unsorted_manifest_when_enforced();
  test_caps_rsp_allows_unsorted_manifest_when_not_enforced();
}

