#include "agent/um_eais_node_write.h"

#include "agent/edge_interop.h"

static agent_status_t agent_encode_text_view(agent_cbor_writer_t* w, void* ctx) {
  const agent_cbor_text_view_t* v = (const agent_cbor_text_view_t*)ctx;
  if (!w || !v || !v->ptr) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_text(w, v->ptr, v->len);
}

static agent_status_t agent_encode_f64(agent_cbor_writer_t* w, void* ctx) {
  const double* v = (const double*)ctx;
  if (!w || !v) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_f64(w, *v);
}

static agent_status_t agent_encode_i64(agent_cbor_writer_t* w, void* ctx) {
  const int64_t* v = (const int64_t*)ctx;
  if (!w || !v) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_int(w, *v);
}

static agent_status_t require_id(const agent_cbor_text_view_t* v) {
  if (!v) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(v->ptr, v->len)) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static agent_status_t require_text_nonempty(const agent_cbor_text_view_t* v) {
  if (!v || !v->ptr || v->len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

static agent_status_t require_sha256_token(const agent_cbor_text_view_t* v) {
  if (!v) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_sha256_token_is_safe(v->ptr, v->len)) return AGENT_ERR_INVALID_ARGUMENT;
  return AGENT_OK;
}

agent_status_t agent_um_eais_node_hello_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_node_hello_body_t* b = (const agent_um_eais_node_hello_body_t*)ctx;

  if (require_id(&b->node_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (b->has_model && require_text_nonempty(&b->model) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (b->has_fw_git_sha && require_text_nonempty(&b->fw_git_sha) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (b->has_caps_sha256 && require_sha256_token(&b->caps_sha256) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_kv_t pairs[4];
  size_t n = 0;

  pairs[n++] = (agent_cbor_kv_t){
    .key = "node_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->node_id,
  };
  if (b->has_model) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "model",
      .key_len = 5,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->model,
    };
  }
  if (b->has_fw_git_sha) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "fw_git_sha",
      .key_len = 10,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->fw_git_sha,
    };
  }
  if (b->has_caps_sha256) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "caps_sha256",
      .key_len = 11,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->caps_sha256,
    };
  }

  return agent_cbor_write_map_sorted(w, pairs, n);
}

typedef struct encode_health_ctx {
  agent_um_eais_encode_fn fn;
  void* ctx;
} encode_health_ctx_t;

static agent_status_t encode_health_value(agent_cbor_writer_t* w, void* ctx) {
  const encode_health_ctx_t* h = (const encode_health_ctx_t*)ctx;
  if (!h || !h->fn) return AGENT_ERR_INVALID_ARGUMENT;
  return h->fn(w, h->ctx);
}

agent_status_t agent_um_eais_node_heartbeat_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_node_heartbeat_body_t* b = (const agent_um_eais_node_heartbeat_body_t*)ctx;

  if (require_id(&b->node_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (b->has_caps_sha256 && require_sha256_token(&b->caps_sha256) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_kv_t pairs[5];
  size_t n = 0;
  encode_health_ctx_t hctx = {.fn = b->encode_health, .ctx = b->health_ctx};

  pairs[n++] = (agent_cbor_kv_t){
    .key = "node_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->node_id,
  };
  if (b->encode_health) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "health",
      .key_len = 6,
      .encode_value = encode_health_value,
      .value_ctx = (void*)&hctx,
    };
  }
  if (b->has_caps_sha256) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "caps_sha256",
      .key_len = 11,
      .encode_value = agent_encode_text_view,
      .value_ctx = (void*)&b->caps_sha256,
    };
  }
  if (b->has_rssi) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "rssi",
      .key_len = 4,
      .encode_value = agent_encode_f64,
      .value_ctx = (void*)&b->rssi,
    };
  }
  if (b->has_battery_pct) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "battery_pct",
      .key_len = 11,
      .encode_value = agent_encode_f64,
      .value_ctx = (void*)&b->battery_pct,
    };
  }

  return agent_cbor_write_map_sorted(w, pairs, n);
}

typedef struct encode_data_ctx {
  agent_um_eais_encode_fn fn;
  void* ctx;
} encode_data_ctx_t;

static agent_status_t encode_data_value(agent_cbor_writer_t* w, void* ctx) {
  const encode_data_ctx_t* d = (const encode_data_ctx_t*)ctx;
  if (!d || !d->fn) return AGENT_ERR_INVALID_ARGUMENT;
  return d->fn(w, d->ctx);
}

agent_status_t agent_um_eais_sensor_event_body_encode_cbor_v0_1(agent_cbor_writer_t* w, void* ctx) {
  if (!w || !ctx) return AGENT_ERR_INVALID_ARGUMENT;
  const agent_um_eais_sensor_event_body_t* b = (const agent_um_eais_sensor_event_body_t*)ctx;

  if (require_id(&b->node_id) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (require_text_nonempty(&b->event_type) != AGENT_OK) return AGENT_ERR_INVALID_ARGUMENT;
  if (!b->encode_data) return AGENT_ERR_INVALID_ARGUMENT;

  agent_cbor_kv_t pairs[5];
  size_t n = 0;

  pairs[n++] = (agent_cbor_kv_t){
    .key = "node_id",
    .key_len = 7,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->node_id,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "event_type",
    .key_len = 10,
    .encode_value = agent_encode_text_view,
    .value_ctx = (void*)&b->event_type,
  };
  pairs[n++] = (agent_cbor_kv_t){
    .key = "ts_utc_ms",
    .key_len = 9,
    .encode_value = agent_encode_i64,
    .value_ctx = (void*)&b->ts_utc_ms,
  };

  if (b->has_confidence) {
    pairs[n++] = (agent_cbor_kv_t){
      .key = "confidence",
      .key_len = 10,
      .encode_value = agent_encode_f64,
      .value_ctx = (void*)&b->confidence,
    };
  }

  encode_data_ctx_t dctx = {.fn = b->encode_data, .ctx = b->data_ctx};
  pairs[n++] = (agent_cbor_kv_t){
    .key = "data",
    .key_len = 4,
    .encode_value = encode_data_value,
    .value_ctx = (void*)&dctx,
  };

  return agent_cbor_write_map_sorted(w, pairs, n);
}
