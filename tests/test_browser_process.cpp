#include "browser_process.h"

#include <json/json.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

using host_tools_internal::BrowserRequest;
using host_tools_internal::browser_read_page;
using Clock = std::chrono::steady_clock;

namespace {
void check(bool value, const char* message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
std::string json(const Json::Value& value) { Json::StreamWriterBuilder writer; writer["indentation"] = ""; return Json::writeString(writer, value); }
void send_frame(Json::Value value) {
  auto text = json(value); text.push_back('\0');
  size_t sent = 0;
  while (sent < text.size()) { const auto count = write(4, text.data() + sent, text.size() - sent); if (count <= 0) return; sent += count; }
}

int fake_browser(int argc, char** argv) {
  std::string profile, input, url;
  bool safe_flags = false, unsafe = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--disable-gpu") safe_flags = true;
    if (arg == "--no-sandbox" || arg.find("--remote-debugging-port") == 0) unsafe = true;
    if (arg.find("--user-data-dir=") == 0) profile = arg.substr(16);
  }
  for (;;) {
    char buffer[4096]; const auto count = read(3, buffer, sizeof(buffer));
    if (count <= 0) break;
    input.append(buffer, static_cast<size_t>(count));
    for (auto end = input.find('\0'); end != std::string::npos; end = input.find('\0')) {
      const auto text = input.substr(0, end); input.erase(0, end + 1);
      Json::CharReaderBuilder builder; std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
      Json::Value request; std::string error;
      if (!reader->parse(text.data(), text.data() + text.size(), &request, &error)) return 2;
      const auto method = request.get("method", "").asString();
      if (method == "Browser.close") return 0;
      Json::Value reply; reply["id"] = request["id"]; reply["result"] = Json::Value(Json::objectValue);
      if (request.isMember("sessionId")) reply["sessionId"] = request["sessionId"];
      if (method == "Target.createTarget") reply["result"]["targetId"] = "fixture-target";
      if (method == "Target.attachToTarget") reply["result"]["sessionId"] = "fixture-session";
      if (method == "Page.navigate") {
        url = request["params"]["url"].asString();
        reply["result"]["frameId"] = "fixture-frame";
        if (url.find("/forbidden") != std::string::npos) {
          Json::Value event;
          event["method"] = "Network.responseReceived"; event["sessionId"] = "fixture-session";
          event["params"]["type"] = "Document"; event["params"]["frameId"] = "fixture-frame"; event["params"]["response"]["status"] = 403;
          send_frame(event);
        }
        if (url.find("/file-redirect") != std::string::npos) {
          Json::Value event;
          event["method"] = "Fetch.requestPaused"; event["sessionId"] = "fixture-session";
          event["params"]["requestId"] = "blocked-file"; event["params"]["resourceType"] = "Document"; event["params"]["request"]["url"] = "file:///etc/passwd";
          send_frame(event);
        }
      }
      if (method == "Page.createIsolatedWorld") reply["result"]["executionContextId"] = 9;
      if (method == "Runtime.evaluate") {
        if (url.find("/slow") != std::string::npos) continue;
        if (url.find("/oversized") != std::string::npos) {
          std::string large(512 * 1024 + 1, 'x'); large.push_back('\0');
          size_t sent = 0;
          while (sent < large.size()) { const auto count = write(4, large.data() + sent, large.size() - sent); if (count <= 0) return 0; sent += count; }
          continue;
        }
        auto& page = reply["result"]["result"]["value"];
        page["url"] = url; page["title"] = "Fixture"; page["ready"] = true;
        page["content_type"] = "text/html"; page["text"] = "Rendered fixture text"; page["text_length"] = 21;
        page["challenge"] = url.find("/challenge") != std::string::npos;
        page["challenge_reason"] = "fixture_gate";
        page["search_results"][0]["title"] = "Fixture source";
        page["search_results"][0]["url"] = "https://example.org/source";
        page["search_results"][0]["snippet"] = "Fixture snippet";
        page["no_results"] = false;
        page["test_profile"] = profile;
        page["test_safe_flags"] = safe_flags && !unsafe;
        page["test_has_secret"] = std::getenv("OPENAI_API_KEY") != nullptr || std::getenv("AGENT_BROWSER_TEST_SECRET") != nullptr;
        page["test_private_home"] = std::getenv("HOME") && std::string(std::getenv("HOME")) == profile + "/home";
      }
      send_frame(reply);
    }
  }
  return 0;
}

bool cancel_after(void* context) { return Clock::now() - *static_cast<Clock::time_point*>(context) >= std::chrono::milliseconds(100); }
}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--headless=new") return fake_browser(argc, argv);
  if (argc > 2 && (std::string(argv[1]) == "--live" || std::string(argv[1]) == "--fixture" || std::string(argv[1]) == "--live-cancel")) {
    BrowserRequest request; request.url = argv[2]; request.search = argc > 3 && std::string(argv[3]) == "search";
    request.allow_local_for_tests = std::string(argv[1]) == "--fixture";
    auto start = Clock::now();
    const auto result = browser_read_page(request, std::string(argv[1]) == "--live-cancel" ? cancel_after : nullptr, &start);
    Json::Value out; out["ok"] = result.ok; out["error"] = result.error; out["blocked"] = result.blocked;
    out["cancelled"] = result.cancelled; out["timed_out"] = result.timed_out; out["http_status"] = result.http_status; out["page"] = result.page;
    out["elapsed_ms"] = Json::Int64(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    std::cout << json(out) << '\n'; return result.ok ? 0 : 1;
  }
#if !defined(__linux__)
  std::cout << "Native browser process fixtures require Linux\n"; return 0;
#else
  const auto self = std::filesystem::canonical(argv[0]).string();
  setenv("AGENT_BROWSER_EXECUTABLE", self.c_str(), 1);
  setenv("AGENT_BROWSER_PROXY", "", 1);
  setenv("OPENAI_API_KEY", "fixture-secret-never-inherit", 1);
  setenv("AGENT_BROWSER_TEST_SECRET", "fixture-private", 1);
  for (const std::string url : {"file:///etc/passwd", "data:text/html,private", "http://localhost/a", "http://127.1/a", "http://2130706433/a", "http://10.0.0.1/a", "http://[::1]/a", "http://[::ffff:127.0.0.1]/a", "https://user:pass@example.org/a"}) {
    check(!host_tools_internal::browser_http_url(url), "unsafe browser URL accepted");
  }
  check(host_tools_internal::browser_http_url("https://example.org/page?q=x"), "public HTTP URL rejected");
  BrowserRequest request; request.url = "https://fixture.example/ok"; request.search = true;
  auto result = browser_read_page(request, nullptr, nullptr);
  check(result.ok && result.page["test_safe_flags"].asBool(), "owned browser fixture failed");
  check(!result.page["test_has_secret"].asBool(), "provider credentials leaked into Chrome environment");
  check(result.page["test_private_home"].asBool(), "browser inherited the owner's home directory");
  check(!std::filesystem::exists(result.page["test_profile"].asString()), "private profile survived cleanup");
  for (const std::string path : {"/forbidden", "/file-redirect", "/challenge"}) {
    request.url = "https://fixture.example" + path;
    result = browser_read_page(request, nullptr, nullptr);
    check(!result.ok && result.blocked, "denial or unsupported navigation was not blocked");
  }
  request.url = "https://fixture.example/oversized";
  result = browser_read_page(request, nullptr, nullptr);
  check(!result.ok && result.error.find("limit") != std::string::npos, "CDP byte cap ignored");
  request.url = "https://fixture.example/slow"; request.timeout_ms = 1000;
  auto start = Clock::now(); result = browser_read_page(request, nullptr, nullptr);
  check(!result.ok && result.timed_out && Clock::now() - start < std::chrono::seconds(3), "browser deadline or cleanup exceeded bound");
  request.timeout_ms = 20000;
  start = Clock::now(); result = browser_read_page(request, cancel_after, &start);
  check(!result.ok && result.cancelled && Clock::now() - start < std::chrono::seconds(3), "browser cancellation failed");
  std::cout << "browser process, isolated environment, protocol, bounds, cancellation and cleanup checks passed\n";
#endif
}
