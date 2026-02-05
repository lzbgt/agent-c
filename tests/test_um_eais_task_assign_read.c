#include "agent/um_eais_task_assign_read.h"

#include "agent/cbor_det.h"
#include "agent/umbmp_envelope_read.h"

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

static void test_task_assign_body_decode_from_envelope_cbor(void) {
  // Build a platform -> node TASK_ASSIGN envelope in deterministic CBOR.
  text_ctx_t v_env_msg_id = {"00000000-0000-4000-8000-000000000099", strlen("00000000-0000-4000-8000-000000000099")};
  u64_ctx_t v_env_ts = {1700000000999ULL};
  text_ctx_t v_env_type = {"TASK_ASSIGN", strlen("TASK_ASSIGN")};
  text_ctx_t v_env_from = {"platform", strlen("platform")};
  text_ctx_t v_env_to = {"node:fixture_node_task_1", strlen("node:fixture_node_task_1")};

  // payload: { "prompt": "...", "max_steps": 2 }
  text_ctx_t v_prompt = {"Turn on LED", strlen("Turn on LED")};
  u64_ctx_t v_max_steps = {2};
  const agent_cbor_kv_t payload_pairs[] = {
    {"prompt", strlen("prompt"), enc_text, &v_prompt},
    {"max_steps", strlen("max_steps"), enc_u64, &v_max_steps},
  };
  map_ctx_t payload_map = {payload_pairs, sizeof(payload_pairs) / sizeof(payload_pairs[0])};

  // body: required keys for v0.1 platform implementation.
  text_ctx_t v_task_id = {"task_fixture_1", strlen("task_fixture_1")};
  text_ctx_t v_step_id = {"s1", strlen("s1")};
  text_ctx_t v_idem = {"idem_fixture_1", strlen("idem_fixture_1")};
  text_ctx_t v_mode = {"agent", strlen("agent")};
  u64_ctx_t v_deadline = {1700000001999ULL};
  u64_ctx_t v_attempt = {1};
  const agent_cbor_kv_t body_pairs[] = {
    {"task_id", strlen("task_id"), enc_text, &v_task_id},
    {"step_id", strlen("step_id"), enc_text, &v_step_id},
    {"idempotency_key", strlen("idempotency_key"), enc_text, &v_idem},
    {"mode", strlen("mode"), enc_text, &v_mode},
    {"deadline_utc_ms", strlen("deadline_utc_ms"), enc_u64, &v_deadline},
    {"attempt", strlen("attempt"), enc_u64, &v_attempt},
    {"payload", strlen("payload"), enc_map, &payload_map},
  };
  map_ctx_t body_map = {body_pairs, sizeof(body_pairs) / sizeof(body_pairs[0])};

  const agent_cbor_kv_t env_pairs[] = {
    {"msg_id", strlen("msg_id"), enc_text, &v_env_msg_id},
    {"ts_utc_ms", strlen("ts_utc_ms"), enc_u64, &v_env_ts},
    {"type", strlen("type"), enc_text, &v_env_type},
    {"from", strlen("from"), enc_text, &v_env_from},
    {"to", strlen("to"), enc_text, &v_env_to},
    {"body", strlen("body"), enc_map, &body_map},
  };

  uint8_t buf[1024];
  agent_cbor_writer_t w;
  agent_cbor_writer_init(&w, buf, sizeof(buf));
  assert(agent_cbor_write_map_sorted(&w, env_pairs, sizeof(env_pairs) / sizeof(env_pairs[0])) == AGENT_OK);

  agent_umbmp_envelope_view_t env;
  assert(agent_umbmp_envelope_read_cbor_v0_1(agent_cbor_writer_bytes(&w), agent_cbor_writer_len(&w), &env) == AGENT_OK);
  assert(text_eq(env.type, "TASK_ASSIGN"));
  assert(env.has_body == 1);

  agent_um_eais_task_assign_view_t ta;
  assert(agent_um_eais_task_assign_body_read_cbor_v0_1(env.body_item.ptr, env.body_item.len, &ta) == AGENT_OK);
  assert(text_eq(ta.task_id, "task_fixture_1"));
  assert(text_eq(ta.step_id, "s1"));
  assert(text_eq(ta.idempotency_key, "idem_fixture_1"));
  assert(text_eq(ta.mode, "agent"));
  assert(ta.deadline_utc_ms == 1700000001999ULL);
  assert(ta.has_attempt == 1);
  assert(ta.attempt == 1);
  assert(ta.payload_item.ptr && ta.payload_item.len > 0);
}

void test_um_eais_task_assign_read_module(void) {
  test_task_assign_body_decode_from_envelope_cbor();
}

