#include "agent/edge_interop.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int umbmp_char_ok(char c) {
  const int ok =
    (c >= 'a' && c <= 'z') ||
    (c >= 'A' && c <= 'Z') ||
    (c >= '0' && c <= '9') ||
    (c == '-') || (c == '_') || (c == '.') || (c == ':');
  return ok ? 1 : 0;
}

static int umbmp_trace_char_ok(char c) {
  // trace_id extends the id token set with '@' (UM‑EAIS v0.2, platform trace_id validator).
  if (umbmp_char_ok(c)) return 1;
  return (c == '@') ? 1 : 0;
}

static int hex_char_ok(char c) {
  const int ok =
    (c >= '0' && c <= '9') ||
    (c >= 'a' && c <= 'f') ||
    (c >= 'A' && c <= 'F');
  return ok ? 1 : 0;
}

int agent_umbmp_id_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0 || len > AGENT_UM_BMP_MAX_ID_LEN) return 0;
  for (size_t i = 0; i < len; i++) {
    if (!umbmp_char_ok(s[i])) return 0;
  }
  return 1;
}

int agent_umbmp_trace_id_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0 || len > AGENT_UM_BMP_MAX_ID_LEN) return 0;
  for (size_t i = 0; i < len; i++) {
    if (!umbmp_trace_char_ok(s[i])) return 0;
  }
  return 1;
}

int agent_umbmp_sha256_token_is_safe(const char* s, size_t len) {
  if (!s) return 0;
  if (len == 0) return 0;

  const char* p = s;
  size_t n = len;
  static const char* kPrefix = "sha256:";
  static const size_t kPrefixLen = 7;
  if (n > kPrefixLen && memcmp(p, kPrefix, kPrefixLen) == 0) {
    p += kPrefixLen;
    n -= kPrefixLen;
  }
  if (n != 64) return 0;
  for (size_t i = 0; i < n; i++) {
    if (!hex_char_ok(p[i])) return 0;
  }
  return 1;
}

agent_status_t agent_umbmp_sanitize_id_token(
  const char* in,
  size_t in_len,
  char* out,
  size_t out_cap,
  size_t max_len,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';

  const size_t cap_len = out_cap - 1; // reserve NUL
  if (cap_len == 0) return AGENT_ERR_BOUNDS;

  if (max_len == 0 || max_len > cap_len) max_len = cap_len;
  size_t n = in_len;
  if (n > max_len) n = max_len;

  // Map invalid characters to '_' and truncate.
  for (size_t i = 0; i < n; i++) {
    const char c = in ? in[i] : '\0';
    out[i] = umbmp_char_ok(c) ? c : '_';
  }

  // Trim leading/trailing underscores.
  size_t start = 0;
  while (start < n && out[start] == '_') start++;
  size_t end = n;
  while (end > start && out[end - 1] == '_') end--;

  if (end <= start) {
    // Default token.
    static const char* kDefault = "msg";
    const size_t kDefaultLen = 3;
    if (out_cap <= kDefaultLen) return AGENT_ERR_BOUNDS;
    memcpy(out, kDefault, kDefaultLen);
    out[kDefaultLen] = '\0';
    if (out_len) *out_len = kDefaultLen;
    return AGENT_OK;
  }

  const size_t trimmed_len = end - start;
  if (start != 0) memmove(out, out + start, trimmed_len);
  out[trimmed_len] = '\0';
  if (out_len) *out_len = trimmed_len;
  return AGENT_OK;
}

