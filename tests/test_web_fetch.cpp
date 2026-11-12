#include "web_fetch.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace host_tools_internal;
namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int64_t now = 1000;
int64_t clock_ms() { return now; }
std::vector<agentd::HttpClientResult> responses;
std::vector<std::string> urls;
size_t browser_calls = 0;
BrowserRequest rendered;
bool cancelled = false;
bool cancel(void*) { return cancelled; }
agentd::HttpClientResult response(const std::string& body, long status = 200, const std::string& type = "text/html; charset=UTF-8") {
  agentd::HttpClientResult value; value.ok = true; value.http_status = status; value.response_body = body;
  value.response_headers["content-type"] = type; return value;
}
agentd::HttpClientResult transport(const std::string& url, const std::string& method,
    const std::map<std::string,std::string>& headers, const std::string& body,
    int64_t timeout, size_t cap, const std::string&, const agentd::HttpClientPinnedResolve* pinned,
    agentd::HttpClientCancelCallback callback, void* context, bool env) {
  check(method == "GET" && body.empty() && !pinned && !env, "HTTP request method/routing contract");
  check(headers.size() == 3 && headers.at("Accept-Encoding") == "gzip, deflate", "HTTP request headers/auth contract");
  check(timeout > 0 && timeout <= 30000 && cap <= 2 * 1024 * 1024, "HTTP request bound contract");
  if (callback) check(!callback(context), "cancelled request reached transport");
  urls.push_back(url); check(!responses.empty(), "unexpected extra HTTP request");
  auto result = responses.front(); responses.erase(responses.begin()); return result;
}
BrowserResult browser(const BrowserRequest& request, HostCancelCallback, void*) {
  ++browser_calls; rendered = request;
  BrowserResult result; result.ok = true; result.http_status = 200;
  result.page["url"] = request.url; result.page["title"] = "Rendered"; result.page["text"] = "JavaScript content";
  result.page["links"] = Json::Value(Json::arrayValue); return result;
}
BrowserRequest request(const std::string& path) { BrowserRequest value; value.url = "https://example.org/" + path; return value; }
BrowserResult run(BrowserRequest value) { return web_fetch_with_transport(value, transport, browser, cancel, nullptr, clock_ms); }
const std::string article = "<!doctype html><html><head><title> A &amp; B 中文 </title><meta property='article:published_time' content='2026-11-12'></head><body><nav>REMOVE_NAV</nav><main><h1>Read this</h1><p>Primary source 中文 text. This article explains how to read a public document with a bounded native HTTP client. The main body contains substantive evidence, while navigation, hidden elements and scripts are excluded from the extracted page text.</p><script>REMOVE_SCRIPT</script><style>REMOVE_STYLE</style><form>REMOVE_FORM</form><p hidden>REMOVE_HIDDEN</p><a href='/read'>Source</a><a href='file:///etc/passwd'>Private</a></main><footer>REMOVE_FOOTER</footer></body></html>";
}
int main(int argc, char** argv) {
  try {
    if (argc >= 3 && (std::string(argv[1]) == "--live" || std::string(argv[1]) == "--fixture" || std::string(argv[1]) == "--cancel-fixture")) {
      BrowserRequest value; value.url = argv[2]; value.fresh = true; value.allow_local_for_tests = std::string(argv[1]) != "--live";
      const auto start = std::chrono::steady_clock::now();
      const HostCancelCallback callback = [](void* context) { return std::chrono::steady_clock::now() - *static_cast<const std::chrono::steady_clock::time_point*>(context) > std::chrono::milliseconds(100); };
      const bool cancel_fixture = std::string(argv[1]) == "--cancel-fixture";
      const auto result = web_fetch_read_page(value, cancel_fixture ? callback : nullptr, const_cast<std::chrono::steady_clock::time_point*>(&start));
      Json::Value out; out["ok"] = result.ok; out["source"] = result.source; out["rendered"] = result.rendered; out["status"] = result.http_status; out["cancelled"] = result.cancelled; out["error"] = result.error; out["page"] = result.page;
      std::cout << out << '\n'; return (cancel_fixture ? result.cancelled : result.ok) ? 0 : 1;
    }
    unsetenv("AGENT_BROWSER_PROXY");
    responses = {response(article)}; auto result = run(request("static"));
    check(result.ok && result.source == "http" && !result.rendered, "static extraction failed");
    check(result.page["title"].asString() == "A & B 中文", "HTML charset/entities/title");
    check(result.page["text"].asString().find("REMOVE_") == std::string::npos, "excluded content leaked");
    check(result.page["links"].size() == 1 && result.page["links"][0]["url"] == "https://example.org/read", "relative/unsafe links");
    check(result.page["metadata"]["article:published_time"] == "2026-11-12", "explicit metadata missing");
    check(!result.fetched_at.empty() && !result.cache_hit, "provenance missing");
    responses = {response("<body><div hidden><main>HIDDEN_MAIN</main></div><article>Visible article</article></body>")};
    result = run(request("hidden-main")); check(result.page["text"] == "Visible article", "hidden ancestor escaped extraction filter");

    const std::string featured = R"HTML(
      <html><head><title>Example release archive</title></head><body>
      <nav>OMIT_NAVIGATION</nav>
      <aside hidden><article>OMIT_HIDDEN_FEATURE</article></aside>
      <aside id="featured"><article>
        <h1>Current releases</h1>
        <table><tr><th>Channel</th><th>Version</th><th>Release date</th></tr>
          <tr><td><strong>stable:</strong></td><td>9.4.2</td><td>2030-01-04</td></tr>
          <tr><td>longterm:</td><td>8.1.19</td><td>2030-01-02</td></tr>
        </table>
      </article></aside>
      <section><article>Other resources without current release evidence.</article></section>
      <footer>OMIT_FOOTER</footer></body></html>)HTML";
    responses = {response(featured)}; result = run(request("featured-article-table"));
    const auto featured_text = result.page["text"].asString();
    check(result.ok && result.source == "http" && !result.rendered, "featured primary article was not extracted by HTTP");
    check(featured_text.find("stable: 9.4.2 2030-01-04") != std::string::npos &&
          featured_text.find("longterm: 8.1.19 2030-01-02") != std::string::npos,
          "featured primary table lost channel, full version or release date");
    check(featured_text.find("OMIT_") == std::string::npos &&
          featured_text.find("Other resources") == std::string::npos,
          "hidden or unrelated article replaced featured primary evidence");

    const auto count = urls.size(); now += 100;
    result = run(request("static")); check(result.cache_hit && result.cache_age_ms == 100 && urls.size() == count, "success cache failed");
    auto fresh = request("static"); fresh.fresh = true; responses = {response("", 503)};
    check(!run(fresh).ok, "fresh fetch ignored");
    responses = {response(article)}; check(!run(request("static")).cache_hit, "fresh failure left stale success");
    now += 300000; responses = {response(article)}; check(!run(request("static")).cache_hit, "cache TTL ignored");
    auto redirect = response("",302); redirect.response_headers["location"] = "/final";
    responses = {redirect,response(article)}; result = run(request("redirect"));
    check(result.ok && result.page["url"] == "https://example.org/final", "manual redirect failed");
    for (const auto& target : {"http://127.0.0.1/private", "file:///etc/passwd", "https://user:pass@example.org/"}) {
      redirect.response_headers["location"] = target; responses = {redirect}; const auto before = urls.size();
      check(!run(request(std::string("bad-redirect") + std::to_string(before))).ok && urls.size() == before + 1, "unsafe redirect followed");
    }
    redirect.response_headers["location"] = "/loop"; responses = {redirect,redirect,redirect,redirect};
    check(!run(request("loop")).ok && responses.empty(), "redirect bound ignored");
    const auto before_browser = browser_calls;
    for (long status : {401,403,429,500}) { responses = {response("<script>app()</script>",status)}; check(!run(request("denied" + std::to_string(status))).ok, "HTTP error became success"); }
    responses = {response("<html><title>DuckDuckGo</title><body>Unfortunately, bots use DuckDuckGo too.<form id='challenge-form' action='/anomaly.js'>Please complete the following challenge to confirm you are human.</form></body></html>",202)};
    result = run(request("ddg202")); check(!result.ok && result.blocked, "DDG202 challenge not blocked");
    responses = {response("<title>Oh noes!</title><body>Oh noes! Access Denied. Protected by Anubis.</body>")};
    result = run(request("anubis200")); check(!result.ok && result.blocked, "Anubis200 not blocked");
    agentd::HttpClientResult failure; failure.error = "connection denied"; responses = {failure};
    check(!run(request("transport-fail")).ok && browser_calls == before_browser, "failure triggered browser fallback");
    const auto shell = response("<title>App</title><body><main>Loading</main><script src='/app.js'></script></body>");
    responses = {shell}; result = run(request("shell-auto"));
    check(result.ok && result.rendered && browser_calls == before_browser + 1 && rendered.timeout_ms < 20000, "bounded browser fallback failed");
    auto http_only = request("shell-http"); http_only.mode = "http"; responses = {shell}; result = run(http_only);
    check(result.ok && !result.rendered && result.page["needs_rendering"].asBool(), "HTTP-only mode changed");
    auto browser_only = request("browser-only"); browser_only.mode = "browser"; result = run(browser_only);
    check(result.ok && result.rendered && browser_calls == before_browser + 2, "explicit browser mode failed");
    responses = {response("Text 中文",200,"text/plain; charset=utf-8")};
    check(run(request("plain")).page["text"] == "Text 中文", "plain UTF8 failed");
    responses = {response("binary",200,"application/pdf")}; check(!run(request("pdf")).ok, "unsupported content accepted");
    responses = {response(std::string(128000 - 3,'a') + "中",200,"text/plain")}; result = run(request("utf8-boundary"));
    check(result.page["text"].asString().size() == 128000, "complete final UTF8 character lost");
    responses = {response(std::string(128000 - 1,'a') + "中suffix",200,"text/plain")}; result = run(request("utf8-clipped"));
    check(result.page["text"].asString().size() == 127999 && result.page["text_truncated"].asBool(), "UTF8 clipping failed");
    responses = {response(std::string(2*1024*1024+1,'x'))}; check(!run(request("oversize")).ok, "decoded response limit ignored");
    cancelled = true; const auto before_cancel = urls.size(); result = run(request("cancelled"));
    check(result.cancelled && !result.ok && urls.size() == before_cancel, "cancellation not checked before IO"); cancelled = false;
    for (int i = 0; i < 33; ++i) { ++now; responses = {response("bounded",200,"text/plain")}; check(run(request("capacity-" + std::to_string(i))).ok, "cache fixture failed"); }
    responses = {response("evicted",200,"text/plain")}; check(!run(request("capacity-0")).cache_hit, "cache capacity ignored");
    std::cout << "native HTTP extraction, redirect/gate, fallback, cancellation and cache tests passed\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
