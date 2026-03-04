#include "agentd/service.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static uint16_t pick_free_port_or_die() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "socket() failed\n";
    std::abort();
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "bind() failed\n";
    ::close(fd);
    std::abort();
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    std::cerr << "getsockname() failed\n";
    ::close(fd);
    std::abort();
  }
  const uint16_t port = ntohs(addr.sin_port);
  ::close(fd);
  if (port == 0) {
    std::cerr << "picked port=0\n";
    std::abort();
  }
  return port;
}

static bool http_get(const std::string& host, uint16_t port, const std::string& path, std::string* out) {
  if (out) out->clear();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0) {
    return false;
  }

  int fd = -1;
  for (addrinfo* p = res; p; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) return false;

  const std::string req =
    "GET " + path + " HTTP/1.1\r\n" +
    "Host: " + host + "\r\n" +
    "Connection: close\r\n" +
    "\r\n";
  const ssize_t sent = ::send(fd, req.data(), req.size(), 0);
  if (sent < 0 || (size_t)sent != req.size()) {
    ::close(fd);
    return false;
  }

  std::string resp;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    resp.append(buf, (size_t)n);
  }
  ::close(fd);
  if (out) *out = resp;
  return true;
}

static bool http_post(
  const std::string& host,
  uint16_t port,
  const std::string& path,
  const std::string& body,
  std::string* out
) {
  if (out) out->clear();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* res = nullptr;
  const std::string port_s = std::to_string(port);
  if (::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) != 0) {
    return false;
  }

  int fd = -1;
  for (addrinfo* p = res; p; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) return false;

  const std::string req =
    "POST " + path + " HTTP/1.1\r\n" +
    "Host: " + host + "\r\n" +
    "Content-Type: application/json\r\n" +
    "Content-Length: " + std::to_string(body.size()) + "\r\n" +
    "Connection: close\r\n" +
    "\r\n" +
    body;
  const ssize_t sent = ::send(fd, req.data(), req.size(), 0);
  if (sent < 0 || (size_t)sent != req.size()) {
    ::close(fd);
    return false;
  }

  std::string resp;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    resp.append(buf, (size_t)n);
  }
  ::close(fd);
  if (out) *out = resp;
  return true;
}

int main() {
  const uint16_t port = pick_free_port_or_die();
  const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "agentd_service_test";
  std::error_code ec;
  std::filesystem::create_directories(tmp, ec);
  const std::string db_path = (tmp / "agentd_test.db").string();

  agentd::DaemonConfig cfg;
  cfg.listen_host = "127.0.0.1";
  cfg.listen_port = port;
  cfg.db_path = db_path;
  cfg.cors_disabled = true;

  agentd::AgentdService::Options opt;
  opt.cfg = cfg;
  agentd::AgentdService svc(opt);
  std::string err;
  if (!svc.start_background(&err)) {
    std::cerr << "start_background failed: " << err << "\n";
    return 2;
  }

  // Give the accept loop a moment to start.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  std::string resp;
  if (!http_get("127.0.0.1", port, "/api/v1/health", &resp)) {
    svc.stop();
    std::cerr << "http_get failed\n";
    return 3;
  }

  const bool ok_status = resp.find("200") != std::string::npos;
  const bool ok_body = resp.find("\"ok\":true") != std::string::npos;

  std::string resp_post;
  const std::string body = "{\"host_path\":\"/tmp\",\"container_path\":\"/workspace/extra/tmp\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", body, &resp_post)) {
    svc.stop();
    std::cerr << "http_post failed\n";
    return 5;
  }
  const bool post_ok_status = resp_post.find("200") != std::string::npos;
  const bool post_ok_body = resp_post.find("\"ok\":true") != std::string::npos;
  const bool post_reason = resp_post.find("allowlist_missing") != std::string::npos ||
                           resp_post.find("\"allowed\":false") != std::string::npos;
  svc.stop();

  if (!ok_status || !ok_body || !post_ok_status || !post_ok_body || !post_reason) {
    std::cerr << "unexpected response:\n" << resp << "\n";
    std::cerr << "unexpected post response:\n" << resp_post << "\n";
    return 4;
  }
  return 0;
}
