#include "toolset_host.h"
#include "toolset_host_web.h"

#include <json/json.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

agentd::HttpClientResult next_response;
size_t requests = 0;
std::string requested_url;

agentd::HttpClientResult fake_request(
    const std::string& url, const std::string& method,
    const std::map<std::string, std::string>& headers, const std::string& body,
    int64_t timeout_ms, size_t max_bytes, const std::string& proxy,
    const agentd::HttpClientPinnedResolve* pinned) {
  ++requests;
  requested_url = url;
  check(method == "GET" && body.empty(), "search must be a bodyless GET");
  check(headers.size() == 2 && headers.at("Accept") == "application/json" && headers.at("User-Agent") == "AgentC/1.0", "search headers must not contain provider authorization or cookies");
  check(timeout_ms == 15000 && max_bytes == 256 * 1024, "search transport must stay bounded");
  check(proxy.empty() && !pinned, "search must use native environment proxy handling");
  check(url.rfind("https://api.mwmbl.org/api/v2/search/?q=", 0) == 0, "fixed search endpoint changed");
  return next_response;
}

std::string json(const Json::Value& value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}
Json::Value parse(const std::string& text) {
  Json::CharReaderBuilder builder;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value out;
  std::string error;
  check(reader->parse(text.data(), text.data() + text.size(), &out, &error), "invalid result JSON");
  return out;
}
Json::Value call(const std::string& args = R"({"query":"x"})") {
  agent_string_t result{};
  check(host_tools_internal::web_search_execute(args.c_str(), &result, fake_request) == AGENT_OK, "tool call failed outside JSON envelope");
  const auto out = parse(std::string(result.data, result.len));
  agent_string_free(&result);
  return out;
}
Json::Value item(const std::string& title = "Example & source", const std::string& url = "https://example.org/page?a=1&b=2", const std::string& snippet = "Result snippet") {
  Json::Value out;
  out["title"] = title;
  out["url"] = url;
  out["content"] = snippet;
  return out;
}
void feed(const std::vector<Json::Value>& items = {item()}, const std::string& query = "x") {
  Json::Value value;
  value["query"] = query;
  value["results"] = Json::Value(Json::arrayValue);
  for (const auto& entry : items) value["results"].append(entry);
  next_response.response_body = json(value);
}
bool registered(agent_tool_registry_t* registry, const std::string& name) {
  for (size_t i = 0; i < agent_tool_registry_count(registry); ++i) {
    agent_tool_def_view_t tool{};
    if (agent_tool_registry_get(registry, i, &tool) == AGENT_OK && tool.name && name == tool.name) return true;
  }
  return false;
}
}  // namespace

