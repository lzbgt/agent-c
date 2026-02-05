#include "agent/base64.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_rfc4648_vectors(void) {
  struct {
    const char* plain;
    const char* b64;
  } vecs[] = {
    {"", ""},
    {"f", "Zg=="},
    {"fo", "Zm8="},
    {"foo", "Zm9v"},
    {"foob", "Zm9vYg=="},
    {"fooba", "Zm9vYmE="},
    {"foobar", "Zm9vYmFy"},
  };

  for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
    const char* plain = vecs[i].plain;
    const char* want_b64 = vecs[i].b64;
    const size_t plain_len = strlen(plain);
    const size_t want_len = strlen(want_b64);

    char out[128];
    size_t out_len = 0;
    assert(agent_base64_encode((const uint8_t*)plain, plain_len, out, sizeof(out), &out_len) == AGENT_OK);
    assert(out_len == want_len);
    assert(strcmp(out, want_b64) == 0);

    uint8_t back[128];
    size_t back_len = 0;
    assert(agent_base64_decode(want_b64, want_len, back, sizeof(back), &back_len) == AGENT_OK);
    assert(back_len == plain_len);
    assert(memcmp(back, plain, plain_len) == 0);

    // Also accept unpadded decode when possible.
    if (want_len && want_b64[want_len - 1] == '=') {
      // Trim all '=' at end.
      size_t unpad_len = want_len;
      while (unpad_len > 0 && want_b64[unpad_len - 1] == '=') unpad_len--;
      back_len = 0;
      assert(agent_base64_decode(want_b64, unpad_len, back, sizeof(back), &back_len) == AGENT_OK);
      assert(back_len == plain_len);
      assert(memcmp(back, plain, plain_len) == 0);
    }
  }
}

static void test_rejects_urlsafe_and_whitespace(void) {
  uint8_t out[64];
  size_t out_len = 0;
  assert(agent_base64_decode("Zg==\n", strlen("Zg==\n"), out, sizeof(out), &out_len) == AGENT_ERR_INVALID_ARGUMENT);
  assert(agent_base64_decode("Zg== ", strlen("Zg== "), out, sizeof(out), &out_len) == AGENT_ERR_INVALID_ARGUMENT);
  assert(agent_base64_decode("Zg==\t", strlen("Zg==\t"), out, sizeof(out), &out_len) == AGENT_ERR_INVALID_ARGUMENT);
  assert(agent_base64_decode("Zg-_", strlen("Zg-_"), out, sizeof(out), &out_len) == AGENT_ERR_INVALID_ARGUMENT);
}

void test_base64_module(void) {
  test_rfc4648_vectors();
  test_rejects_urlsafe_and_whitespace();
}

