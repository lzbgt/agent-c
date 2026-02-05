#include <assert.h>
#include <string.h>

#include "agent/cbor_read.h"
#include "agent/um_eais_task_lifecycle_write.h"

static void assert_text_eq(agent_cbor_text_view_t v, const char* s) {
  size_t n = strlen(s);
  assert(v.len == n);
  assert(memcmp(v.ptr, s, n) == 0);
}

static agent_status_t encode_true(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_bool(w, 1);
}

static agent_status_t encode_text_hi(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  return agent_cbor_write_text(w, "hi", 2);
}

static agent_status_t encode_result_minimal(agent_cbor_writer_t* w, void* ctx) {
  (void)ctx;
  // {"ok": true, "assistant_text": "hi"}
  const agent_cbor_kv_t kv[] = {
    (agent_cbor_kv_t){
      .key = "ok",
      .key_len = 2,
      .encode_value = encode_true,
      .value_ctx = NULL,
    },
    (agent_cbor_kv_t){
      .key = "assistant_text",
      .key_len = 14,
      .encode_value = encode_text_hi,
      .value_ctx = NULL,
    },
  };
  return agent_cbor_write_map_sorted(w, kv, 2);
}

static void test_task_ack_key_order_and_types(void) {
  uint8_t buf[256];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_task_ack_body_t b = {
    .task_id = {.ptr = "t1", .len = 2},
    .step_id = {.ptr = "s1", .len = 2},
    .idempotency_key = {.ptr = "k1", .len = 2},
    .accepted = 1,
    .reason = {.ptr = "ok", .len = 2},
    .has_reason = 1,
  };
  assert(agent_um_eais_task_ack_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));

  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 5);

  // Deterministic order: reason(6), step_id(7), task_id(7), accepted(8), idempotency_key(15)
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};
  int bval = 0;

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "reason");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "ok");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "step_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "s1");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "task_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "t1");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "accepted");
  assert(agent_cbor_read_bool(&r, &bval) == AGENT_OK);
  assert(bval == 1);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "idempotency_key");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "k1");
}

static void test_task_event_key_order_and_types(void) {
  uint8_t buf[256];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_task_event_body_t b = {
    .task_id = {.ptr = "t2", .len = 2},
    .step_id = {.ptr = "s2", .len = 2},
    .idempotency_key = {.ptr = "k2", .len = 2},
    .state = {.ptr = "running", .len = 7},
    .progress = 0.5,
    .has_progress = 1,
    .error = {.ptr = "warn", .len = 4},
    .has_error = 1,
  };
  assert(agent_um_eais_task_event_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));

  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 6);

  // Deterministic order: error(5), state(5), step_id(7), task_id(7), progress(8), idempotency_key(15)
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};
  double dv = 0.0;

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "error");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "warn");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "state");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "running");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "step_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "s2");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "task_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "t2");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "progress");
  assert(agent_cbor_read_f64(&r, &dv) == AGENT_OK);
  assert(dv > 0.49 && dv < 0.51);

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "idempotency_key");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "k2");
}

static void test_task_failed_key_order_and_types(void) {
  uint8_t buf[256];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_task_failed_body_t b = {
    .task_id = {.ptr = "t3", .len = 2},
    .step_id = {.ptr = "s3", .len = 2},
    .idempotency_key = {.ptr = "k3", .len = 2},
    .error = {.ptr = "boom", .len = 4},
  };
  assert(agent_um_eais_task_failed_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));

  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 4);

  // Deterministic order: error(5), step_id(7), task_id(7), idempotency_key(15)
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "error");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "boom");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "step_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "s3");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "task_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "t3");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "idempotency_key");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "k3");
}

static void test_task_done_key_order_and_types(void) {
  uint8_t buf[512];
  agent_cbor_writer_t w = {0};
  agent_cbor_writer_init(&w, buf, sizeof(buf));

  const agent_um_eais_task_done_body_t b = {
    .task_id = {.ptr = "t4", .len = 2},
    .step_id = {.ptr = "s4", .len = 2},
    .idempotency_key = {.ptr = "k4", .len = 2},
    .encode_result = encode_result_minimal,
    .result_ctx = NULL,
  };
  assert(agent_um_eais_task_done_body_encode_cbor_v0_1(&w, (void*)&b) == AGENT_OK);

  agent_cbor_reader_t r = {0};
  agent_cbor_reader_init(&r, agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w));

  size_t n_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &n_pairs) == AGENT_OK);
  assert(n_pairs == 4);

  // Order: result(6), step_id(7), task_id(7), idempotency_key(15)
  agent_cbor_text_view_t k = {0};
  agent_cbor_text_view_t tv = {0};

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "result");
  size_t r_pairs = 0;
  assert(agent_cbor_read_map_start(&r, &r_pairs) == AGENT_OK);
  assert(r_pairs == 2);
  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "ok");
  int bval = 0;
  assert(agent_cbor_read_bool(&r, &bval) == AGENT_OK);
  assert(bval == 1);
  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "assistant_text");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "hi");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "step_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "s4");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "task_id");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "t4");

  assert(agent_cbor_read_text(&r, &k) == AGENT_OK);
  assert_text_eq(k, "idempotency_key");
  assert(agent_cbor_read_text(&r, &tv) == AGENT_OK);
  assert_text_eq(tv, "k4");
}

void test_um_eais_task_lifecycle_write_module(void) {
  test_task_ack_key_order_and_types();
  test_task_event_key_order_and_types();
  test_task_failed_key_order_and_types();
  test_task_done_key_order_and_types();
}
