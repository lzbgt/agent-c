#include <assert.h>
#include <string.h>

#include "agent/cbor_read.h"
#include "agent/um_eais_node_write.h"

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

static agent_status_t encode_health_minimal(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  // {"ok": true}
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "ok",
      .key_len = 2,
      .encode_value = encode_true,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static agent_status_t encode_data_minimal(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  // {"x": 1}
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "x",
      .key_len = 1,
      .encode_value = encode_uint_one,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 1);
}

static void test_node_hello_order_and_values(void) {
  uint8_t buf[256];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_node_hello_body_t b = {
    .node_id = {.ptr = "n1", .len = 2},
    .model = {.ptr = "esp32", .len = 5},
    .has_model = 1,
    .fw_git_sha = {.ptr = "deadbeef", .len = 8},
    .has_fw_git_sha = 1,
    .caps_sha256 = {.ptr = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", .len = 71},
    .has_caps_sha256 = 1,
  };

  assert(agent_um_eais_node_hello_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));
  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 4);

  // Order: model(5), node_id(7), fw_git_sha(10), caps_sha256(11)
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t v = {0};

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "model");
  assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
  assert_text_eq(v, "esp32");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "node_id");
  assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
  assert_text_eq(v, "n1");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "fw_git_sha");
  assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
  assert_text_eq(v, "deadbeef");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "caps_sha256");
  assert(agent_cbor_read_text(&r, &v) == AGENT_OK);
  assert(v.len == b.caps_sha256.len);
}

static void test_node_heartbeat_order_and_values(void) {
  uint8_t buf[256];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_node_heartbeat_body_t b = {
    .node_id = {.ptr = "n2", .len = 2},
    .caps_sha256 = {.ptr = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", .len = 64},
    .has_caps_sha256 = 1,
    .battery_pct = 88.0,
    .has_battery_pct = 1,
    .rssi = -42.0,
    .has_rssi = 1,
    .encode_health = encode_health_minimal,
    .health_ctx = NULL,
  };

  assert(agent_um_eais_node_heartbeat_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));
  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 5);

  // Order: rssi(4), health(6), node_id(7), battery_pct(11), caps_sha256(11) with lex tie -> battery_pct then caps_sha256
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};
  double dv = 0.0;

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "rssi");
  assert(agent_cbor_read_f64(&r, &dv) == AGENT_OK);
  assert(dv < -41.9 && dv > -42.1);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "health");
  size_t h_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &h_pairs) == AGENT_OK);
  assert(h_pairs == 1);
  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "ok");
  int bval = 0;
  assert(agent_cbor_read_bool(&r, &bval) == AGENT_OK);
  assert(bval == 1);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "node_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "n2");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "battery_pct");
  assert(agent_cbor_read_f64(&r, &dv) == AGENT_OK);
  assert(dv > 87.9 && dv < 88.1);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "caps_sha256");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert(tv.len == 64);
}

static void test_sensor_event_order_and_values(void) {
  uint8_t buf[256];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_sensor_event_body_t b = {
    .node_id = {.ptr = "n3", .len = 2},
    .event_type = {.ptr = "dirt", .len = 4},
    .ts_utc_ms = 1700000000123LL,
    .confidence = 0.9,
    .has_confidence = 1,
    .encode_data = encode_data_minimal,
    .data_ctx = NULL,
  };

  assert(agent_um_eais_sensor_event_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));
  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 5);

  // Order: data(4), node_id(7), ts_utc_ms(9), confidence(10), event_type(10) with lex tie -> confidence then event_type
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};
  int64_t i64 = 0;
  double dv = 0.0;

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "data");
  size_t d_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &d_pairs) == AGENT_OK);
  assert(d_pairs == 1);
  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "x");
  uint64_t u64 = 0;
  assert(agent_cbor_read_uint(&r, &u64) == AGENT_OK);
  assert(u64 == 1);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "node_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "n3");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "ts_utc_ms");
  assert(agent_cbor_read_int(&r, &i64) == AGENT_OK);
  assert(i64 == b.ts_utc_ms);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "confidence");
  assert(agent_cbor_read_f64(&r, &dv) == AGENT_OK);
  assert(dv > 0.89 && dv < 0.91);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "event_type");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "dirt");
}

void test_um_eais_node_write_module(void) {
  test_node_hello_order_and_values();
  test_node_heartbeat_order_and_values();
  test_sensor_event_order_and_values();
}
