#include "web_discovery.h"

#include <json/json.h>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace host_tools_internal;
namespace {
agentd::HttpClientResult next_response;
int calls = 0;
int64_t fake_time = 1000000;
std::string observed_url;
bool expect_cancel = false;
int64_t now_ms() { return fake_time; }
void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
std::string json(const Json::Value& value) {
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}
agentd::HttpClientResult fake_http(
    const std::string& url, const std::string& method,
    const std::map<std::string, std::string>& headers, const std::string& body,
    int64_t timeout_ms, size_t max_response_bytes, const std::string& proxy,
    const agentd::HttpClientPinnedResolve* pinned, agentd::HttpClientCancelCallback cancel,
    void* context, bool use_env_proxy) {
  ++calls;
  observed_url = url;
  check(method == "GET" && body.empty(), "discovery is not a fixed GET");
  check(proxy.empty() && !use_env_proxy && pinned == nullptr, "loopback request can inherit a proxy");
  check(timeout_ms >= 1000 && timeout_ms <= 30000 && max_response_bytes == 1024 * 1024,
        "transport bounds missing");
  check(headers.at("Accept") == "application/json", "JSON media type not requested");
  check(headers.at("Accept-Encoding") == "gzip, deflate", "decoded-body limit not enabled");
  check(headers.count("Authorization") == 0 && headers.count("Cookie") == 0, "credentials forwarded");
  if (expect_cancel) check(cancel && context, "cancellation was not passed to transport");
  return next_response;
}
struct Environment {
  std::string name;
  std::string previous;
  bool had_value = false;
  explicit Environment(const char* key) : name(key) {
    if (const char* value = std::getenv(key)) { previous = value; had_value = true; }
  }
  ~Environment() {
    if (had_value) setenv(name.c_str(), previous.c_str(), 1);
    else unsetenv(name.c_str());
  }
};
Json::Value sample() {
  Json::Value root;
  root["query"] = "fixture";
  root["results"] = Json::Value(Json::arrayValue);
  root["unresponsive_engines"] = Json::Value(Json::arrayValue);
  Json::Value item;
  item["title"] = "<b>Architecture &amp; Safety</b><script>not evidence</script>";
  item["url"] = "https://Example.org:443/a#first";
  item["content"] = "Alpha <i>beta</i> gamma";
  item["engine"] = "one";
  item["score"] = 2.0;
  root["results"].append(item);
  item["url"] = "https://example.org/a#second";
  item["engine"] = "two";
  root["results"].append(item);
  item["url"] = "https://example.net/b";
  item["title"] = "Second ranked source";
  item["engine"] = "one";
  item["score"] = 1.0;
  root["results"].append(item);
  return root;
}
void respond(const Json::Value& body, long status = 200) {
  next_response = {};
  next_response.ok = true;
  next_response.http_status = status;
  next_response.response_headers["content-type"] = "application/json; charset=utf-8";
  next_response.response_body = json(body);
}
WebDiscoveryResult run(const std::string& query, bool fresh = true) {
  WebDiscoveryRequest request;
  request.query = query;
  request.fresh = fresh;
  return web_discover_with_transport(request, fake_http, nullptr, nullptr, now_ms);
}
bool cancel_now(void*) { return true; }
bool never_cancel(void*) { return false; }
}  // namespace

