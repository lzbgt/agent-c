#include "tool_loop_truncation.h"

// Keep these boundary assertions active in optimized test builds.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <sstream>
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
  data["published_at"] = "2025-11-15T12:00:00Z";
  data["publication_date_source"] = "page.metadata.article:published_time";
  data["publication_date_status"] = "unverified";
  data["publication_date_unknown"] = false;
  data["modified_at"] = "2026-11-12T10:00:00Z";
  data["modified_date_source"] = "page.metadata.article:modified_time";
  data["freshness_verified"] = false;
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
  assert(summary["published_at"] == data["published_at"] && summary["modified_at"] == data["modified_at"]);
  assert(summary["publication_date_source"] == data["publication_date_source"]);
  assert(summary["publication_date_status"] == "unverified" && !summary["publication_date_unknown"].asBool());
  assert(summary.isMember("freshness_verified") && !summary["freshness_verified"].asBool());
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
  data["time_range"] = "day";
  data["time_range_scope"] = "upstream_hint";
  for (int i = 0; i < 5; ++i) {
    Json::Value item;
    item["title"] = "source " + std::to_string(i);
    item["url"] = "https://example.org/" + std::to_string(i);
    item["snippet"] = "FULL_SNIPPET_MUST_NOT_BE_RETAINED";
    item["published_at"] = i == 0 ? "2025-11-15T12:00:00Z" : "";
    item["publication_date_source"] = i == 0 ? "searxng.publishedDate" : "";
    item["publication_date_status"] = i == 0 ? "unverified" : "unknown";
    item["publication_date_unknown"] = i != 0;
    data["results"].append(item);
  }
  auto summary = summarize_tool_output_json(encode_json(input), "web_search");
  assert(summary["source"] == "searxng" && summary["query"] == data["query"]);
  assert(summary["results"].size() == 3 && summary["result_count"].asUInt64() == 5);
  assert(summary["results_truncated"].asBool());
  assert(summary["results"][0]["url"] == "https://example.org/0");
  assert(summary["time_range"] == "day" && summary["time_range_scope"] == "upstream_hint");
  assert(summary["results"][0]["published_at"] == "2025-11-15T12:00:00Z");
  assert(summary["results"][0]["publication_date_status"] == "unverified");
  assert(summary["results"][1]["publication_date_unknown"].asBool());
  assert(summary["results"][1]["publication_date_status"] == "unknown");
  assert(!summary["results"][0].isMember("snippet"));
  assert(encode_json(summary).find("FULL_SNIPPET_MUST_NOT_BE_RETAINED") == std::string::npos);

  const std::string prefix = "https://example.org/";
  const auto complete_url = prefix + std::string(4095 - prefix.size(), 'a');
  data["results"][0]["url"] = complete_url;
  summary = summarize_tool_output_json(encode_json(input), "web_search");
  assert(summary["results"][0]["url"].asString() == complete_url);

  data["results"][0]["published_at"] = std::string(129, 'x');
  summary = summarize_tool_output_json(encode_json(input), "web_search");
  assert(!summary["results"][0].isMember("published_at") && summary["summary_truncated"].asBool());

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


static void test_saved_exec_output_receipt() {
  const std::string full = std::string("HEAD\n") + std::string(80000, 'x') + "\nTAIL";
  Json::Value input;
  input["ok"] = true;
  auto& data = input["data"];
  data["tool"] = "shell_exec";
  data["exit_code"] = 0;
  data["truncated"] = false;
  data["capture_truncated"] = false;
  data["capture_complete"] = true;
  data["captured_output_bytes"] = Json::UInt64(full.size());
  data["observed_output_bytes"] = Json::UInt64(full.size());
  data["output"] = full;
  data["output_artifact"]["path"] = "out/tool-output-fixture";
  data["output_artifact"]["size_bytes"] = Json::UInt64(full.size());
  data["output_artifact"]["capture_truncated"] = false;
  const std::string original = encode_json(input);
  for (const auto* tool : {"shell_exec", "proc_exec"}) {
    for (size_t limit : {size_t(0), size_t(16000)}) {
      bool truncated = false;
      const auto compact = tool_loop_cap_tool_output_for_prompt(original, limit, &truncated, tool);
      Json::Value parsed;
      Json::CharReaderBuilder builder;
      std::string errors;
      std::istringstream stream(compact);
      assert(Json::parseFromStream(builder, stream, &parsed, &errors));
      const auto& result = parsed["data"];
      assert(truncated && compact.size() < 2048);
      assert(!result.isMember("output"));
      assert(compact.find("HEAD") == std::string::npos && compact.find("TAIL") == std::string::npos);
      assert(compact.find(std::string(100, 'x')) == std::string::npos);
      assert(result["output_omitted_from_prompt"].asBool() && result["prompt_truncated"].asBool());
      assert(!result["truncated"].asBool() && !result["capture_truncated"].asBool());
      assert(result["output_readback"]["tool"].asString() == "fs_read");
      assert(result["output_readback"]["arguments"]["path"] == data["output_artifact"]["path"]);
    }
  }

  // The payload's tool field cannot spoof executor identity (e.g. web content).
  bool truncated = false;
  const auto untrusted = tool_loop_cap_tool_output_for_prompt(original, 0, &truncated, "web_fetch");
  assert(!truncated);
  assert(untrusted.find("output_omitted_from_prompt") == std::string::npos);
  assert(untrusted.size() > full.size());

  // UI evidence is independent: display clipping is distinct from capture loss.
  const auto summary = summarize_tool_output_json(original, "shell_exec");
  assert(summary["output"].asString().size() <= 64 * 1024 + 32);
  assert(summary["output_display_truncated"].asBool() && summary["output_truncated"].asBool());
  assert(!summary["capture_truncated"].asBool());
  assert(summary["captured_output_bytes"].asUInt64() == full.size());
  assert(summary["output_artifact"]["path"] == data["output_artifact"]["path"]);
  assert(!summary.isMember("output_omitted_from_prompt"));

  data.removeMember("output_artifact");
  data["output_artifact_error"] = "storage unavailable";
  const auto no_store = tool_loop_cap_tool_output_for_prompt(encode_json(input), 0, &truncated, "shell_exec");
  assert(!truncated && no_store.find("output_readback") == std::string::npos);
  assert(no_store.size() > full.size());
}


