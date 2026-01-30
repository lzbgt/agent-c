#include "http_server.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string_view>
#include <thread>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace agentd {

static std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

static void trim_inplace(std::string& s) {
  size_t a = 0;
  while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
  size_t b = s.size();
  while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
  s = s.substr(a, b - a);
}

static bool read_exact(int fd, std::string* out, size_t n) {
  out->clear();
  out->reserve(n);
  while (out->size() < n) {
    char buf[4096];
    const size_t want = std::min(sizeof(buf), n - out->size());
    ssize_t r = ::read(fd, buf, want);
    if (r <= 0) {
      return false;
    }
    out->append(buf, (size_t)r);
  }
  return true;
}

static bool read_until(int fd, std::string* out, const std::string& needle, size_t max_bytes) {
  out->clear();
  while (out->size() < max_bytes) {
    char buf[2048];
    ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r <= 0) {
      return false;
    }
    out->append(buf, (size_t)r);
    if (out->find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

static bool parse_request(const std::string& head, HttpRequest* out_req, size_t* out_header_bytes, size_t* out_content_length) {
  if (!out_req || !out_header_bytes || !out_content_length) {
    return false;
  }
  *out_header_bytes = 0;
  *out_content_length = 0;
  out_req->headers.clear();
  out_req->body.clear();

  const size_t header_end = head.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    return false;
  }
  *out_header_bytes = header_end + 4;

  std::istringstream iss(head.substr(0, header_end));
  std::string request_line;
  if (!std::getline(iss, request_line)) {
    return false;
  }
  if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

  std::istringstream rls(request_line);
  std::string method, path, version;
  rls >> method >> path >> version;
  if (method.empty() || path.empty() || version.empty()) {
    return false;
  }
  out_req->method = method;
  out_req->raw_path = path;
  out_req->path = path;
  out_req->query.clear();
  {
    const size_t q = out_req->path.find('?');
    if (q != std::string::npos) {
      out_req->query = out_req->path.substr(q + 1);
      out_req->path = out_req->path.substr(0, q);
    }
  }

  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string k = lower(line.substr(0, colon));
    std::string v = line.substr(colon + 1);
    trim_inplace(k);
    trim_inplace(v);
    out_req->headers[k] = v;
  }

  auto it = out_req->headers.find("content-length");
  if (it != out_req->headers.end()) {
    try {
      *out_content_length = (size_t)std::stoull(it->second);
    } catch (...) {
      return false;
    }
  }
  return true;
}

static std::string reason_phrase(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "OK";
  }
}

static std::string build_response(const HttpResponse& resp, const std::map<std::string, std::string>& default_headers) {
  std::ostringstream oss;
  oss << "HTTP/1.1 " << resp.status << " " << reason_phrase(resp.status) << "\r\n";

  std::map<std::string, std::string> headers = default_headers;
  for (const auto& kv : resp.headers) {
    headers[kv.first] = kv.second;
  }
  headers["Content-Length"] = std::to_string(resp.body.size());
  if (!headers.count("Content-Type")) {
    headers["Content-Type"] = "application/json; charset=utf-8";
  }
  if (!headers.count("Connection")) {
    headers["Connection"] = "close";
  }

  for (const auto& kv : headers) {
    oss << kv.first << ": " << kv.second << "\r\n";
  }
  oss << "\r\n";
  oss << resp.body;
  return oss.str();
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() {
  stop();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HttpServer::set_default_headers(std::map<std::string, std::string> headers) {
  default_headers_ = std::move(headers);
}

void HttpServer::set_options_handler(OptionsHandler handler) {
  options_handler_ = std::move(handler);
}

void HttpServer::handle(const std::string& method, const std::string& path, Handler handler) {
  RouteKey k{method, path};
  routes_[k] = std::move(handler);
}

void HttpServer::handle_stream(const std::string& method, const std::string& path, StreamHandler handler) {
  RouteKey k{method, path};
  stream_routes_[k] = std::move(handler);
}

void HttpServer::stop() {
  stop_.store(true);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
  }
}

bool HttpServer::serve(const std::string& host, uint16_t port, std::string* out_error) {
  if (out_error) out_error->clear();
  stop_.store(false);

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    if (out_error) *out_error = std::string("socket failed: ") + std::strerror(errno);
    return false;
  }
  int yes = 1;
  (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  // Bind strictly to the requested host (default should be 127.0.0.1).
  // Supported forms: "127.0.0.1", "0.0.0.0".
  // Note: INADDR_LOOPBACK / INADDR_ANY are host-order constants; sockaddr expects network order.
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (host == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (host != "127.0.0.1") {
    in_addr a{};
    if (::inet_pton(AF_INET, host.c_str(), &a) == 1) {
      addr.sin_addr = a;
    }
  }
  addr.sin_port = htons(port);

  if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0) {
    if (out_error) *out_error = std::string("bind failed: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    if (out_error) *out_error = std::string("listen failed: ") + std::strerror(errno);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  // Handle each client in its own thread to keep the daemon responsive even when
  // a request is long-running (e.g., SSE streaming endpoint).
  while (!stop_.load()) {
    int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      if (stop_.load()) break;
      continue;
    }

    std::thread([this, client]() {
      std::string head;
      if (!read_until(client, &head, "\r\n\r\n", 1024 * 1024)) {
        ::close(client);
        return;
      }

      HttpRequest req;
      size_t header_bytes = 0;
      size_t content_len = 0;
      if (!parse_request(head, &req, &header_bytes, &content_len)) {
        HttpResponse resp;
        resp.status = 400;
        resp.body = R"({"ok":false,"error":"bad request"})";
        const std::string wire = build_response(resp, default_headers_);
        (void)::write(client, wire.data(), wire.size());
        ::close(client);
        return;
      }

      // If head included bytes after headers, treat them as start of body.
      std::string body_already;
      if (head.size() > header_bytes) {
        body_already = head.substr(header_bytes);
      }
      if (content_len > 0) {
        if (body_already.size() >= content_len) {
          req.body = body_already.substr(0, content_len);
        } else {
          std::string rest;
          if (!read_exact(client, &rest, content_len - body_already.size())) {
            ::close(client);
            return;
          }
          req.body = body_already + rest;
        }
      }

      if (req.method == "OPTIONS") {
        HttpResponse resp;
        if (options_handler_) {
          options_handler_(req, &resp);
        } else {
          resp.status = 204;
          resp.body.clear();
        }
        const std::string wire = build_response(resp, default_headers_);
        (void)::write(client, wire.data(), wire.size());
        ::close(client);
        return;
      }

      // Prefer streaming routes when registered.
      auto sit = stream_routes_.find(RouteKey{req.method, req.path});
      if (sit != stream_routes_.end()) {
        sit->second(req, client);
        // Server closes socket after handler returns.
        ::close(client);
        return;
      }

      HttpResponse resp;
      auto it = routes_.find(RouteKey{req.method, req.path});
      if (it == routes_.end()) {
        resp.status = 404;
        resp.body = R"({"ok":false,"error":"not found"})";
      } else {
        it->second(req, &resp);
      }

      const std::string wire = build_response(resp, default_headers_);
      (void)::write(client, wire.data(), wire.size());
      ::close(client);
    }).detach();
  }

  return true;
}

}  // namespace agentd
