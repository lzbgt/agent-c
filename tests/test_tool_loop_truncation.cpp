#include "tool_loop_truncation.h"

#include <cassert>
#include <string>

static void test_plain_text_truncation() {
  std::string s(10000, 'x');
  bool truncated = false;
  const std::string out = tool_loop_cap_tool_output_for_prompt(s, 100, &truncated);
  assert(truncated);
  assert(out.size() <= 100);
}

static void test_json_output_field_truncation() {
  // Simulate a host tool JSON envelope with a large output string.
  std::string big(5000, 'A');
  const std::string in =
    std::string("{\"ok\":true,\"data\":{\"tool\":\"shell_exec\",\"exit_code\":0,\"output\":\"") + big + "\"}}";
  bool truncated = false;
  const std::string out = tool_loop_cap_tool_output_for_prompt(in, 300, &truncated);
  assert(truncated);
  // Should remain JSON-ish and contain marker
  assert(out.find("\"prompt_truncated\"") != std::string::npos);
  assert(out.size() <= 300);
}

static void test_json_content_field_truncation() {
  std::string big(5000, 'B');
  const std::string in =
    std::string("{\"ok\":true,\"data\":{\"tool\":\"fs_read\",\"path\":\"x\",\"content\":\"") + big + "\"}}";
  bool truncated = false;
  const std::string out = tool_loop_cap_tool_output_for_prompt(in, 300, &truncated);
  assert(truncated);
  assert(out.find("\"prompt_truncated\"") != std::string::npos);
  assert(out.size() <= 300);
}

int main() {
  test_plain_text_truncation();
  test_json_output_field_truncation();
  test_json_content_field_truncation();
  return 0;
}
