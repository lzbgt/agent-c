#include "agent/um_eais_task_lifecycle_write.h"

#include "agent/edge_interop.h"

static agent_status_t agent_require_id(const agent_cbor_text_view_t* v) {
  if (!v) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(v->ptr, v->len)) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static agent_status_t agent_require_text_nonempty(const agent_cbor_text_view_t* v) {
  if (!v || !v->ptr || v->len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static agent_status_t agent_encode_text_view(agent_cbor_writer_t* w, void* ctx) {
  const agent_cbor_text_view_t* v = (const agent_cbor_text_view_t*)ctx;
  if (!v || !v->ptr) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, v->ptr, v->len);
}

static agent_status_t agent_encode_bool(agent_cbor_writer_t* w, void* ctx) {
  const int* v = (const int*)ctx;
  if (!v) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_bool(w, *v ? 1 : 0);
}

static agent_status_t agent_encode_f64(agent_cbor_writer_t* w, void* ctx) {
  const double* v = (const double*)ctx;
  if (!v) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_f64(w, *v);
}

typedef struct agent_encode_cb_ctx {
  agent_cbor_encode_fn fn;
  void* ctx;
} agent_encode_cb_ctx_t;

static agent_status_t agent_encode_cb(agent_cbor_writer_t* w, void* ctx) {
  const agent_encode_cb_ctx_t* c = (const agent_encode_cb_ctx_t*)ctx;
  if (!c || !c->fn) return AGENT_ERR_INVALID_ARGUMENT;
  return c->fn(w, c->ctx);
}

agent_status_t agent_um_eais_task_ack_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_task_ack_body_t* b = (const agent_um_eais_task_ack_body_t*)ctx;
  if (agent_require_id(&b->task_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->step_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->idempotency_key) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (b->has_reason && agent_require_text_nonempty(&b->reason) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_kv_t pairs[5];
  size_t n = 0;

  pairs[n++] = (agent_cbor_kv_t){
    .key = "task_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->task_id,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "step_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->step_id,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "idempotency_key",
    .key_len = 15,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->idempotency_key,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "accepted",
    .key_len = 8,
    .encode_value = agent_encode_bool,
    .value_ctx = (void*)&b->accepted,
  };
  if (b->has_reason) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "reason",
      .key_len = 6,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->reason,
    };
  }

  return agent_cbor_write_map_sorted(w, pairs, n);
}

agent_status_t agent_um_eais_task_event_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_task_event_body_t* b = (const agent_um_eais_task_event_body_t*)ctx;
  if (agent_require_id(&b->task_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->step_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->idempotency_key) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_text_nonempty(&b->state) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (b->has_error && agent_require_text_nonempty(&b->error) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_kv_t pairs[6];
  size_t n = 0;

  pairs[n++] = (agent_cbor_kv_t){
    .key = "task_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->task_id,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "step_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->step_id,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "idempotency_key",
    .key_len = 15,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->idempotency_key,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "state",
    .key_len = 5,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->state,
  };

  if (b->has_progress) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "progress",
      .key_len = 8,
      .encode_value = agent_encode_f64,
      .value_ctx = (void*)&b->progress,
    };
  }
  if (b->has_error) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "error",
      .key_len = 5,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->error,
    };
  }

  return agent_cbor_write_map_sorted(w, pairs, n);
}

agent_status_t agent_um_eais_task_failed_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_task_failed_body_t* b = (const agent_um_eais_task_failed_body_t*)ctx;
  if (agent_require_id(&b->task_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->step_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->idempotency_key) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_text_nonempty(&b->error) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;

  const agent_cbor_kv_t pairs[] = {
    (agent_cbor_kv_t){
      .key = "task_id",
      .key_len = 7,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->task_id,
    },
    (agent_cbor_kv_t){
      .key = "step_id",
      .key_len = 7,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->step_id,
    },
    (agent_cbor_kv_t){
      .key = "idempotency_key",
      .key_len = 15,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->idempotency_key,
    },
    (agent_cbor_kv_t){
      .key = "error",
      .key_len = 5,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->error,
    },
  };

  return agent_cbor_write_map_sorted(w, pairs, sizeof(pairs) / sizeof(pairs[0]));
}

agent_status_t agent_um_eais_task_done_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_task_done_body_t* b = (const agent_um_eais_task_done_body_t*)ctx;
  if (agent_require_id(&b->task_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->step_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (agent_require_id(&b->idempotency_key) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (!b->encode_result) return AGENT_ERR_INVALID_ARGUMENT;

  agent_encode_cb_ctx_t rctx = {.fn = b->encode_result, .ctx = b->result_ctx};
  const agent_cbor_kv_t pairs[] = {
    (agent_cbor_kv_t){
      .key = "task_id",
      .key_len = 7,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->task_id,
    },
    (agent_cbor_kv_t){
      .key = "step_id",
      .key_len = 7,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->step_id,
    },
    (agent_cbor_kv_t){
      .key = "idempotency_key",
      .key_len = 15,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->idempotency_key,
    },
    (agent_cbor_kv_t){
      .key = "result",
      .key_len = 6,
      .encode_value = agent_encode_cb,
      .value_ctx = (void*)&rctx,
    },
  };

  return agent_cbor_write_map_sorted(w, pairs, sizeof(pairs) / sizeof(pairs[0]));
}
