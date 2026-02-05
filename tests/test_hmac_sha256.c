#include "agent/hmac_sha256.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_hmac_sha256_rfc4231_case_1(void) {
  // RFC 4231 test case 1:
  // key = 0x0b repeated 20
  // data = "Hi There"
  // HMAC-SHA256 = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  const char* msg = "Hi There";

  uint8_t mac[32];
  memset(mac, 0, sizeof(mac));
  agent_hmac_sha256(key, sizeof(key), msg, strlen(msg), mac);

  const uint8_t expect[32] = {
    0xb0, 0x34, 0x4c, 0x61, 0xd8, 0xdb, 0x38, 0x53,
    0x5c, 0xa8, 0xaf, 0xce, 0xaf, 0x0b, 0xf1, 0x2b,
    0x88, 0x1d, 0xc2, 0x00, 0xc9, 0x83, 0x3d, 0xa7,
    0x26, 0xe9, 0x37, 0x6c, 0x2e, 0x32, 0xcf, 0xf7,
  };
  assert(memcmp(mac, expect, sizeof(expect)) == 0);
}

void test_hmac_sha256_module(void) {
  test_hmac_sha256_rfc4231_case_1();
}