int main() {
  next_response.ok = true;
  next_response.http_status = 200;
  feed({item("工程 & 🤖")}, "x&format=html#片\r\n");
  auto out = call(R"({"query":"x&format=html#片\r\n","max_results":1})");
  check(out["ok"].asBool() && out["data"]["results"].size() == 1, "normal search result failed");
  check(requested_url == "https://api.mwmbl.org/api/v2/search/?q=x%26format%3Dhtml%23%E7%89%87%0D%0A", "query must be fully percent-encoded");
  check(out["data"]["results"][0]["url"].asString() == "https://example.org/page?a=1&b=2", "source URL changed");
  check(out["data"]["results"][0]["title"].asString() == "工程 & 🤖", "Unicode title changed");

  const size_t before_invalid = requests;
  for (const auto& args : {"{}", "[]", "{", R"({"query":""})", R"({"query":" \t\n"})", R"({"query":false})", R"({"query":"x","max_results":0})", R"({"query":"x","max_results":11})", R"({"query":"x","max_results":1.5})", R"({"query":"x","max_results":true})", R"({"query":"x","url":"http://localhost"})", R"({"query":"x","query":"y"})", R"({"query":"x\u0000y"})"}) {
    check(!call(args)["ok"].asBool(), "invalid arguments accepted");
  }
  check(!call("{\"query\":\"" + std::string(1025, 'x') + "\"}")["ok"].asBool(), "query byte limit ignored");
  check(!call(std::string(8193, 'x'))["ok"].asBool(), "argument byte limit ignored");
  check(!call(std::string("{\"query\":\"") + char(0x80) + "\"}")["ok"].asBool(), "invalid query UTF-8 accepted");
  check(requests == before_invalid, "invalid arguments reached HTTP transport");
  feed({item()}, std::string(1024, 'x'));
  check(call("{\"query\":\"" + std::string(1024, 'x') + "\"}")["ok"].asBool(), "1024-byte query rejected");
  feed();

  for (long status : {302, 429, 500}) {
    next_response.http_status = status;
    check(!call()["ok"].asBool(), "non-200 response treated as success");
  }
  next_response.http_status = 200;
  next_response.ok = false;
  next_response.error = "sensitive transport details";
  out = call();
  check(!out["ok"].asBool() && json(out).find(next_response.error) == std::string::npos, "transport failure leaked details");
  next_response.ok = true;
  next_response.timed_out = true;
  check(!call()["ok"].asBool(), "timeout treated as success");
  next_response.timed_out = false;
  next_response.response_too_large = true;
  check(!call()["ok"].asBool(), "response cap ignored");
  next_response.response_too_large = false;
  for (const auto& body : std::vector<std::string>{
      std::string(256 * 1024 + 1, 'x'), "<html>Verify you are human</html>", "", "[]", "{}",
      R"({"query":"other","results":[]})", R"({"query":"x","results":{}})",
      R"({"query":"x","results":[],"query":"x"})", R"({"query":"x","results":[]} garbage)",
      std::string("{\"query\":\"x\",\"results\":[],\"deep\":") + std::string(50, '[') + "0" + std::string(50, ']') + "}"}) {
    next_response.response_body = body;
    check(!call()["ok"].asBool(), "invalid or oversized source JSON treated as success");
  }
  feed({});
  out = call();
  check(out["ok"].asBool() && out["data"]["results"].empty(), "valid empty result must be distinct from error");
  feed();
  next_response.response_body.resize(256 * 1024, ' ');
  check(call()["ok"].asBool(), "exact response byte limit rejected");

  for (const auto& url : std::vector<std::string>{"", "javascript:alert(1)", "file:///tmp/file", "data:text/plain,test", "https:///", "https://user:pass@example.com/", "https://example.com:65536/", "https://example.com/a b", "https://example.com/a\nb", "https://example.com\\@other/", "https://example.com/" + std::string(4096, 'a')}) {
    feed({item("Bad", url), item("Good")});
    out = call();
    check(out["ok"].asBool() && out["data"]["results"].size() == 1 && out["data"]["skipped_items"].asUInt() == 1, "invalid source URL not skipped");
    check(out["data"]["results"][0]["title"].asString() == "Good", "wrong result retained");
  }
  feed({Json::Value(5), item(" ")});
  out = call();
  check(out["ok"].asBool() && out["data"]["results"].empty() && out["data"]["skipped_items"].asUInt() == 2, "malformed items not reported");
  next_response.response_body = std::string(R"({"query":"x","results":[{"title":")") + char(0x80) + R"(","url":"https://example.com"}]})";
  out = call();
  check(!out["ok"].asBool(), "invalid source UTF-8 accepted");
  feed({item(std::string(511, 'a') + "中tail", "https://example.com", std::string(999, 'b') + "🤖tail")});
  out = call();
  check(out["ok"].asBool() && out["data"]["truncated"].asBool(), "field clipping not reported");
  check(out["data"]["results"][0]["title"].asString() == std::string(511, 'a') && out["data"]["results"][0]["snippet"].asString() == std::string(999, 'b'), "UTF-8 clipping broke a character");
  feed(std::vector<Json::Value>(20, item()));
  out = call();
  check(out["data"]["results"].size() == 5 && out["data"]["truncated"].asBool(), "default result cap ignored");
  check(call(R"({"query":"x","max_results":10})")["data"]["results"].size() == 10, "explicit result cap ignored");
  feed(std::vector<Json::Value>(101, Json::Value(5)));
  out = call();
  check(out["data"]["skipped_items"].asUInt() == 100 && out["data"]["truncated"].asBool(), "candidate processing limit ignored");

  for (const auto policy : {HostToolsetPolicyMode::Full, HostToolsetPolicyMode::ReadOnly}) {
    HostToolsetConfig config;
    config.policy = policy;
    agent_tool_registry_t* registry = nullptr;
    agent_tool_executor_t executor{};
    check(toolset_host_create(config, &registry, &executor) == AGENT_OK, "host toolset failed");
    check(registered(registry, "web_search"), "native web_search missing from host registry");
    check(registered(registry, "proc_exec") == (policy == HostToolsetPolicyMode::Full), "existing execution policy changed");
    agent_string_t result{};
    check(executor.execute(executor.ctx, "web_search", "{}", &result) == AGENT_OK, "registry dispatch failed");
    check(!parse(std::string(result.data, result.len))["ok"].asBool(), "invalid registry call should fail without network");
    agent_string_free(&result);
    toolset_host_destroy(&executor);
    agent_tool_registry_destroy(registry);
  }
  std::cout << "web_search request, JSON validation, bounds, authentication isolation and registry tests passed\n";
}