int main() {
  try {
    Environment restore("AGENT_SEARXNG_URL");
    unsetenv("AGENT_SEARXNG_URL");
    check(!run("absent").configured && calls == 0, "absent backend made a request");
    for (const char* invalid : {
        "https://example.org:443", "http://localhost:8080", "http://10.0.0.1:8080",
        "http://0.0.0.0:8080", "http://127.0.0.1", "http://127.0.0.1:8080/prefix",
        "http://127.0.0.1:8080?q=x", "http://127.0.0.1:8080#x",
        "http://user:secret@127.0.0.1:8080", "file:///tmp/search"}) {
      setenv("AGENT_SEARXNG_URL", invalid, 1);
      const auto result = run("invalid endpoint");
      check(result.configured && !result.ok && calls == 0, "unsafe endpoint accepted");
    }
    setenv("AGENT_SEARXNG_URL", "http://127.0.0.1:18091", 1);
    for (const auto& query : {std::string(), std::string("   "), std::string("a\nb"),
                              std::string(1025, 'x'), std::string("\xc0\xaf", 2)}) {
      check(!run(query).ok && calls == 0, "invalid query reached transport");
    }
    WebDiscoveryRequest invalid_request{"valid", 0, 12000, true};
    check(!web_discover_with_transport(invalid_request, fake_http, nullptr, nullptr, now_ms).ok,
          "zero result limit accepted");
    invalid_request.max_results = 5; invalid_request.timeout_ms = 999;
    check(!web_discover_with_transport(invalid_request, fake_http, nullptr, nullptr, now_ms).ok && calls == 0,
          "invalid timeout reached transport");

    respond(sample());
    auto result = run("a &format=html 中文");
    check(result.ok && !result.partial && result.results.size() == 2, "valid results failed");
    check(result.results[0].url == "https://example.org/a" && result.results[0].engines.size() == 2,
          "URL canonicalization or ranked duplicate merge failed");
    check(result.results[0].title == "Architecture & Safety" &&
          result.results[0].snippet == "Alpha beta gamma", "markup was not converted to plain evidence");
    check(!result.fetched_at.empty() && result.quality == "unassessed", "evidence metadata missing");
    check(observed_url.find("/search?format=json&q=a%20%26format%3Dhtml%20") != std::string::npos,
          "query was not safely encoded");
    setenv("AGENT_SEARXNG_URL", "http://[::1]:18091/", 1);
    check(run("IPv6 endpoint").ok && observed_url.find("http://[::1]:18091/search?") == 0,
          "literal IPv6 loopback endpoint failed");
    setenv("AGENT_SEARXNG_URL", "http://127.0.0.1:18091", 1);

    auto body = sample();
    body["results"][0]["title"] = "&amp;lt;script&amp;gt;";
    body["results"][0]["content"] = std::string(1100, 'a');
    result = run("stale fixture"); // Ensure changing fixture never affects prior fetched results.
    respond(body);
    result = run("decode once");
    check(result.ok && result.results[0].title == "&lt;script&gt;" &&
          result.results[0].snippet.size() == 1000, "entity decoding or snippet bound failed");
    std::string chinese;
    for (int i = 0; i < 400; ++i) chinese += "中";
    body["results"][0]["title"] = chinese; body["results"][0]["content"] = chinese;
    respond(body);
    result = run("UTF8 bounds");
    check(result.ok && result.results[0].title.size() == 510 && result.results[0].snippet.size() == 999,
          "UTF8 truncation split a character");

    body = sample();
    Json::Value bad;
    bad["title"] = "private"; bad["url"] = "http://127.0.0.1/private";
    body["results"].append(bad);
    bad["url"] = "https://user:secret@example.org/private"; body["results"].append(bad);
    bad["url"] = "https://example.org/" + std::string(4096, 'x'); body["results"].append(bad);
    respond(body);
    result = run("unsafe result links");
    check(result.ok && result.partial && result.skipped_results == 3 && result.results.size() == 2,
          "unsafe links were accepted or silently ignored");

    body = sample();
    Json::Value failure(Json::arrayValue);
    failure.append("brave"); failure.append("too many requests");
    body["unresponsive_engines"].append(failure);
    respond(body);
    result = run("partial");
    check(result.ok && result.partial && !result.blocked && !result.warning.empty() &&
          result.engine_failures.size() == 1, "partial useful coverage failed");
    body["results"] = Json::Value(Json::arrayValue);
    respond(body);
    result = run("blocked engines");
    check(!result.ok && result.blocked && !result.empty, "failed engines became genuine empty results");
    body["unresponsive_engines"] = Json::Value(Json::arrayValue);
    respond(body);
    result = run("genuine empty");
    check(result.ok && result.empty && !result.partial && result.results.empty(), "genuine empty results failed");

    body["results"].append(bad);
    respond(body);
    check(!run("all invalid").ok, "all-invalid results became empty success");
    body = sample(); body["unresponsive_engines"].append("malformed");
    respond(body);
    check(!run("bad engine metadata").ok, "malformed engine metadata accepted");
    Json::Value error; error["error"] = "Service error";
    respond(error);
    check(!run("service error").ok, "200 error envelope accepted");

    for (const auto& malformed : {std::string("{} trailing"), std::string("{\"results\":[],\"results\":[]}"),
                                  std::string("[]"), std::string("{")}) {
      respond(sample()); next_response.response_body = malformed;
      check(!run("bad JSON").ok, "malformed JSON accepted");
    }
    respond(sample()); next_response.response_body.assign(1024 * 1024 + 1, ' ');
    check(!run("oversized JSON").ok, "oversized body accepted");
    respond(sample()); next_response.response_headers["content-type"] = "text/html";
    check(!run("HTML not JSON").ok, "HTML became JSON success");
    for (long status : {401L, 403L, 429L}) {
      respond(sample(), status); next_response.ok = false;
      result = run("denied");
      check(!result.ok && result.blocked && result.http_status == status, "denied HTTP status lost");
    }
    respond(sample(), 302);
    check(!run("redirect").ok, "HTTP redirect accepted");
    respond(sample()); next_response.ok = false; next_response.timed_out = true;
    result = run("timeout");
    check(!result.ok && result.timed_out, "timeout lost");
    respond(sample()); next_response.ok = false; next_response.response_too_large = true;
    check(!run("body cap").ok, "transport body cap lost");

    WebDiscoveryRequest request{"cancel", 5, 12000, true};
    const int before_cancel = calls;
    result = web_discover_with_transport(request, fake_http, cancel_now, nullptr, now_ms);
    check(!result.ok && result.cancelled && calls == before_cancel, "cancelled request was dispatched");
    int context = 1;
    expect_cancel = true;
    next_response = {}; next_response.cancelled = true;
    result = web_discover_with_transport(request, fake_http, never_cancel, &context, now_ms);
    check(!result.ok && result.cancelled, "transport cancellation lost");
    expect_cancel = false;

    respond(sample());
    int before = calls;
    result = run("cache", false);
    check(result.ok && !result.cache_hit && calls == before + 1, "initial cache fetch failed");
    fake_time += 100;
    result = run("cache", false);
    check(result.ok && result.cache_hit && result.cache_age_ms == 100 && calls == before + 1, "cache hit failed");
    result = run("cache", true);
    check(result.ok && !result.cache_hit && calls == before + 2, "fresh did not bypass cache");
    fake_time += 300000;
    result = run("cache", false);
    check(result.ok && !result.cache_hit && calls == before + 3, "expired cache was reused");
    body = sample(); body["unresponsive_engines"].append(failure);
    respond(body);
    result = run("partial cache", false);
    before = calls;
    fake_time += 30000;
    check(!run("partial cache", false).cache_hit && calls == before + 1, "partial coverage cache lived too long");

    respond(sample());
    run("refresh failure", false);
    respond(sample(), 403); next_response.ok = false;
    result = run("refresh failure", true);
    check(!result.ok && !result.cache_hit && result.results.empty(), "fresh failure returned cached success");
    before = calls;
    result = run("refresh failure", false);
    check(!result.ok && calls == before + 1, "failed refresh left stale success cached");

    respond(sample());
    for (int n = 0; n < 65; ++n) {
      fake_time += 1;
      check(run("capacity " + std::to_string(n), false).ok, "capacity fill failed");
    }
    before = calls;
    check(!run("capacity 0", false).cache_hit && calls == before + 1, "bounded cache did not evict oldest entry");
    std::cout << "web discovery endpoint, evidence, failure, cancellation and cache checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "web discovery test failed: " << error.what() << "\n";
    return 1;
  }
}
