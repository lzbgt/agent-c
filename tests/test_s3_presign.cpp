#include "blob_object_store.h"

#include <cassert>
#include <iostream>

using agentd::DaemonConfig;
using agentd::blob_object_store_presign_url;

static void expect_eq(const std::string& got, const std::string& expect, const char* what) {
  if (got != expect) {
    std::cerr << "FAILED: " << (what ? what : "value") << "\n"
              << "  got:    " << got << "\n"
              << "  expect: " << expect << "\n";
    std::abort();
  }
}

int main() {
  DaemonConfig cfg;
  cfg.blob_store_mode = "object";
  cfg.blob_store_endpoint = "https://s3.amazonaws.com";
  cfg.blob_store_region = "us-east-1";
  cfg.blob_store_bucket = "examplebucket";
  cfg.blob_store_path_style = false;
  cfg.blob_store_access_key = "AKIAIOSFODNN7EXAMPLE";
  cfg.blob_store_secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
  cfg.blob_store_presign_ttl_sec = 86400;

  const int64_t now_ms = 1369353600LL * 1000LL; // 2013-05-24T00:00:00Z
  std::string url;
  std::string err;
  const bool ok = blob_object_store_presign_url(cfg, "GET", "test.txt", now_ms, 86400, &url, &err);
  assert(ok && err.empty());

  const std::string expect =
    "https://examplebucket.s3.amazonaws.com/test.txt"
    "?X-Amz-Algorithm=AWS4-HMAC-SHA256"
    "&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request"
    "&X-Amz-Date=20130524T000000Z"
    "&X-Amz-Expires=86400"
    "&X-Amz-SignedHeaders=host"
    "&X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404";

  expect_eq(url, expect, "presigned URL");

  std::cout << "ok\n";
  return 0;
}
