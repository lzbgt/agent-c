#include "browser_process.h"
#include "browser_dom_scripts.h"

#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace host_tools_internal {
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kFrameLimit = 512 * 1024;
constexpr size_t kProtocolLimit = 4 * 1024 * 1024;
constexpr double kNetworkLimit = 8 * 1024 * 1024;

struct Fd {
  int value = -1;
  Fd() = default;
  explicit Fd(int fd) : value(fd) {}
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  ~Fd() { reset(); }
  void reset(int fd = -1) { if (value >= 0) close(value); value = fd; }
};

std::string env(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

std::string json(const Json::Value& value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  return Json::writeString(writer, value);
}

bool parse(const std::string& text, Json::Value& value) {
  Json::CharReaderBuilder builder;
  builder["failIfExtra"] = true;
  builder["rejectDupKeys"] = true;
  builder["allowComments"] = false;
  builder["stackLimit"] = 64;
  std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  std::string error;
  try { return reader->parse(text.data(), text.data() + text.size(), &value, &error); }
  catch (const Json::Exception&) { return false; }
}

std::string url_part(CURLU* url, CURLUPart part) {
  char* value = nullptr;
  if (curl_url_get(url, part, &value, 0) != CURLUE_OK) return "";
  std::string out(value);
  curl_free(value);
  return out;
}

bool private_host(std::string host) {
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  while (!host.empty() && host.back() == '.') host.pop_back();
  if (host.empty() || host == "localhost") return true;
  for (const std::string suffix : {".localhost", ".local", ".internal", ".lan"}) {
    if (host.size() >= suffix.size() && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
  }
  in_addr v4{};
  if (inet_aton(host.c_str(), &v4)) {
    const auto ip = ntohl(v4.s_addr);
    return (ip >> 24) == 0 || (ip >> 24) == 10 || (ip >> 24) == 127 ||
           (ip >> 16) == 0xa9fe || (ip >> 20) == 0xac1 || (ip >> 16) == 0xc0a8 ||
           (ip >> 22) == 0x191 || (ip >> 24) >= 224;
  }
  if (host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
  in6_addr v6{};
  if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
    if (IN6_IS_ADDR_LOOPBACK(&v6) || IN6_IS_ADDR_UNSPECIFIED(&v6) ||
        IN6_IS_ADDR_LINKLOCAL(&v6) || IN6_IS_ADDR_MULTICAST(&v6) || (v6.s6_addr[0] & 0xfe) == 0xfc) return true;
    if (IN6_IS_ADDR_V4MAPPED(&v6)) {
      char ip[INET_ADDRSTRLEN];
      if (!inet_ntop(AF_INET, &v6.s6_addr[12], ip, sizeof(ip))) return true;
      return private_host(ip);
    }
  }
  return host.find('.') == std::string::npos && host.find(':') == std::string::npos;
}

std::string executable() {
  auto configured = env("AGENT_BROWSER_EXECUTABLE");
  if (!configured.empty()) return configured;
  for (const char* path : {"/usr/bin/google-chrome-stable", "/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser", "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}) {
    if (access(path, X_OK) == 0) return path;
  }
  return "";
}

bool proxy_valid(const std::string& proxy) {
  if (proxy.empty()) return true;
  if (!browser_http_url(proxy, true)) return false;
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
  if (!url || curl_url_set(url.get(), CURLUPART_URL, proxy.c_str(), 0) != CURLUE_OK) return false;
  const auto path = url_part(url.get(), CURLUPART_PATH);
  return (path.empty() || path == "/") && url_part(url.get(), CURLUPART_QUERY).empty() && url_part(url.get(), CURLUPART_FRAGMENT).empty();
}

int elevated_fd(int original) {
  if (original < 0) return -1;
  const int fd = fcntl(original, F_DUPFD_CLOEXEC, 10);
  close(original);
  return fd;
}

#if defined(__linux__)
class Browser {
 public:
  Browser(const BrowserRequest& request, HostCancelCallback cancel, void* context)
      : request_(request), cancel_(cancel), cancel_context_(context),
        deadline_(Clock::now() + std::chrono::milliseconds(request.timeout_ms)) {}
  ~Browser() {
    if (pid_ > 0) {
      // Close only our owned browser; the child PID remains reserved until reap.
      if (command_.value >= 0) {
        std::string close_message = "{\"id\":2147483647,\"method\":\"Browser.close\"}";
        close_message.push_back('\0');
        (void)send(command_.value, close_message.data(), close_message.size(), MSG_NOSIGNAL);
      }
      command_.reset();
      const auto until = Clock::now() + std::chrono::milliseconds(400);
      siginfo_t info{};
      while (Clock::now() < until) {
        if (waitid(P_PID, pid_, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      (void)kill(-pid_, SIGTERM);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      (void)kill(-pid_, SIGKILL);
      int status;
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
    response_.reset();
    if (!profile_.empty()) { std::error_code error; std::filesystem::remove_all(profile_, error); }
  }

  BrowserResult run() {
    if (!browser_http_url(request_.url, request_.allow_local_for_tests)) return failure("Only public HTTP(S) browser URLs are supported", true);
    if (request_.timeout_ms < 1000 || request_.timeout_ms > 30000) return failure("Invalid browser timeout");
    if (!start()) return result_;
    Json::Value reply;
    if (!call("Browser.getVersion", {}, reply)) return result_;
    Json::Value params;
    params["behavior"] = "deny";
    if (!call("Browser.setDownloadBehavior", params, reply)) return result_;
    params.clear(); params["url"] = "about:blank";
    if (!call("Target.createTarget", params, reply) || !reply["targetId"].isString()) return failure("Chrome did not create a browser target");
    params.clear(); params["targetId"] = reply["targetId"]; params["flatten"] = true;
    if (!call("Target.attachToTarget", params, reply) || !reply["sessionId"].isString()) return failure("Chrome did not attach a browser target");
    session_ = reply["sessionId"].asString();
    if (!call("Page.enable", {}, reply)) return result_;
    params.clear(); params["maxTotalBufferSize"] = 65536; params["maxResourceBufferSize"] = 65536;
    if (!call("Network.enable", params, reply)) return result_;
    params.clear(); params["bypass"] = true;
    if (!call("Network.setBypassServiceWorker", params, reply)) return result_;
    params.clear(); params["patterns"][0]["urlPattern"] = "*"; params["patterns"][0]["requestStage"] = "Request";
    if (!call("Fetch.enable", params, reply)) return result_;
    params.clear(); params["url"] = request_.url;
    if (!call("Page.navigate", params, reply)) return result_;
    if (reply.get("errorText", "").asString() == "net::ERR_NETWORK_CHANGED") {
      if (!pump_until(Clock::now() + std::chrono::milliseconds(250)) || !call("Page.navigate", params, reply)) return result_;
    }
    if (reply["errorText"].isString() && !reply["errorText"].asString().empty()) return failure("Browser navigation failed: " + reply["errorText"].asString());
    if (reply.get("isDownload", false).asBool()) return failure("Browser downloads are not supported", true);
    main_frame_ = reply.get("frameId", "").asString();
    auto ready_since = Clock::time_point{};
    while (active()) {
      params.clear(); params["frameId"] = main_frame_; params["worldName"] = "agent-c-read";
      if (!call("Page.createIsolatedWorld", params, reply)) return result_;
      if (!reply["executionContextId"].isInt()) return failure("Chrome did not create an isolated read context");
      params.clear(); params["expression"] = kBrowserSnapshotScript;
      params["contextId"] = reply["executionContextId"]; params["returnByValue"] = true; params["timeout"] = 1000;
      if (!call("Runtime.evaluate", params, reply)) return result_;
      if (reply.isMember("exceptionDetails")) return failure("Browser page extraction failed");
      const auto& page = reply["result"]["value"];
      if (!page.isObject() || !page["url"].isString()) return failure("Invalid browser page snapshot");
      if (!browser_http_url(page["url"].asString(), request_.allow_local_for_tests)) {
        if (page["url"].asString() != "about:blank") return failure("Browser redirected to an unsupported URL", true);
      } else {
        result_.page = page;
        if (page.get("challenge", false).asBool()) return failure("Browser encountered an access challenge or sign-in gate", true);
        const bool ready = page.get("ready", false).asBool();
        if (ready) {
          if (ready_since == Clock::time_point{}) ready_since = Clock::now();
          const bool settled = Clock::now() - ready_since >= std::chrono::milliseconds(700);
          const bool has_results = page["search_results"].isArray() && !page["search_results"].empty();
          const bool empty_results = page.get("no_results", false).asBool();
          if (settled && (!request_.search || has_results || empty_results)) { result_.ok = true; return result_; }
          if (request_.search && Clock::now() - ready_since > std::chrono::seconds(6)) return failure("Rendered search page has no recognized visible results");
        } else ready_since = {};
      }
      if (!pump_until(Clock::now() + std::chrono::milliseconds(150))) return result_;
    }
    return result_;
  }

 private:
  BrowserResult failure(const std::string& message, bool blocked = false) {
    if (result_.error.empty()) result_.error = message;
    result_.blocked = result_.blocked || blocked;
    return result_;
  }
  bool active() {
    if (!result_.error.empty()) return false;
    if (cancel_ && cancel_(cancel_context_)) { result_.cancelled = true; failure("Browser operation cancelled"); return false; }
    if (Clock::now() >= deadline_) { result_.timed_out = true; failure("Browser operation timed out"); return false; }
    return true;
  }
  bool start() {
    const auto path = executable();
    if (path.empty() || path.front() != '/' || access(path.c_str(), X_OK) != 0) { failure("Chrome executable unavailable; configure AGENT_BROWSER_EXECUTABLE"); return false; }
    const auto proxy = env("AGENT_BROWSER_PROXY");
    if (!proxy_valid(proxy)) { failure("AGENT_BROWSER_PROXY must be an HTTP(S) proxy without credentials"); return false; }
    const auto lock_path = "/tmp/agent-c-browser-" + std::to_string(geteuid()) + ".lock";
    lock_.reset(open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    struct stat st{};
    if (lock_.value < 0 || fstat(lock_.value, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 0777) != 0600) { failure("Cannot acquire private browser lock"); return false; }
    while (flock(lock_.value, LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EAGAIN) { failure("Browser lock failed"); return false; }
      if (!active()) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!active()) return false;
    std::array<char, 64> directory{};
    std::strcpy(directory.data(), "/tmp/agent-c-browser-XXXXXX");
    if (!mkdtemp(directory.data())) { failure("Cannot create private browser profile"); return false; }
    profile_ = directory.data();
    int commands[2], responses[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, commands) != 0) { failure("Cannot create browser command pipe"); return false; }
    Fd child_command(elevated_fd(commands[0])); command_.reset(elevated_fd(commands[1]));
    if (pipe2(responses, O_CLOEXEC) != 0) { failure("Cannot create browser response pipe"); return false; }
    response_.reset(elevated_fd(responses[0])); Fd child_response(elevated_fd(responses[1]));
    Fd null(elevated_fd(open("/dev/null", O_RDWR | O_CLOEXEC)));
    if (child_command.value < 0 || command_.value < 0 || response_.value < 0 || child_response.value < 0 || null.value < 0) { failure("Cannot allocate browser descriptors"); return false; }
    for (int fd : {command_.value, response_.value}) {
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) { failure("Cannot configure browser pipes"); return false; }
    }
    std::vector<std::string> args = {path, "--headless=new", "--disable-gpu", "--disable-software-rasterizer",
      "--disable-background-networking", "--disable-extensions", "--disable-sync", "--no-first-run",
      "--no-default-browser-check", "--password-store=basic", "--lang=en-US", "--js-flags=--max-old-space-size=256",
      "--remote-debugging-pipe", "--user-data-dir=" + profile_};
    if (!proxy.empty()) args.push_back("--proxy-server=" + proxy);
    else args.push_back("--no-proxy-server");
    args.push_back("about:blank");
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    const auto private_home = profile_ + "/home";
    std::error_code home_error;
    if (!std::filesystem::create_directory(private_home, home_error) || home_error) { failure("Cannot isolate browser home"); return false; }
    std::vector<std::string> environment = {"HOME=" + private_home, "XDG_CONFIG_HOME=" + private_home + "/.config", "XDG_CACHE_HOME=" + private_home + "/.cache"};
    for (const char* name : {"PATH", "USER", "LOGNAME", "LANG", "LC_ALL", "LC_CTYPE"}) {
      const auto value = env(name);
      if (!value.empty()) environment.emplace_back(std::string(name) + "=" + value);
    }
    std::vector<char*> envp;
    for (auto& value : environment) envp.push_back(value.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child_command.value, 3);
    posix_spawn_file_actions_adddup2(&actions, child_response.value, 4);
    for (int fd : {0, 1, 2}) posix_spawn_file_actions_adddup2(&actions, null.value, fd);
    posix_spawn_file_actions_addclosefrom_np(&actions, 5);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t empty, defaults;
    sigemptyset(&empty); sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE); sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigmask(&attributes, &empty);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const int status = posix_spawn(&pid_, path.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
    if (status) { pid_ = -1; failure("Cannot start isolated Chrome process"); return false; }
    return true;
  }

  int send_request(const std::string& method, const Json::Value& params) {
    Json::Value message;
    const int id = ++next_id_;
    message["id"] = id; message["method"] = method; message["params"] = params.isNull() ? Json::Value(Json::objectValue) : params;
    if (!session_.empty() && method.rfind("Browser.", 0) != 0 && method.rfind("Target.", 0) != 0) message["sessionId"] = session_;
    std::string wire = json(message); wire.push_back('\0');
    if (wire.size() > kFrameLimit) { failure("Browser command exceeds byte limit"); return 0; }
    size_t offset = 0;
    while (offset < wire.size() && active()) {
      const auto sent = send(command_.value, wire.data() + offset, wire.size() - offset, MSG_NOSIGNAL);
      if (sent > 0) offset += static_cast<size_t>(sent);
      else if (sent < 0 && errno == EINTR) continue;
      else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { pollfd fd{command_.value, POLLOUT, 0}; (void)poll(&fd, 1, 25); }
      else { failure("Chrome command pipe closed"); return 0; }
    }
    return offset == wire.size() ? id : 0;
  }

  bool read_frame(Json::Value& message, Clock::time_point until) {
    while (active()) {
      const auto end = buffer_.find('\0');
      if (end != std::string::npos) {
        const auto frame = buffer_.substr(0, end); buffer_.erase(0, end + 1);
        if (frame.empty()) continue;
        if (frame.size() > kFrameLimit || !parse(frame, message) || !message.isObject()) { failure("Invalid Chrome protocol frame"); return false; }
        return true;
      }
      if (Clock::now() >= until) return false;
      pollfd fd{response_.value, POLLIN, 0};
      const int wait = std::min<int>(25, std::max<int>(1, std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count()));
      if (poll(&fd, 1, wait) < 0) { if (errno == EINTR) continue; failure("Browser pipe polling failed"); return false; }
      if (!(fd.revents & (POLLIN | POLLHUP | POLLERR))) continue;
      char chunk[16384];
      const auto count = read(response_.value, chunk, sizeof(chunk));
      if (count > 0) {
        protocol_bytes_ += static_cast<size_t>(count); buffer_.append(chunk, static_cast<size_t>(count));
        if (buffer_.size() > kFrameLimit || protocol_bytes_ > kProtocolLimit) { failure("Browser protocol byte limit exceeded"); return false; }
      } else if (count == 0) { failure("Chrome response pipe closed"); return false; }
      else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { failure("Cannot read Chrome response pipe"); return false; }
    }
    return false;
  }

  void event(const Json::Value& message) {
    if (message.isMember("sessionId") && message["sessionId"].asString() != session_) return;
    const auto method = message.get("method", "").asString();
    const auto& params = message["params"];
    if (method == "Fetch.requestPaused") {
      Json::Value answer; answer["requestId"] = params["requestId"];
      const auto url = params["request"].get("url", "").asString();
      const auto type = params.get("resourceType", "").asString();
      const bool permitted = browser_http_url(url, request_.allow_local_for_tests);
      if (!permitted || type == "Image" || type == "Media" || type == "Font") {
        answer["errorReason"] = "BlockedByClient";
        (void)send_request("Fetch.failRequest", answer);
        if (!permitted && type == "Document") failure("Browser blocked a non-public or non-HTTP navigation", true);
      } else (void)send_request("Fetch.continueRequest", answer);
    } else if (method == "Network.responseReceived" && params.get("type", "").asString() == "Document" &&
               (main_frame_.empty() || params.get("frameId", "").asString() == main_frame_)) {
      result_.http_status = params["response"].get("status", 0).asInt();
      if (result_.http_status >= 400) failure("Browser page returned HTTP " + std::to_string(result_.http_status),
          result_.http_status == 401 || result_.http_status == 403 || result_.http_status == 429);
    } else if (method == "Network.dataReceived") {
      network_bytes_ += params.get("encodedDataLength", 0.0).asDouble();
      if (network_bytes_ > kNetworkLimit) failure("Browser network byte limit exceeded");
    } else if (method == "Page.frameNavigated" && !params["frame"].isMember("parentId")) {
      main_frame_ = params["frame"].get("id", "").asString();
      const auto url = params["frame"].get("url", "").asString();
      if (url != "about:blank" && !browser_http_url(url, request_.allow_local_for_tests)) failure("Browser navigated outside public HTTP(S)", true);
    } else if (method == "Inspector.targetCrashed") failure("Chrome page crashed");
  }
  bool call(const std::string& method, const Json::Value& params, Json::Value& result) {
    const int id = send_request(method, params);
    if (!id) return false;
    Json::Value message;
    while (read_frame(message, deadline_)) {
      if (message.get("id", 0).asInt() == id) {
        if (message.isMember("error")) { failure("Chrome protocol command failed: " + method); return false; }
        result = message["result"]; return active();
      }
      event(message);
    }
    return false;
  }
  bool pump_until(Clock::time_point until) {
    Json::Value message;
    while (read_frame(message, std::min(until, deadline_))) event(message);
    return active();
  }

  BrowserRequest request_;
  HostCancelCallback cancel_;
  void* cancel_context_;
  Clock::time_point deadline_;
  BrowserResult result_;
  Fd lock_, command_, response_;
  pid_t pid_ = -1;
  std::string profile_, session_, main_frame_, buffer_;
  int next_id_ = 0;
  size_t protocol_bytes_ = 0;
  double network_bytes_ = 0;
};
#endif
}  // namespace

bool browser_http_url(const std::string& url, bool allow_local) {
  if (url.empty() || url.size() > 4096) return false;
  for (unsigned char c : url) if (c <= 0x20 || c == 0x7f || c == '\\') return false;
  std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) return false;
  const auto scheme = url_part(parsed.get(), CURLUPART_SCHEME);
  const auto host = url_part(parsed.get(), CURLUPART_HOST);
  char* credential = nullptr;
  const bool has_user = curl_url_get(parsed.get(), CURLUPART_USER, &credential, 0) == CURLUE_OK;
  curl_free(credential); credential = nullptr;
  const bool has_password = curl_url_get(parsed.get(), CURLUPART_PASSWORD, &credential, 0) == CURLUE_OK;
  curl_free(credential);
  return (scheme == "http" || scheme == "https") && !host.empty() && !has_user && !has_password && (allow_local || !private_host(host));
}

BrowserResult browser_read_page(const BrowserRequest& request, HostCancelCallback cancel, void* cancel_context) {
#if defined(__linux__)
  try {
    Browser browser(request, cancel, cancel_context);
    return browser.run();
  } catch (const std::exception&) {
    BrowserResult result; result.error = "Browser process or protocol failure"; return result;
  }
#else
  (void)request; (void)cancel; (void)cancel_context;
  BrowserResult result; result.error = "Native browser tools currently require Linux"; return result;
#endif
}
}  // namespace host_tools_internal
