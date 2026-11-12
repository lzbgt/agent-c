#include "tool_loop_truncation.h"

// Keep these boundary assertions active in optimized test builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <json/json.h>

// Existing host event summary implementation; no portable core ABI change.
Json::Value summarize_tool_output_json(const std::string& output, const std::string& tool_name);

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


static std::string encode_json(const Json::Value& value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}

static void test_web_output_summary_provenance() {
  Json::Value input;
  input["ok"] = true;
  auto& data = input["data"];
  data["source"] = "http";
  data["requested_url"] = "https://example.org/";
  data["url"] = "https://example.org/releases";
  data["title"] = "Release versions";
  data["http_status"] = 200;
  data["fetched_at"] = "2026-11-12T18:00:00Z";
  data["cache_hit"] = false;
  data["cache_age_ms"] = 0;
  data["rendered"] = false;
  data["truncated"] = false;
  data["untrusted_content"] = true;
  data["text"] = "stable: 7.3.4 2026-11-10 " + std::string(3000, 'x');
  data["headers"]["Authorization"] = "PRIVATE_PROVIDER_KEY";
  data["html"] = "<script>RAW_HTML_MUST_NOT_BE_RETAINED</script>";
  data["output"] = "UNRELATED_OUTPUT_MUST_NOT_BE_RETAINED";
  const auto summary = summarize_tool_output_json(encode_json(input), "web_fetch");
  assert(summary["source"] == "http");
  assert(summary["requested_url"] == data["requested_url"] && summary["url"] == data["url"]);
  assert(summary["title"] == data["title"] && summary["fetched_at"] == data["fetched_at"]);
  assert(summary["http_status"] == 200 && !summary["cache_hit"].asBool() && !summary["rendered"].asBool());
  assert(summary["text_excerpt"].asString() == data["text"].asString().substr(0, 1024));
  assert(summary["excerpt_truncated"].asBool() && !summary["truncated"].asBool());
  assert(summary["text_chars"].asUInt64() == data["text"].asString().size());
  const auto encoded = encode_json(summary);
  assert(encoded.size() <= 16 * 1024);
  assert(encoded.find("PRIVATE_PROVIDER_KEY") == std::string::npos);
  assert(encoded.find("RAW_HTML_MUST_NOT_BE_RETAINED") == std::string::npos);
  assert(encoded.find("UNRELATED_OUTPUT_MUST_NOT_BE_RETAINED") == std::string::npos);
  for (size_t length : {size_t(4095), size_t(4096)}) {
    const std::string prefix = "https://example.org/";
    const auto complete_url = prefix + std::string(length - prefix.size(), 'a');
    data["url"] = complete_url;
    data["requested_url"] = complete_url;
    const auto long_url_summary = summarize_tool_output_json(encode_json(input), "web_fetch");
    assert(long_url_summary["url"].asString() == complete_url);
    assert(long_url_summary["requested_url"].asString() == complete_url);
    assert(encode_json(long_url_summary).size() <= 16 * 1024);
  }

  // Only the trusted executor's actual web tool name selects this summary.
  data["output"] = "ordinary shell output";
  const auto other = summarize_tool_output_json(encode_json(input), "shell_exec");
  assert(!other.isMember("source") && !other.isMember("url") && !other.isMember("text_excerpt"));
  assert(other["output"] == "ordinary shell output");

  input["ok"] = false;
  input["error"] = "HTTP page returned status 403";
  data["http_status"] = 403;
  data["blocked"] = true;
  const auto failure = summarize_tool_output_json(encode_json(input), "web_fetch");
  assert(!failure["ok"].asBool() && failure["error"] == input["error"]);
  assert(failure["blocked"].asBool() && failure["http_status"] == 403);
}

static void test_web_output_summary_bounds() {
  Json::Value input;
  input["ok"] = true;
  auto& data = input["data"];
  data["source"] = "searxng";
  data["query"] = "current stable release";
  for (int i = 0; i < 5; ++i) {
    Json::Value item;
    item["title"] = "source " + std::to_string(i);
    item["url"] = "https://example.org/" + std::to_string(i);
    item["snippet"] = "FULL_SNIPPET_MUST_NOT_BE_RETAINED";
    data["results"].append(item);
  }
  auto summary = summarize_tool_output_json(encode_json(input), "web_search");
  assert(summary["source"] == "searxng" && summary["query"] == data["query"]);
  assert(summary["results"].size() == 3 && summary["result_count"].asUInt64() == 5);
  assert(summary["results_truncated"].asBool());
  assert(summary["results"][0]["url"] == "https://example.org/0");
  assert(!summary["results"][0].isMember("snippet"));
  assert(encode_json(summary).find("FULL_SNIPPET_MUST_NOT_BE_RETAINED") == std::string::npos);

  const std::string prefix = "https://example.org/";
  const auto complete_url = prefix + std::string(4095 - prefix.size(), 'a');
  data["results"][0]["url"] = complete_url;
  summary = summarize_tool_output_json(encode_json(input), "web_search");
  assert(summary["results"][0]["url"].asString() == complete_url);

  data["query"] = std::string(10000, '"');
  for (auto& item : data["results"]) {
    item["title"] = std::string(10000, '"');
    item["url"] = std::string(10000, '"');
  }
  summary = summarize_tool_output_json(encode_json(input), "web_search");
  assert(encode_json(summary).size() <= 16 * 1024);
  assert(summary["summary_truncated"].asBool());
  assert(summary["results"].empty()); // Oversized identifiers are omitted, not shortened.

  std::string multibyte;
  for (int i = 0; i < 2000; ++i) multibyte += "\xe4\xb8\xad";
  data.removeMember("results");
  data["text"] = multibyte;
  data["url"] = std::string(10000, '"');
  data["requested_url"] = std::string(10000, '"');
  data["title"] = multibyte;
  summary = summarize_tool_output_json(encode_json(input), "web_fetch");
  assert(encode_json(summary).size() <= 16 * 1024);
  assert(!summary.isMember("url") && !summary.isMember("requested_url"));
  assert(summary["summary_truncated"].asBool());
  const auto excerpt = summary["text_excerpt"].asString();
  assert(!excerpt.empty() && excerpt.size() % 3 == 0 && excerpt.size() <= 1024 * 3);
  assert(multibyte.compare(0, excerpt.size(), excerpt) == 0);
  assert(summary["text_chars"].asUInt64() == 2000 && summary["excerpt_truncated"].asBool());
}

int main() {
  test_plain_text_truncation();
  test_json_output_field_truncation();
  test_json_content_field_truncation();
  test_json_entries_array_truncation();
  test_json_matches_array_truncation();
  test_web_output_summary_provenance();
  test_web_output_summary_bounds();
  return 0;
}
