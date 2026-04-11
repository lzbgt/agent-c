#include "agent/edge_interop.h"

#include <assert.h>
#include <string.h>

static void test_id_is_safe_basic(void) {
  assert(agent_umbmp_id_is_safe("wf:abc-XYZ_09.", strlen("wf:abc-XYZ_09.")) == 1);
  assert(agent_umbmp_id_is_safe("edge_msg:1234", strlen("edge_msg:1234")) == 1);
  assert(agent_umbmp_id_is_safe("", 0) == 0);
  assert(agent_umbmp_id_is_safe("has space", strlen("has space")) == 0);
  assert(agent_umbmp_id_is_safe("has/slash", strlen("has/slash")) == 0);
  assert(agent_umbmp_id_is_safe("has\"quote", strlen("has\"quote")) == 0);
}

static void test_trace_id_is_safe_allows_at(void) {
  assert(agent_umbmp_trace_id_is_safe("trace:demo@x", strlen("trace:demo@x")) == 1);
  assert(agent_umbmp_trace_id_is_safe("trace:bad space", strlen("trace:bad space")) == 0);
  // '@' is NOT allowed for general ids.
  assert(agent_umbmp_id_is_safe("trace:demo@x", strlen("trace:demo@x")) == 0);
}

static void test_sha256_token_is_safe_basic(void) {
  assert(agent_umbmp_sha256_token_is_safe("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                          strlen("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) == 1);
  assert(agent_umbmp_sha256_token_is_safe("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                          strlen("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")) == 1);
  assert(agent_umbmp_sha256_token_is_safe("sha256:zzz", strlen("sha256:zzz")) == 0);
  assert(agent_umbmp_sha256_token_is_safe("", 0) == 0);
}

static void test_sanitize_trims_and_defaults(void) {
  char out[64];
  size_t n = 0;

  assert(agent_umbmp_sanitize_id_token("___hello$$$", strlen("___hello$$$"), out, sizeof(out), 32, &n) == AGENT_OK);
  assert(strcmp(out, "hello") == 0);
  assert(n == strlen("hello"));

  assert(agent_umbmp_sanitize_id_token("$$$", strlen("$$$"), out, sizeof(out), 32, &n) == AGENT_OK);
  assert(strcmp(out, "msg") == 0);
  assert(n == strlen("msg"));

  assert(agent_umbmp_sanitize_id_token(NULL, 0, out, sizeof(out), 32, &n) == AGENT_OK);
  assert(strcmp(out, "msg") == 0);
  assert(n == strlen("msg"));
}

static void test_sanitize_respects_max_len(void) {
  char out[16];
  size_t n = 0;
  const char* in = "abcdefghijklmnopqrstuvwxyz";
  assert(agent_umbmp_sanitize_id_token(in, strlen(in), out, sizeof(out), 8, &n) == AGENT_OK);
  assert(n == 8);
  assert(strcmp(out, "abcdefgh") == 0);
}

static void test_result_attest_signing_input_v0_1(void) {
  char out[256];
  size_t out_len = 0;

  const char* task_id = "t1";
  const char* step_id = "s1";
  const char* idem = "k1";
  const char* sha = "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  const int64_t ts = 1700000000000LL;

  assert(agent_um_eais_result_attest_signing_input_v0_1(
           task_id, strlen(task_id),
           step_id, strlen(step_id),
           idem, strlen(idem),
           sha, strlen(sha),
           ts,
           out, sizeof(out),
           &out_len) == AGENT_OK);

  const char* expected =
    "UM_EAIS_RESULT_ATTEST_v0_1\n"
    "t1\n"
    "s1\n"
    "k1\n"
    "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "1700000000000\n";
  assert(out_len == strlen(expected));
  assert(strcmp(out, expected) == 0);

  // Reject unsafe IDs (newline would be ambiguous).
  assert(agent_um_eais_result_attest_signing_input_v0_1(
           "bad\nid", strlen("bad\nid"),
           step_id, strlen(step_id),
           idem, strlen(idem),
           sha, strlen(sha),
           ts,
           out, sizeof(out),
           &out_len) == AGENT_ERR_INVALID_ARGUMENT);
}

static void test_consensus_constants_and_quorum(void) {
  assert(strcmp(AGENT_UM_BMP_TYPE_CONSENSUS_FRAME, "CONSENSUS_FRAME") == 0);
  assert(strcmp(
           AGENT_UM_BMP_TYPE_PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE,
           "PLATFORM_CONSENSUS_MEMBERSHIP_BUNDLE") == 0);
  assert(strcmp(AGENT_EDGE_CONSENSUS_FRAME_SCHEMA_V1, "edge_node_consensus_frame_v1") == 0);
  assert(strcmp(AGENT_EDGE_CONSENSUS_MEMBERSHIP_SCHEMA_V1, "edge_consensus_membership_v1") == 0);
  assert(strcmp(
           AGENT_EDGE_CONSENSUS_MEMBERSHIP_ATTEST_SCHEMA_V1,
           "edge_consensus_membership_attest_v1") == 0);

  assert(agent_edge_consensus_quorum_for_cluster_size(0) == 1);
  assert(agent_edge_consensus_quorum_for_cluster_size(1) == 1);
  assert(agent_edge_consensus_quorum_for_cluster_size(2) == 2);
  assert(agent_edge_consensus_quorum_for_cluster_size(3) == 2);
  assert(agent_edge_consensus_quorum_for_cluster_size(4) == 3);
  assert(agent_edge_consensus_quorum_for_cluster_size(5) == 3);
}

void test_edge_interop_module(void) {
  test_id_is_safe_basic();
  test_trace_id_is_safe_allows_at();
  test_sha256_token_is_safe_basic();
  test_sanitize_trims_and_defaults();
  test_sanitize_respects_max_len();
  test_result_attest_signing_input_v0_1();
  test_consensus_constants_and_quorum();
}
