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

void test_edge_interop_module(void) {
  test_id_is_safe_basic();
  test_sanitize_trims_and_defaults();
  test_sanitize_respects_max_len();
}

