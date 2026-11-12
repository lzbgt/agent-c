#include "openai_client.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

static void require(bool value, const char* message) {
  if (!value) {
    std::cerr << "FAILED: " << message << "\n";
    std::abort();
  }
}

static long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

static bool wait_flag(const std::atomic<bool>& flag, int timeout_ms) {
  const auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!flag.load() && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return flag.load();
}

// A bounded local fake provider: never contacts a model or a public endpoint.
class LoopbackProvider {
 public:
  enum class Mode { Blocked, Streaming, Retry };
  std::atomic<bool> ready{false};
  std::atomic<bool> disconnected{false};
  std::atomic<int> requests{0};

  explicit LoopbackProvider(Mode mode) : mode_(mode) {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    require(listener_ >= 0, "loopback socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "loopback bind");
    socklen_t size = sizeof(addr);
    require(getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &size) == 0, "loopback port");
    port_ = ntohs(addr.sin_port);
    require(listen(listener_, 4) == 0, "loopback listen");
    worker_ = std::thread([this]() { serve(); });
  }

  ~LoopbackProvider() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
    close(listener_);
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

 private:
  bool send_all(int fd, const std::string& text) {
    size_t sent = 0;
    while (sent < text.size() && !stop_.load()) {
      const auto n = send(fd, text.data() + sent, text.size() - sent, 0);
      if (n <= 0) return false;
      sent += static_cast<size_t>(n);
    }
    return sent == text.size();
  }

  void serve() {
    while (!stop_.load()) {
      pollfd listening{listener_, POLLIN, 0};
      if (poll(&listening, 1, 25) <= 0) continue;
      const int fd = accept(listener_, nullptr, nullptr);
      if (fd < 0) continue;
      requests.fetch_add(1);
      std::string request;
      char buffer[4096];
      while (!stop_.load() && request.find("\r\n\r\n") == std::string::npos) {
        pollfd incoming{fd, POLLIN, 0};
        if (poll(&incoming, 1, 25) <= 0) continue;
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        request.append(buffer, static_cast<size_t>(n));
      }
      if (mode_ == Mode::Retry) {
        send_all(fd, "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nRetry-After: 5\r\nConnection: close\r\n\r\n");
        ready.store(true);
        close(fd);
        continue;
      }
      if (mode_ == Mode::Streaming) {
        // Two SSE events in one body write exercise cancellation inside a chunk.
        send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n"
                     "data: {\"choices\":[{\"delta\":{\"content\":\"first\"}}]}\n\n"
                     "data: {\"choices\":[{\"delta\":{\"content\":\"second\"}}]}\n\n");
      }
      ready.store(true);
      while (!stop_.load()) {
        pollfd incoming{fd, POLLIN, 0};
        if (poll(&incoming, 1, 25) <= 0) continue;
        const auto n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
          disconnected.store(true);
          break;
        }
      }
      close(fd);
    }
  }

  Mode mode_;
  int listener_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

static OpenAIClientConfig config_for(const LoopbackProvider& server, std::atomic<bool>* cancelled) {
  OpenAIClientConfig cfg;
  cfg.base_url = server.url() + "/v1";
  cfg.model = "fake";
  cfg.timeout_ms = 6000;
  cfg.connect_timeout_ms = 3000;
  cfg.max_retries = 2;
  cfg.retry_base_ms = 3000;
  cfg.retry_max_ms = 5000;
  cfg.retry_jitter = 0;
  cfg.should_cancel = [](void* ctx) { return static_cast<std::atomic<bool>*>(ctx)->load(); };
  cfg.should_cancel_ctx = cancelled;
  return cfg;
}

enum class RequestKind { Post, Stream, Get };

