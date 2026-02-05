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

void test_edge_interop_module(void) {
  test_id_is_safe_basic();
  test_sha256_token_is_safe_basic();
  test_sanitize_trims_and_defaults();
  test_sanitize_respects_max_len();
  test_result_attest_signing_input_v0_1();
}
