#include "agent/json_c14n.h"

#include <assert.h>
#include <string.h>

static void test_object_key_sort_and_number_normalize(void) {
  const char* a = "{\"b\":1,\"a\":2}";
  const char* b = "{ \"a\" : 2.0000e+0 ,\"b\": 1 }";

  char* ca = NULL;
  size_t na = 0;
  char* cb = NULL;
  size_t nb = 0;
  char err[256];
  memset(err, 0, sizeof(err));

  assert(agent_json_c14n_canonicalize(a, strlen(a), &ca, &na, err, sizeof(err)) == AGENT_OK);
  assert(agent_json_c14n_canonicalize(b, strlen(b), &cb, &nb, err, sizeof(err)) == AGENT_OK);
  assert(ca && cb);
  assert(na == nb);
  assert(strcmp(ca, cb) == 0);

  // Expected canonical form: keys sorted, compact, exponent removed, and number tokens normalized.
  assert(strcmp(ca, "{\"a\":2,\"b\":1}") == 0);

  agent_free(ca);
  agent_free(cb);
}

static void test_sha256_token_stable(void) {
  const char* a = "{\"x\":1,\"y\":[true,false,null,\"hi\"]}";
  const char* b = "{\n\"y\" : [ true , false , null , \"hi\" ] , \"x\" : 1e0 \n}";

  char ta[80];
  char tb[80];
  char err[256];
  memset(ta, 0, sizeof(ta));
  memset(tb, 0, sizeof(tb));
  memset(err, 0, sizeof(err));

  assert(agent_json_c14n_sha256_token(a, strlen(a), ta, err, sizeof(err)) == AGENT_OK);
  assert(agent_json_c14n_sha256_token(b, strlen(b), tb, err, sizeof(err)) == AGENT_OK);
  assert(strcmp(ta, tb) == 0);
  assert(strlen(ta) == strlen("sha256:") + 64);
}

void test_json_c14n_module(void) {
  test_object_key_sort_and_number_normalize();
  test_sha256_token_stable();
}