agent_status_t agent_um_eais_result_attest_signing_input_v0_1(
  const char* task_id,
  size_t task_id_len,
  const char* step_id,
  size_t step_id_len,
  const char* idempotency_key,
  size_t idempotency_key_len,
  const char* result_sha256_token,
  size_t result_sha256_token_len,
  int64_t ts_utc_ms,
  char* out,
  size_t out_cap,
  size_t* out_len
) {
  if (out_len) *out_len = 0;
  if (!out || out_cap == 0) return AGENT_ERR_INVALID_ARGUMENT;
  out[0] = '\0';

  if (!agent_umbmp_id_is_safe(task_id, task_id_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(step_id, step_id_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_id_is_safe(idempotency_key, idempotency_key_len)) return AGENT_ERR_INVALID_ARGUMENT;
  if (!agent_umbmp_sha256_token_is_safe(result_sha256_token, result_sha256_token_len)) return AGENT_ERR_INVALID_ARGUMENT;

  // Reserve NUL for convenience, but return the byte length excluding it.
  const size_t cap_len = out_cap - 1;
  if (cap_len == 0) return AGENT_ERR_BOUNDS;

  // Convert timestamp once so we can compute/validate bounds.
  char ts_buf[32];
  const int ts_n = snprintf(ts_buf, sizeof(ts_buf), "%" PRId64 "\n", ts_utc_ms);
  if (ts_n <= 0) return AGENT_ERR_INTERNAL;
  const size_t ts_len = (size_t)ts_n;
  if (ts_len >= sizeof(ts_buf)) return AGENT_ERR_INTERNAL;

  static const char* kPrefix = AGENT_UM_EAIS_RESULT_ATTEST_SIGNING_PREFIX;
  static const size_t kPrefixLen = sizeof("UM_EAIS_RESULT_ATTEST_v0_1\n") - 1;

  // Compute required size (bytes), including newlines after each field.
  const size_t need =
    kPrefixLen +
    task_id_len + 1 +
    step_id_len + 1 +
    idempotency_key_len + 1 +
    result_sha256_token_len + 1 +
    ts_len;

  if (need > cap_len) return AGENT_ERR_BOUNDS;

  size_t pos = 0;
  memcpy(out + pos, kPrefix, kPrefixLen);
  pos += kPrefixLen;

  memcpy(out + pos, task_id, task_id_len);
  pos += task_id_len;
  out[pos++] = '\n';

  memcpy(out + pos, step_id, step_id_len);
  pos += step_id_len;
  out[pos++] = '\n';

  memcpy(out + pos, idempotency_key, idempotency_key_len);
  pos += idempotency_key_len;
  out[pos++] = '\n';

  memcpy(out + pos, result_sha256_token, result_sha256_token_len);
  pos += result_sha256_token_len;
  out[pos++] = '\n';

  memcpy(out + pos, ts_buf, ts_len);
  pos += ts_len;

  out[pos] = '\0';
  if (out_len) *out_len = pos;
  return AGENT_OK;
}

size_t agent_edge_consensus_quorum_for_cluster_size(size_t cluster_size) {
  if (cluster_size < 1) cluster_size = 1;
  return (cluster_size / 2) + 1;
}

int agent_edge_consensus_has_quorum(size_t cluster_size, size_t vote_count) {
  return vote_count >= agent_edge_consensus_quorum_for_cluster_size(cluster_size) ? 1 : 0;
}

static int consensus_string_eq(const char* s, size_t s_len, const char* lit) {
  if (!s || !lit) return 0;
  const size_t lit_len = strlen(lit);
  return s_len == lit_len && memcmp(s, lit, lit_len) == 0;
}

int agent_edge_consensus_frame_kind_is_valid(const char* kind, size_t kind_len) {
  return
    consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_REQUEST) ||
    consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_VOTE_GRANT) ||
    consensus_string_eq(kind, kind_len, AGENT_EDGE_CONSENSUS_KIND_LEADER_COMMIT);
}

int agent_edge_consensus_identity_membership_matches(
  uint64_t local_membership_epoch,
  uint64_t identity_membership_epoch,
  int identity_node_is_member
) {
  return local_membership_epoch == identity_membership_epoch && identity_node_is_member ? 1 : 0;
}

static int64_t consensus_clamp_i64(int64_t value, int64_t lo, int64_t hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

agent_status_t agent_edge_consensus_policy_timing_normalize(
  agent_edge_consensus_policy_timing_t* timing
) {
  if (!timing) return AGENT_ERR_INVALID_ARGUMENT;
  timing->campaign_delay_ms = consensus_clamp_i64(
    timing->campaign_delay_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);
  timing->campaign_retry_ms = consensus_clamp_i64(
    timing->campaign_retry_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);

  const int64_t retry_max_upper = AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS;
  if (timing->campaign_retry_max_ms > retry_max_upper) timing->campaign_retry_max_ms = retry_max_upper;
  if (timing->campaign_retry_max_ms < timing->campaign_retry_ms) {
    timing->campaign_retry_max_ms = timing->campaign_retry_ms;
  }

  timing->campaign_retry_backoff_factor = consensus_clamp_i64(
    timing->campaign_retry_backoff_factor,
    AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MIN,
    AGENT_EDGE_CONSENSUS_POLICY_BACKOFF_FACTOR_MAX);
  timing->leader_heartbeat_ms = consensus_clamp_i64(
    timing->leader_heartbeat_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_RETRY_MAX_MS);

  if (timing->leader_lease_ms > AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS) {
    timing->leader_lease_ms = AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS;
  }
  if (timing->leader_lease_ms < timing->leader_heartbeat_ms) {
    timing->leader_lease_ms = timing->leader_heartbeat_ms;
  }

  timing->lease_expiry_recampaign_delay_ms = consensus_clamp_i64(
    timing->lease_expiry_recampaign_delay_ms, 0, AGENT_EDGE_CONSENSUS_POLICY_LEASE_MAX_MS);
  timing->stale_runtime_recovery_grace_ms = consensus_clamp_i64(
    timing->stale_runtime_recovery_grace_ms,
    0,
    AGENT_EDGE_CONSENSUS_POLICY_STALE_RUNTIME_RECOVERY_GRACE_MAX_MS);
  return AGENT_OK;
}
