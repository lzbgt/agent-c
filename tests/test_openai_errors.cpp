#include "openai_client.h"

#include <cassert>
#include <iostream>

static void expect_true(bool v, const char* what) {
  if (!v) {
    std::cerr << "FAILED: expected true: " << (what ? what : "") << "\n";
    std::abort();
  }
}

static void expect_false(bool v, const char* what) {
  if (v) {
    std::cerr << "FAILED: expected false: " << (what ? what : "") << "\n";
    std::abort();
  }
}

int main() {
  // HTTP-level rejections.
  expect_true(openai_is_context_too_long_error(413, ""), "413 implies too large");

  // OpenAI-style JSON error.
  expect_true(
    openai_is_context_too_long_error(
      400,
      R"({"error":{"message":"This model's maximum context length is 8192 tokens, however you requested 9000 tokens."}})"
    ),
    "OpenAI context length exceeded message"
  );

  // Plain text / non-JSON providers.
  expect_true(
    openai_is_context_too_long_error(400, "request too large: context_length_exceeded"),
    "plain context_length_exceeded"
  );

  // Non-matching error should not trigger session rotation.
  expect_false(
    openai_is_context_too_long_error(400, R"({"error":{"message":"invalid api key"}})"),
    "invalid key should not match"
  );

  std::cout << "ok\n";
  return 0;
}

