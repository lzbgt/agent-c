#include "agent/umbmp_auth.h"

#include "agent/base64.h"
#include "agent/ed25519.h"
#include "agent/edge_interop.h"
#include "agent/hmac_sha256.h"

#include <string.h>

typedef struct {
  const char* s;
  size_t n;
} text_ctx_t;

typedef struct {
  uint64_t v;
} u64_ctx_t;

typedef struct {
  agent_umbmp_encode_fn fn;
  void* ctx;
} fn_ctx_t;

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

static agent_status_t enc_fn(agent_cbor_writer_t* w, void* ctx) {
  const fn_ctx_t* f = (const fn_ctx_t*)ctx;
  if (!f || !f->fn) return AGENT_ERR_INVALID_ARGUMENT;
  return f->fn(w, f->ctx);
}

static agent_status_t enc_map_sorted(agent_cbor_writer_t* w, void* ctx) {
  const map_ctx_t* m = (const map_ctx_t*)ctx;
  if (!m) return AGENT_ERR_INVALID_ARGUMENT;
  return agent_cbor_write_map_sorted(w, m->pairs, m->n_pairs);
}

static agent_status_t encode_env_impl(
  const agent_umbmp_envelope_cbor_params_t* p,
  agent_cbor_writer_t* w,
  int include_sig
) {
  if (!p || !w) return AGENT_ERR_INVALID_ARGUMENT;
  if (!p->msg_id || p->msg_id_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!p->type || p->type_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!p->from || p->from_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!p->to || p->to_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!p->encode_body) return AGENT_ERR_INVALID_ARGUMENT;

  // Best-effort: enforce the shared id-safe character set for the main envelope ids so
  // nodes don't sign bytes the platform will reject anyway.
  if (!agent_umbmp_id_is_safe(p->msg_id, p->msg_id_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (p->type_len > AGENT_UM_BMP_MAX_ID_LEN) return AGENT_ERR_INVALID_ARGUMENT;
  if (p->from_len > AGENT_UM_BMP_MAX_ID_LEN) return AGENT_ERR_INVALID_ARGUMENT;
  if (p->to_len > AGENT_UM_BMP_MAX_ID_LEN) return AGENT_ERR_INVALID_ARGUMENT;

  const int want_auth = (p->auth_alg && p->auth_alg_len && p->auth_kid && p->auth_kid_len) ? 1 : 0;
  if (want_auth) {
    if (!agent_umbmp_id_is_safe(p->auth_kid, p->auth_kid_len)) return AGENT_ERR_INVALID_ARGUMENT;
    if (p->auth_alg_len > 64) return AGENT_ERR_INVALID_ARGUMENT;
  }
  if (include_sig && p->auth_sig_b64) {
    // sig without auth is meaningless.
    if (!want_auth) return AGENT_ERR_INVALID_ARGUMENT;
    if (p->auth_sig_b64_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  }

  // Build key/value list for the envelope.
  agent_cbor_kv_t env_pairs[8];
  size_t n = 0;

  text_ctx_t msg_id = {p->msg_id, p->msg_id_len};
  text_ctx_t type = {p->type, p->type_len};
  text_ctx_t from = {p->from, p->from_len};
  text_ctx_t to = {p->to, p->to_len};
  u64_ctx_t ts = {(p->ts_utc_ms < 0) ? 0 : (uint64_t)p->ts_utc_ms};
  fn_ctx_t body = {p->encode_body, p->body_ctx};

  env_pairs[n++] = (agent_cbor_kv_t){"msg_id", strlen("msg_id"), enc_text, &msg_id};
  env_pairs[n++] = (agent_cbor_kv_t){"ts_utc_ms", strlen("ts_utc_ms"), enc_u64, &ts};
  env_pairs[n++] = (agent_cbor_kv_t){"type", strlen("type"), enc_text, &type};
  env_pairs[n++] = (agent_cbor_kv_t){"from", strlen("from"), enc_text, &from};
  env_pairs[n++] = (agent_cbor_kv_t){"to", strlen("to"), enc_text, &to};
  env_pairs[n++] = (agent_cbor_kv_t){"body", strlen("body"), enc_fn, &body};

  fn_ctx_t trace = {p->encode_trace, p->trace_ctx};
  if (p->encode_trace) {
    env_pairs[n++] = (agent_cbor_kv_t){"trace", strlen("trace"), enc_fn, &trace};
  }

  // Optional auth metadata (alg/kid/seq) with optional sig.
  agent_cbor_kv_t auth_pairs[4];
  text_ctx_t auth_alg = {p->auth_alg, p->auth_alg_len};
  text_ctx_t auth_kid = {p->auth_kid, p->auth_kid_len};
  u64_ctx_t auth_seq = {p->auth_seq};
  text_ctx_t auth_sig = {p->auth_sig_b64, p->auth_sig_b64_len};

  map_ctx_t auth_map = {NULL, 0};
  if (want_auth) {
    size_t an = 0;
    auth_pairs[an++] = (agent_cbor_kv_t){"alg", strlen("alg"), enc_text, &auth_alg};
    auth_pairs[an++] = (agent_cbor_kv_t){"kid", strlen("kid"), enc_text, &auth_kid};
    if (p->auth_has_seq) auth_pairs[an++] = (agent_cbor_kv_t){"seq", strlen("seq"), enc_u64, &auth_seq};
    if (include_sig && p->auth_sig_b64) auth_pairs[an++] = (agent_cbor_kv_t){"sig", strlen("sig"), enc_text, &auth_sig};
    auth_map = (map_ctx_t){auth_pairs, an};
    env_pairs[n++] = (agent_cbor_kv_t){"auth", strlen("auth"), enc_map_sorted, &auth_map};
  }

  return agent_cbor_write_map_sorted(w, env_pairs, n);
}

agent_status_t agent_umbmp_envelope_no_sig_cbor_v0_4(
  const agent_umbmp_envelope_cbor_params_t* p,
  agent_cbor_writer_t* w
) {
  return encode_env_impl(p, w, 0);
}

agent_status_t agent_umbmp_envelope_cbor_v0_4(
  const agent_umbmp_envelope_cbor_params_t* p,
  agent_cbor_writer_t* w
) {
  return encode_env_impl(p, w, 1);
}

agent_status_t agent_umbmp_auth_hmac_sha256_cbor_sig_b64(
  const void* secret,
  size_t secret_len,
  const uint8_t* signing_input,
  size_t signing_input_len,
  char* out_b64,
  size_t out_cap,
  size_t* out_len
) {
  if (!secret || secret_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!signing_input || signing_input_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!out_b64 || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;

  uint8_t mac[32];
  agent_hmac_sha256(secret, secret_len, signing_input, signing_input_len, mac);
  return agent_base64_encode(mac, sizeof(mac), out_b64, out_cap, out_len);
}

agent_status_t agent_umbmp_auth_ed25519_cbor_sig_b64(
  const uint8_t sk_seed32[32],
  const uint8_t pk32[32],
  const uint8_t* signing_input,
  size_t signing_input_len,
  char* out_b64,
  size_t out_cap,
  size_t* out_len
) {
  if (!sk_seed32 || !pk32) return AGENT_ERR_INVALID_ARGUMENT;
  if (!signing_input || signing_input_len == 0) return AGENT_ERR_INVALID_ARGUMENT;
  if (!out_b64 || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;

  uint8_t sig[64];
  agent_ed25519_sign(signing_input, signing_input_len, sk_seed32, pk32, sig);
  return agent_base64_encode(sig, sizeof(sig), out_b64, out_cap, out_len);
}
