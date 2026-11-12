#include "toolset_host.h"
#include "toolset_host_web.h"
#include <json/json.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace host_tools_internal;
namespace {
void check(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
BrowserResult next;
BrowserRequest requested;
size_t calls = 0;
BrowserResult fake(const BrowserRequest& request, HostCancelCallback, void*) { ++calls; requested = request; return next; }
Json::Value parse(const std::string& text) {
  Json::CharReaderBuilder builder; std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value; std::string error;
  check(reader->parse(text.data(), text.data() + text.size(), &value, &error), "invalid tool result JSON"); return value;
}
WebDiscoveryResult found;
WebDiscoveryRequest discovery_request;
size_t discovery_calls = 0;
WebDiscoveryResult fake_discovery(const WebDiscoveryRequest& request, HostCancelCallback, void*) { ++discovery_calls; discovery_request = request; return found; }
Json::Value call(const std::string& args, bool search = true) {
  agent_string_t result{};
  std::string input = args;
  if (search && args.find("\"query\"") != std::string::npos && args.find("\"engine\"") == std::string::npos && !args.empty() && args.back() == '}') input.insert(input.size()-1, ",\"engine\":\"bing\"");
  const auto status = search ? web_search_execute(input.c_str(), &result, fake, nullptr, nullptr, fake_discovery) : web_fetch_execute(args.c_str(), &result, fake);
  check(status == AGENT_OK, "tool failed outside JSON envelope");
  auto value = parse(std::string(result.data, result.len)); agent_string_free(&result); return value;
}
void page() {
  next = {}; next.ok = true; next.http_status = 200;
  next.page["url"] = "https://www.bing.com/search?q=x"; next.page["title"] = "Source page";
  next.page["text"] = "Rendered page text"; next.page["content_type"] = "text/html";
  next.page["text_truncated"] = false; next.page["no_results"] = false;
  next.page["search_results"][0]["title"] = "Source";
  next.page["search_results"][0]["url"] = "https://example.org/source";
  next.page["search_results"][0]["snippet"] = "Actual rendered snippet";
  next.page["links"] = next.page["search_results"];
}
bool registered(agent_tool_registry_t* registry, const std::string& name) {
  for (size_t i = 0; i < agent_tool_registry_count(registry); ++i) {
    agent_tool_def_view_t tool{}; if (agent_tool_registry_get(registry, i, &tool) == AGENT_OK && tool.name && name == tool.name) return true;
  }
  return false;
}
}
int main() {
  setenv("AGENT_BROWSER_SEARCH_ENGINE", "bing", 1);
  page(); auto out = call(R"({"query":"x&extra=片","max_results":1})");
  check(out["ok"].asBool() && out["data"]["results"].size() == 1, "rendered search failed");
  check(requested.search && requested.url == "https://www.bing.com/search?q=x%26extra%3D%E7%89%87" && !requested.allow_local_for_tests, "search URL or isolation contract changed");
  check(requested.timeout_ms == 20000, "default browser deadline changed");
  const auto before = calls;
  for (const char* args : {"{}", "[]", "{", R"({"query":""})", R"({"query":" \n"})", R"({"query":false})", R"({"query":"x","query":"y"})", R"({"query":"x\u0000y"})", R"({"query":"x","engine":"custom"})", R"({"query":"x","engine":1})", R"({"query":"x","url":"http://localhost"})", R"({"query":"x","max_results":0})", R"({"query":"x","max_results":11})", R"({"query":"x","max_results":true})", R"({"query":"x","timeout_ms":999})", R"({"query":"x","timeout_ms":30001})"}) check(!call(args)["ok"].asBool(), "invalid search arguments accepted");
  check(!call("{\"query\":\"" + std::string(1025, 'a') + "\"}")["ok"].asBool(), "query byte limit ignored");
  check(!call(std::string(8193, 'x'))["ok"].asBool(), "argument cap ignored");
  check(calls == before, "invalid arguments started browser");
  check(call(R"({"query":"x","engine":"mojeek","timeout_ms":30000})")["ok"].asBool() && requested.url == "https://www.mojeek.com/search?q=x", "allowlisted engine failed");
  next.ok = false; next.blocked = true; next.http_status = 403; next.error = "Access challenge"; next.page["challenge_reason"] = "verification_required";
  out = call(R"({"query":"x"})");
  check(!out["ok"].asBool() && out["data"]["blocked"].asBool() && out["data"]["http_status"].asInt() == 403 && !out["data"].isMember("results"), "blocked result became search success");
  page(); next.page["search_results"] = Json::Value(Json::arrayValue);
  check(!call(R"({"query":"x"})")["ok"].asBool(), "unrecognized result DOM became empty success");
  next.page["no_results"] = true;
  check(call(R"({"query":"x"})")["ok"].asBool(), "explicit empty-results page rejected");
  page(); const auto item = next.page["search_results"][0];
  for (int i = 1; i < 20; ++i) { auto extra = item; extra["url"] = "https://example.org/" + std::to_string(i); next.page["search_results"].append(extra); }
  out = call(R"({"query":"x"})");
  check(out["data"]["results"].size() == 5 && out["data"]["truncated"].asBool(), "search result cap ignored");
  page(); next.page["search_results"][0]["url"] = "file:///etc/passwd";
  check(!call(R"({"query":"x"})")["ok"].asBool(), "nonpublic result links accepted");
  page(); next.page["url"] = "https://example.org/source";
  out = call(R"({"url":"https://example.org/source","max_chars":1000})", false);
  check(out["ok"].asBool() && !requested.search && out["data"]["text"].asString() == "Rendered page text", "rendered fetch failed");
  for (const char* args : {"{}", R"({"url":"file:///etc/passwd"})", R"({"url":"http://127.0.0.1/private"})", R"({"url":"https://user:pass@example.org"})", R"({"url":"https://example.org","max_chars":999})", R"({"url":"https://example.org","max_chars":32001})", R"({"url":"https://example.org","allow_local_for_tests":true})"}) check(!call(args, false)["ok"].asBool(), "invalid fetch arguments accepted");
  next.page["text"] = std::string(999, 'a') + "中suffix";
  out = call(R"({"url":"https://example.org","max_chars":1000})", false);
  check(out["data"]["text"].asString() == std::string(999, 'a') + "中" && out["data"]["truncated"].asBool(), "fetch Unicode cap failed");
  agent_string_t raw{};
  found = {}; found.backend = "searxng"; found.error.clear();
  const auto browser_before = calls;
  check(web_search_execute(R"({"query":"Arch Linux installation guide"})", &raw, fake, nullptr, nullptr, fake_discovery) == AGENT_OK, "discovery dispatch failed");
  out = parse(std::string(raw.data, raw.len)); agent_string_free(&raw);
  check(!out["ok"].asBool() && out["error"].asString().find("AGENT_SEARXNG_URL") != std::string::npos && calls == browser_before && discovery_calls == 1 && discovery_request.timeout_ms == 12000, "missing discovery silently started browser");
  found.configured = found.ok = true; found.error.clear();
  found.results.push_back({"Translation", "https://example.org/translation", "Unrelated English words", {"fixture"}, 1.0});
  check(web_search_execute(R"({"query":"Arch Linux installation guide","fresh":true})", &raw, fake, nullptr, nullptr, fake_discovery) == AGENT_OK, "configured discovery dispatch failed");
  out = parse(std::string(raw.data, raw.len)); agent_string_free(&raw);
  check(out["ok"].asBool() && discovery_request.fresh && out["data"]["quality"] == "low_lexical_overlap" && out["data"]["untrusted_content"].asBool(), "discovery evidence metadata missing");
  page(); next.source = "http"; next.rendered = false; next.cache_hit = true; next.cache_age_ms = 40;
  next.page["url"] = "https://example.org/source";
  out = call(R"({"url":"https://example.org/source","mode":"http","fresh":true})", false);
  check(out["ok"].asBool() && requested.mode == "http" && requested.fresh && !out["data"]["rendered"].asBool() && out["data"]["cache_hit"].asBool() && out["data"]["source"] == "http", "fetch provenance/options lost");
  check(!call(R"({"url":"https://example.org","mode":"invalid"})", false)["ok"].asBool(), "invalid fetch mode accepted");
  check(!call(R"({"url":"https://example.org","fresh":1})", false)["ok"].asBool(), "invalid fresh accepted");
  for (const auto policy : {HostToolsetPolicyMode::Full, HostToolsetPolicyMode::ReadOnly}) {
    HostToolsetConfig config; config.policy = policy;
    agent_tool_registry_t* registry = nullptr; agent_tool_executor_t executor{};
    check(toolset_host_create(config, &registry, &executor) == AGENT_OK, "host registry failed");
    check(registered(registry, "web_search") && registered(registry, "web_fetch"), "native browser tools missing");
    check(registered(registry, "proc_exec") == (policy == HostToolsetPolicyMode::Full), "existing execution policy changed");
    toolset_host_destroy(&executor); agent_tool_registry_destroy(registry);
  }
  std::cout << "native browser tool request, output, bounds and policy checks passed\n";
}