static void test_saved_fs_list_output_receipt() {
  Json::Value input;
  input["ok"] = true;
  auto& data = input["data"];
  data["tool"] = "fs_list";
  data["path"] = "fixture-directory";
  data["resolved_path"] = "/fixture/directory";
  data["recursive"] = false;
  data["max_entries"] = 500;
  data["include_hidden"] = true;
  data["use_default_excludes"] = false;
  data["entries"] = Json::Value(Json::arrayValue);
  std::string output = "path: fixture-directory\nentries: 160\n";
  for (int i = 0; i < 160; ++i) {
    Json::Value entry;
    entry["path"] = "PRIVATE_ENTRY_" + std::to_string(i) + "_" + std::string(72, 'x');
    entry["type"] = "file";
    entry["size_bytes"] = 1;
    data["entries"].append(entry);
    output += "- " + entry["path"].asString() + " (file) 1 bytes\n";
  }
  data["entries_count"] = 160;
  data["output"] = output;
  data["captured_output_bytes"] = Json::UInt64(output.size());
  data["observed_output_bytes"] = Json::UInt64(output.size());
  data["output_artifact"]["path"] = "out/tool-output-listing";
  data["output_artifact"]["size_bytes"] = Json::UInt64(output.size());
  for (bool partial : {false, true}) {
    data["truncated"] = partial;
    data["capture_truncated"] = partial;
    data["capture_complete"] = !partial;
    data["listing_complete"] = !partial;
    const std::string original = encode_json(input);
    // Reproduce the important case: the full structured result fits the budget.
    for (size_t limit : {size_t(0), size_t(200000), size_t(16000)}) {
      bool truncated = false;
      const auto receipt = tool_loop_cap_tool_output_for_prompt(original, limit, &truncated, "fs_list");
      Json::Value parsed;
      Json::CharReaderBuilder builder;
      std::string errors;
      std::istringstream stream(receipt);
      assert(Json::parseFromStream(builder, stream, &parsed, &errors));
      const auto& result = parsed["data"];
      assert(truncated && receipt.size() < 4096);
      assert(!result.isMember("output") && !result.isMember("entries"));
      assert(receipt.find("PRIVATE_ENTRY_") == std::string::npos);
      assert(result["entries_count"].asUInt64() == 160);
      assert(result["output_omitted_from_prompt"].asBool() && result["entries_omitted_from_prompt"].asBool());
      assert(result["path"] == data["path"] && result["max_entries"] == data["max_entries"]);
      assert(result["include_hidden"].asBool() && !result["use_default_excludes"].asBool());
      assert(result["truncated"].asBool() == partial && result["capture_complete"].asBool() == !partial);
      assert(result["output_readback"]["tool"] == "fs_read");
      assert(result["output_readback"]["arguments"]["path"] == data["output_artifact"]["path"]);
    }
    const auto summary = summarize_tool_output_json(original, "fs_list");
    assert(summary["output"].asString() == output);
    assert(summary["entries_count"].asUInt64() == 160 && summary["listing_complete"].asBool() == !partial);
    assert(summary["capture_truncated"].asBool() == partial);
    assert(!summary["output_display_truncated"].asBool());
    assert(!summary.isMember("entries_omitted_from_prompt"));
  }
  bool truncated = false;
  for (const char* actual_tool : {"web_fetch", "fs_find", ""}) {
    const auto forged = tool_loop_cap_tool_output_for_prompt(encode_json(input), 0, &truncated, actual_tool);
    assert(!truncated && forged.find("PRIVATE_ENTRY_") != std::string::npos);
    assert(forged.find("output_omitted_from_prompt") == std::string::npos);
  }
  // Display clipping is marked even when the captured directory listing is complete.
  data["output"] = std::string(80000, 'L');
  data["truncated"] = false;
  data["capture_truncated"] = false;
  data["capture_complete"] = true;
  data["captured_output_bytes"] = 80000;
  const auto clipped = summarize_tool_output_json(encode_json(input), "fs_list");
  assert(clipped["output"].asString().size() < 80000);
  assert(clipped["output_display_truncated"].asBool() && clipped["output_truncated"].asBool());
  assert(clipped["capture_complete"].asBool() && !clipped["capture_truncated"].asBool());
  assert(clipped["captured_output_bytes"].asUInt64() == 80000);

  data.removeMember("output_artifact");
  data["output_artifact_error"] = "storage unavailable";
  const auto failed = tool_loop_cap_tool_output_for_prompt(encode_json(input), 0, &truncated, "fs_list");
  assert(!truncated && failed.find("PRIVATE_ENTRY_") != std::string::npos);
  assert(failed.find("output_readback") == std::string::npos);
}

int main() {
  test_saved_exec_output_receipt();
  test_saved_fs_list_output_receipt();
  test_plain_text_truncation();
  test_json_output_field_truncation();
  test_json_content_field_truncation();
  test_json_entries_array_truncation();
  test_json_matches_array_truncation();
  test_web_output_summary_provenance();
  test_web_output_summary_bounds();
  return 0;
}
