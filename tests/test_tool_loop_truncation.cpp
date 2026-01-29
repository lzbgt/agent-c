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

static void test_json_entries_array_truncation() {
  // Simulate a bounded directory listing tool that still produces a large JSON array payload.
  std::string in = "{\"ok\":true,\"data\":{\"tool\":\"fs_list\",\"entries\":[";
  for (int i = 0; i < 200; i++) {
    if (i) in += ",";
    in += "{\"path\":\"/very/long/path/";
    in += std::to_string(i);
    in += "_";
    in += std::string(80, 'x');
    in += "\",\"type\":\"file\"}";
  }
  in += "]}}";

  bool truncated = false;
  const std::string out = tool_loop_cap_tool_output_for_prompt(in, 400, &truncated);
  assert(truncated);
  assert(out.size() <= 400);
  // Should remain JSON-shaped enough for UIs/clients to parse.
  assert(out.find("\"prompt_truncated\"") != std::string::npos);
  // Prefer entries truncation/dropping over invalid JSON string chopping.
  assert(
    out.find("\"entries_truncated\"") != std::string::npos ||
    out.find("\"entries_dropped\"") != std::string::npos ||
    out.find("tool_output_truncated") != std::string::npos
  );
}

static void test_json_matches_array_truncation() {
  // Simulate text_search returning many matches.
  std::string in = "{\"ok\":true,\"data\":{\"tool\":\"text_search\",\"matches\":[";
  for (int i = 0; i < 200; i++) {
    if (i) in += ",";
    in += "{\"path\":\"/repo/file";
    in += std::to_string(i);
    in += ".cpp\",\"line\":";
    in += std::to_string(100 + i);
    in += ",\"column\":1,\"snippet\":\"";
    in += std::string(80, 'y');
    in += "\"}";
  }
  in += "]}}";

  bool truncated = false;
  const std::string out = tool_loop_cap_tool_output_for_prompt(in, 400, &truncated);
  assert(truncated);
  assert(out.size() <= 400);
  assert(out.find("\"prompt_truncated\"") != std::string::npos);
  assert(
    out.find("\"matches_truncated\"") != std::string::npos ||
    out.find("\"matches_dropped\"") != std::string::npos ||
    out.find("tool_output_truncated") != std::string::npos
  );
}

int main() {
  test_plain_text_truncation();
  test_json_output_field_truncation();
  test_json_content_field_truncation();
  test_json_entries_array_truncation();
  test_json_matches_array_truncation();
  return 0;
}
