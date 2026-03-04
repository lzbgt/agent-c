#include "agentd/service.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

  const std::filesystem::path home_root = tmp / "home";
  const std::filesystem::path allow_root = tmp / "allow_root";
  const std::filesystem::path blocked_path = allow_root / ".ssh";
  const std::filesystem::path allow_file = home_root / ".config" / "agent" / "mount-allowlist.json";
  std::filesystem::create_directories(allow_root, ec);
  std::filesystem::create_directories(blocked_path, ec);
  std::filesystem::create_directories(allow_file.parent_path(), ec);
  const std::string allow_root_str = allow_root.generic_string();
  const std::string allow_json =
    std::string("{\"allowed_roots\":[{\"path\":\"") + allow_root_str +
    "\",\"readonly\":false}],\"blocked_patterns\":[\".ssh\"],\"non_main_readonly\":true}";
  {
    std::ofstream out(allow_file);
    out << allow_json;
    out.close();
  }
  (void)::setenv("HOME", home_root.c_str(), 1);

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
  const std::string body =
    std::string("{\"host_path\":\"") + allow_root_str + "\",\"container_path\":\"/workspace/extra/tmp\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", body, &resp_post)) {
    svc.stop();
    std::cerr << "http_post failed\n";
    return 5;
  }
  const bool post_ok_status = resp_post.find("200") != std::string::npos;
  const bool post_ok_body = resp_post.find("\"ok\":true") != std::string::npos;
  const bool post_allowed = resp_post.find("\"allowed\":true") != std::string::npos;
  const bool post_readonly = resp_post.find("\"readonly\":false") != std::string::npos;
  const bool post_reason = resp_post.find("\"reason\":\"ok\"") != std::string::npos;

  std::string resp_blocked;
  const std::string blocked_body =
    std::string("{\"host_path\":\"") + blocked_path.generic_string() + "\",\"container_path\":\"/workspace/extra/blocked\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", blocked_body, &resp_blocked)) {
    svc.stop();
    std::cerr << "http_post blocked failed\n";
    return 6;
  }
  const bool blocked_ok_status = resp_blocked.find("200") != std::string::npos;
  const bool blocked_ok_body = resp_blocked.find("\"ok\":true") != std::string::npos;
  const bool blocked_denied = resp_blocked.find("\"allowed\":false") != std::string::npos;
  const bool blocked_reason = resp_blocked.find("\"reason\":\"blocked_pattern\"") != std::string::npos;

  std::string resp_prefix;
  const std::string prefix_body =
    std::string("{\"host_path\":\"") + allow_root_str + "\",\"container_path\":\"/etc/passwd\",\"container_prefix\":\"/workspace/extra\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", prefix_body, &resp_prefix)) {
    svc.stop();
    std::cerr << "http_post prefix failed\n";
    return 7;
  }
  const bool prefix_ok_status = resp_prefix.find("200") != std::string::npos;
  const bool prefix_ok_body = resp_prefix.find("\"ok\":true") != std::string::npos;
  const bool prefix_denied = resp_prefix.find("\"allowed\":false") != std::string::npos;
  const bool prefix_reason = resp_prefix.find("\"reason\":\"container_path_outside_prefix\"") != std::string::npos;

  std::string resp_custom;
  const std::string custom_body =
    std::string("{\"host_path\":\"") + allow_root_str + "\",\"container_path\":\"/data/ok\",\"container_prefix\":\"/data\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", custom_body, &resp_custom)) {
    svc.stop();
    std::cerr << "http_post custom prefix failed\n";
    return 8;
  }
  const bool custom_ok_status = resp_custom.find("200") != std::string::npos;
  const bool custom_ok_body = resp_custom.find("\"ok\":true") != std::string::npos;
  const bool custom_allowed = resp_custom.find("\"allowed\":true") != std::string::npos;
  const bool custom_reason = resp_custom.find("\"reason\":\"ok\"") != std::string::npos;

  std::string resp_bad_prefix;
  const std::string bad_prefix_body =
    std::string("{\"host_path\":\"") + allow_root_str + "\",\"container_path\":\"/data/ok\",\"container_prefix\":\"data\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", bad_prefix_body, &resp_bad_prefix)) {
    svc.stop();
    std::cerr << "http_post bad prefix failed\n";
    return 9;
  }
  const bool bad_prefix_ok_status = resp_bad_prefix.find("200") != std::string::npos;
  const bool bad_prefix_ok_body = resp_bad_prefix.find("\"ok\":true") != std::string::npos;
  const bool bad_prefix_denied = resp_bad_prefix.find("\"allowed\":false") != std::string::npos;
  const bool bad_prefix_reason = resp_bad_prefix.find("\"reason\":\"container_prefix_invalid\"") != std::string::npos;

  std::string resp_missing_host;
  const std::string missing_host_body = "{}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", missing_host_body, &resp_missing_host)) {
    svc.stop();
    std::cerr << "http_post missing host failed\n";
    return 10;
  }
  const bool missing_host_status = resp_missing_host.find("400") != std::string::npos;
  const bool missing_host_err = resp_missing_host.find("missing host_path") != std::string::npos;

  std::string resp_missing_container;
  const std::string missing_container_body = "{\"host_path\":\"/tmp\"}";
  if (!http_post("127.0.0.1", port, "/api/v1/sandbox/mount_validate", missing_container_body, &resp_missing_container)) {
    svc.stop();
    std::cerr << "http_post missing container failed\n";
    return 11;
  }
  const bool missing_container_status = resp_missing_container.find("400") != std::string::npos;
  const bool missing_container_err = resp_missing_container.find("missing container_path") != std::string::npos;
  svc.stop();

  if (!ok_status || !ok_body || !post_ok_status || !post_ok_body || !post_allowed || !post_readonly || !post_reason ||
      !blocked_ok_status || !blocked_ok_body || !blocked_denied || !blocked_reason ||
      !prefix_ok_status || !prefix_ok_body || !prefix_denied || !prefix_reason ||
      !custom_ok_status || !custom_ok_body || !custom_allowed || !custom_reason ||
      !bad_prefix_ok_status || !bad_prefix_ok_body || !bad_prefix_denied || !bad_prefix_reason ||
      !missing_host_status || !missing_host_err || !missing_container_status || !missing_container_err) {
    std::cerr << "unexpected response:\n" << resp << "\n";
    std::cerr << "unexpected post response:\n" << resp_post << "\n";
    std::cerr << "unexpected blocked response:\n" << resp_blocked << "\n";
    std::cerr << "unexpected prefix response:\n" << resp_prefix << "\n";
    std::cerr << "unexpected custom prefix response:\n" << resp_custom << "\n";
    std::cerr << "unexpected bad prefix response:\n" << resp_bad_prefix << "\n";
    std::cerr << "unexpected missing host response:\n" << resp_missing_host << "\n";
    std::cerr << "unexpected missing container response:\n" << resp_missing_container << "\n";
    return 4;
  }
  return 0;
}