static void test_cancel_blocked(RequestKind kind, bool use_proxy = false) {
  LoopbackProvider server(LoopbackProvider::Mode::Blocked);
  std::atomic<bool> cancelled{false};
  std::atomic<long long> cancelled_at{0};
  auto cfg = config_for(server, &cancelled);
  if (use_proxy) cfg.proxy_url = server.url();
  std::thread stopper([&]() {
    require(wait_flag(server.ready, 3000), "request reached fake provider");
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    cancelled_at.store(now_ms());
    cancelled.store(true);
  });
  bool was_cancelled = false;
  long status = 0;
  if (kind == RequestKind::Stream) {
    const auto r = openai_chat_completions_raw_stream(cfg, "{}", nullptr, nullptr);
    was_cancelled = r.cancelled;
    status = r.http_status;
  } else if (kind == RequestKind::Get) {
    const auto r = openai_http_get_raw(cfg, server.url() + "/models", {});
    was_cancelled = r.cancelled;
    status = r.http_status;
  } else {
    const auto r = openai_chat_completions_raw(cfg, "{}");
    was_cancelled = r.cancelled;
    status = r.http_status;
  }
  const auto finished_at = now_ms();
  stopper.join();
  require(was_cancelled, "blocked request reports cancellation");
  require(status == 0, "cancelled blocked request is not HTTP success");
  require(cancelled_at.load() > 0 && finished_at - cancelled_at.load() < 2000, "blocked cancellation under two seconds");
  require(wait_flag(server.disconnected, 500), "cancellation closes provider connection");
  require(server.requests.load() == 1, "cancelled transfer is never retried or sent through direct proxy fallback");
  std::cout << "blocked cancellation: " << finished_at - cancelled_at.load() << " ms\n";
}

static void test_cancel_streaming() {
  LoopbackProvider server(LoopbackProvider::Mode::Streaming);
  struct State { std::atomic<bool> cancelled{false}; int chunks = 0; } state;
  auto cfg = config_for(server, &state.cancelled);
  const auto started = now_ms();
  const auto r = openai_chat_completions_raw_stream(cfg, "{}",
    [](void* ctx, const char*, size_t) {
      auto* state = static_cast<State*>(ctx);
      state->chunks++;
      state->cancelled.store(true);
    }, &state);
  require(r.cancelled && !r.saw_done, "partial stream reports cancellation");
  require(state.chunks == 1, "no more decoded chunks after Stop");
  require(r.response_body.find("first") != std::string::npos, "received wire evidence preserved");
  require(now_ms() - started < 1500, "stream cancellation returns promptly");
  require(wait_flag(server.disconnected, 500), "stream cancellation closes provider connection");
  require(server.requests.load() == 1, "partial cancelled stream is never retried");
}

static void test_cancel_retry_wait() {
  LoopbackProvider server(LoopbackProvider::Mode::Retry);
  std::atomic<bool> cancelled{false};
  std::atomic<bool> retry_seen{false};
  auto cfg = config_for(server, &cancelled);
  cfg.on_retry = [](void* ctx, const char*) { static_cast<std::atomic<bool>*>(ctx)->store(true); };
  cfg.on_retry_ctx = &retry_seen;
  std::thread stopper([&]() {
    require(wait_flag(retry_seen, 3000), "provider entered retry wait");
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    cancelled.store(true);
  });
  const auto started = now_ms();
  const auto r = openai_chat_completions_raw(cfg, "{}");
  stopper.join();
  require(r.cancelled, "retry wait reports cancellation");
  require(now_ms() - started < 1500, "five-second Retry-After is interruptible");
  require(server.requests.load() == 1, "no request after cancelled retry wait");
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  // Make the fixture independent of developer/CI proxy environment.
  for (const char* name : {"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy"}) unsetenv(name);
  setenv("NO_PROXY", "", 1);
  setenv("no_proxy", "", 1);
  test_cancel_blocked(RequestKind::Post);
  test_cancel_blocked(RequestKind::Stream);
  test_cancel_blocked(RequestKind::Get);
  test_cancel_blocked(RequestKind::Post, true);
  test_cancel_streaming();
  test_cancel_retry_wait();
  std::cout << "ok\n";
  return 0;
}
