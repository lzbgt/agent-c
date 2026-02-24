#include "http_server.h"

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>

#include "net_compat.h"
#include "http_util.h"

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

static bool token_is_safe(const std::string& s) {
  if (s.empty() || s.size() > 128) return false;
  for (char c : s) {
    const bool ok =
      (c >= 'a' && c <= 'z') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == ':' || c == '@';
    if (!ok) return false;
  }
  return true;
}

static std::string make_request_id() {
  std::random_device rd;
  std::mt19937_64 gen(((uint64_t)rd() << 32) ^ (uint64_t)rd());
  std::uniform_int_distribution<uint32_t> dist(0, 0xffffffffu);

  uint32_t a = dist(gen);
  uint16_t b = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t c = (uint16_t)(dist(gen) & 0xffffu);
  uint16_t d = (uint16_t)(dist(gen) & 0xffffu);
  uint64_t e = ((uint64_t)dist(gen) << 32) ^ (uint64_t)dist(gen);

  c = (uint16_t)((c & 0x0fffu) | 0x4000u);
  d = (uint16_t)((d & 0x3fffu) | 0x8000u);

  char buf[96];
  (void)snprintf(
    buf,
    sizeof(buf),
    "req_%08x-%04x-%04x-%04x-%012llx",
    a,
    (unsigned)b,
    (unsigned)c,
    (unsigned)d,
    (unsigned long long)(e & 0xffffffffffffull)
  );
  return std::string(buf);
}

static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          (void)snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
        break;
    }
  }
  return out;
}

static int access_log_mode() {
  const char* v = std::getenv("AGENTD_ACCESS_LOG");
  if (!v || !v[0]) return 0;
  std::string s = lower(std::string(v));
  trim_inplace(s);
  if (s == "json") return 2;
  if (s == "1" || s == "true" || s == "yes" || s == "on") return 1;
  return 0;
}

static size_t max_request_body_bytes() {
  const size_t kDefault = 64ull * 1024ull * 1024ull;
  const size_t kMaxCap = 512ull * 1024ull * 1024ull;
  const char* v = std::getenv("AGENTD_HTTP_MAX_BODY_BYTES");
  if (!v || !v[0]) return kDefault;
  std::string s(v);
  trim_inplace(s);
  if (s.empty()) return kDefault;
  if (s == "0") return 0;
  try {
    unsigned long long parsed = std::stoull(s);
    if (parsed == 0) return 0;
    if (parsed > kMaxCap) parsed = kMaxCap;
    return (size_t)parsed;
  } catch (...) {
    return kDefault;
  }
}

static size_t max_request_header_bytes() {
  const size_t kDefault = 1ull * 1024ull * 1024ull;
  const size_t kMaxCap = 32ull * 1024ull * 1024ull;
  const char* v = std::getenv("AGENTD_HTTP_MAX_HEADER_BYTES");
  if (!v || !v[0]) return kDefault;
  std::string s(v);
  trim_inplace(s);
  if (s.empty()) return kDefault;
  if (s == "0") return 0;
  try {
    unsigned long long parsed = std::stoull(s);
    if (parsed == 0) return 0;
    if (parsed > kMaxCap) parsed = kMaxCap;
    return (size_t)parsed;
  } catch (...) {
    return kDefault;
  }
}

static int http_read_timeout_ms() {
  const int kDefault = 15000;
  const int kMaxCap = 300000;
  const char* v = std::getenv("AGENTD_HTTP_READ_TIMEOUT_MS");
  if (!v || !v[0]) return kDefault;
  std::string s(v);
  trim_inplace(s);
  if (s.empty()) return kDefault;
  try {
    long long parsed = std::stoll(s);
    if (parsed <= 0) return 0;
    if (parsed > kMaxCap) parsed = kMaxCap;
    return (int)parsed;
  } catch (...) {
    return kDefault;
  }
}

static bool is_timeout_err(int err) {
#if defined(_WIN32)
  return err == WSAETIMEDOUT;
#else
  return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

static void set_socket_read_timeout(socket_t fd, int timeout_ms) {
  if (!socket_valid(fd) || timeout_ms <= 0) return;
#if defined(_WIN32)
  DWORD tv = (DWORD)timeout_ms;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
  struct timeval tv;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
}

static std::string addr_to_string(const sockaddr_storage& addr) {
  char buf[INET6_ADDRSTRLEN];
  if (addr.ss_family == AF_INET) {
    const auto* a = reinterpret_cast<const sockaddr_in*>(&addr);
    if (::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf)) != nullptr) {
      return std::string(buf);
    }
  } else if (addr.ss_family == AF_INET6) {
    const auto* a = reinterpret_cast<const sockaddr_in6*>(&addr);
    if (::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf)) != nullptr) {
      return std::string(buf);
    }
  }
  return std::string();
}

struct ReadResult {
  bool ok = false;
  bool timeout = false;
  bool too_large = false;
};

static ReadResult read_exact(socket_t fd, std::string* out, size_t n) {
  ReadResult rr;
  out->clear();
  out->reserve(n);
  while (out->size() < n) {
    char buf[4096];
    const size_t want = std::min(sizeof(buf), n - out->size());
    socket_io_t r = socket_read(fd, buf, want);
    if (r == 0) return rr;
    if (r == kSocketError) {
      const int err = socket_last_error();
      if (socket_should_retry(err)) continue;
      if (is_timeout_err(err)) {
        rr.timeout = true;
      }
      return rr;
    }
    out->append(buf, (size_t)r);
  }
  rr.ok = true;
  return rr;
}

static ReadResult read_until(socket_t fd, std::string* out, const std::string& needle, size_t max_bytes) {
  ReadResult rr;
  out->clear();
  const size_t limit = max_bytes == 0 ? std::numeric_limits<size_t>::max() : max_bytes;
  while (out->size() < limit) {
    const size_t remaining = limit - out->size();
    if (remaining == 0) {
      rr.too_large = true;
      return rr;
    }
    char buf[2048];
    const size_t want = std::min(sizeof(buf), remaining);
    socket_io_t r = socket_read(fd, buf, want);
    if (r == 0) return rr;
    if (r == kSocketError) {
      const int err = socket_last_error();
      if (socket_should_retry(err)) continue;
      if (is_timeout_err(err)) {
        rr.timeout = true;
      }
      return rr;
    }
    out->append(buf, (size_t)r);
    if (out->find(needle) != std::string::npos) {
      rr.ok = true;
      return rr;
    }
  }
  rr.too_large = true;
  return rr;
}

static bool write_all(socket_t fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    socket_io_t w = socket_write(fd, s.data() + off, s.size() - off);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w == kSocketError && socket_should_retry(socket_last_error())) {
      continue;
    }
    return false;
  }
  return true;
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
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 405: return "Method Not Allowed";
    case 431: return "Request Header Fields Too Large";
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
  if (socket_valid(listen_fd_)) {
    socket_close(listen_fd_);
    listen_fd_ = kInvalidSocket;
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

void HttpServer::handle_prefix(const std::string& method, const std::string& path_prefix, Handler handler) {
  RouteKey k{method, path_prefix};
  prefix_routes_.emplace_back(std::move(k), std::move(handler));
}

void HttpServer::handle_stream(const std::string& method, const std::string& path, StreamHandler handler) {
  RouteKey k{method, path};
  stream_routes_[k] = std::move(handler);
}

void HttpServer::stop() {
  stop_.store(true);
  if (socket_valid(listen_fd_)) {
    (void)socket_shutdown(listen_fd_);
    // Ensure any blocking accept() wakes up promptly.
    socket_close(listen_fd_);
    listen_fd_ = kInvalidSocket;
  }
}

bool HttpServer::serve(const std::string& host, uint16_t port, std::string* out_error) {
  if (out_error) out_error->clear();
  stop_.store(false);

  if (!net_init(out_error)) {
    return false;
  }

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (!socket_valid(listen_fd_)) {
    if (out_error) *out_error = std::string("socket failed: ") + socket_strerror(socket_last_error());
    return false;
  }
  int yes = 1;
  (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

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
    if (out_error) *out_error = std::string("bind failed: ") + socket_strerror(socket_last_error());
    socket_close(listen_fd_);
    listen_fd_ = kInvalidSocket;
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    if (out_error) *out_error = std::string("listen failed: ") + socket_strerror(socket_last_error());
    socket_close(listen_fd_);
    listen_fd_ = kInvalidSocket;
    return false;
  }

  const int log_mode = access_log_mode();
  const size_t max_body = max_request_body_bytes();
  const size_t max_header = max_request_header_bytes();
  const int read_timeout_ms = http_read_timeout_ms();

  // Handle each client in its own thread to keep the daemon responsive even when
  // a request is long-running (e.g., SSE streaming endpoint).
  while (!stop_.load()) {
    sockaddr_storage peer{};
    socklen_t peer_len = sizeof(peer);
    socket_t client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (!socket_valid(client)) {
      if (stop_.load()) break;
      continue;
    }

    const std::string peer_addr = addr_to_string(peer);
    std::thread([this, client, peer_addr, log_mode, max_body, max_header, read_timeout_ms]() {
      const auto start_time = std::chrono::steady_clock::now();
      std::string request_id = make_request_id();

      auto log_access = [&](const HttpRequest& req,
                            int status,
                            bool stream,
                            size_t bytes_in,
                            size_t bytes_out,
                            const std::string& trace_id) {
        if (log_mode == 0) return;
        const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
        const int64_t latency_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start_time)
                                     .count();
        if (log_mode == 2) {
          std::ostringstream oss;
          oss << "{\"ts_unix_ms\":" << now_ms
              << ",\"method\":\"" << json_escape(req.method)
              << "\",\"path\":\"" << json_escape(req.raw_path)
              << "\",\"status\":" << status
              << ",\"latency_ms\":" << latency_ms
              << ",\"bytes_in\":" << (unsigned long long)bytes_in
              << ",\"bytes_out\":" << (unsigned long long)bytes_out
              << ",\"stream\":" << (stream ? "true" : "false")
              << ",\"request_id\":\"" << json_escape(request_id) << "\"";
          if (!trace_id.empty()) {
            oss << ",\"trace_id\":\"" << json_escape(trace_id) << "\"";
          }
          if (!peer_addr.empty()) {
            oss << ",\"remote\":\"" << json_escape(peer_addr) << "\"";
          }
          oss << "}\n";
          std::cerr << oss.str();
        } else {
          std::cerr << "agentd http " << req.method << " " << req.raw_path << " " << status
                    << " " << latency_ms << "ms"
                    << " in=" << (unsigned long long)bytes_in
                    << " out=" << (unsigned long long)bytes_out
                    << " stream=" << (stream ? "1" : "0")
                    << " req_id=" << request_id;
          if (!trace_id.empty()) {
            std::cerr << " trace_id=" << trace_id;
          }
          if (!peer_addr.empty()) {
            std::cerr << " remote=" << peer_addr;
          }
          std::cerr << "\n";
        }
      };

      auto send_response = [&](const HttpRequest& req, HttpResponse& resp, size_t bytes_in) {
        if (!resp.headers.count("X-Request-Id")) {
          resp.headers["X-Request-Id"] = request_id;
        }
        std::string trace_id;
        auto tit = resp.headers.find("X-Trace-Id");
        if (tit == resp.headers.end()) {
          tit = resp.headers.find("x-trace-id");
        }
        if (tit != resp.headers.end()) {
          trace_id = tit->second;
        } else {
          const auto it = req.headers.find("x-trace-id");
          if (it != req.headers.end()) {
            std::string v = it->second;
            trim_inplace(v);
            if (token_is_safe(v)) trace_id = v;
          }
        }
        if (!trace_id.empty() && !resp.headers.count("X-Trace-Id")) {
          resp.headers["X-Trace-Id"] = trace_id;
        }
        const std::string wire = build_response(resp, default_headers_);
        (void)write_all(client, wire);
        log_access(req, resp.status, false, bytes_in, resp.body.size(), trace_id);
        socket_close(client);
      };

      auto send_raw_error = [&](int status, const std::string& body, size_t bytes_in) {
        HttpResponse resp;
        resp.status = status;
        resp.body = body;
        resp.headers["X-Request-Id"] = request_id;
        const std::string wire = build_response(resp, default_headers_);
        (void)write_all(client, wire);
        HttpRequest req;
        req.method = "-";
        req.raw_path = "-";
        log_access(req, status, false, bytes_in, resp.body.size(), "");
        socket_close(client);
      };

      set_socket_read_timeout(client, read_timeout_ms);

      std::string head;
      const ReadResult head_rr = read_until(client, &head, "\r\n\r\n", max_header);
      if (!head_rr.ok) {
        if (head_rr.timeout) {
          send_raw_error(408, json_error_body("request timeout"), head.size());
          return;
        }
        if (head_rr.too_large) {
          send_raw_error(431, json_error_body("request header too large"), head.size());
          return;
        }
        socket_close(client);
        return;
      }

      HttpRequest req;
      size_t header_bytes = 0;
      size_t content_len = 0;
      if (!parse_request(head, &req, &header_bytes, &content_len)) {
        HttpResponse resp;
        resp.status = 400;
        resp.body = json_error_body("bad request");
        send_response(req, resp, 0);
        return;
      }
      if (max_header > 0 && header_bytes > max_header) {
        HttpResponse resp;
        resp.status = 431;
        resp.body = json_error_body("request header too large");
        send_response(req, resp, header_bytes);
        return;
      }
      if (max_body > 0 && content_len > max_body) {
        HttpResponse resp;
        resp.status = 413;
        resp.body = json_error_body("request body too large");
        send_response(req, resp, 0);
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
          const ReadResult body_rr = read_exact(client, &rest, content_len - body_already.size());
          if (!body_rr.ok) {
            if (body_rr.timeout) {
              send_raw_error(408, json_error_body("request timeout"), head.size() + body_already.size());
              return;
            }
            socket_close(client);
            return;
          }
          req.body = body_already + rest;
        }
      }

      {
        auto it = req.headers.find("x-request-id");
        if (it != req.headers.end()) {
          std::string v = it->second;
          trim_inplace(v);
          if (token_is_safe(v)) {
            request_id = v;
          }
        }
        req.headers["x-request-id"] = request_id;
      }

      if (req.method == "OPTIONS") {
        HttpResponse resp;
        if (options_handler_) {
          options_handler_(req, &resp);
        } else {
          resp.status = 204;
          resp.body.clear();
        }
        send_response(req, resp, req.body.size());
        return;
      }

      // Prefer streaming routes when registered.
      auto sit = stream_routes_.find(RouteKey{req.method, req.path});
      if (sit != stream_routes_.end()) {
        sit->second(req, client);
        std::string trace_id;
        const auto it = req.headers.find("x-trace-id");
        if (it != req.headers.end()) {
          trace_id = it->second;
          trim_inplace(trace_id);
          if (!token_is_safe(trace_id)) trace_id.clear();
        }
        log_access(req, 200, true, req.body.size(), 0, trace_id);
        // Server closes socket after handler returns.
        socket_close(client);
        return;
      }

      HttpResponse resp;
      auto it = routes_.find(RouteKey{req.method, req.path});
      if (it == routes_.end()) {
        const Handler* prefix_match = nullptr;
        size_t best_len = 0;
        for (const auto& entry : prefix_routes_) {
          if (entry.first.method != req.method) continue;
          const std::string& prefix = entry.first.path;
          if (prefix.empty()) continue;
          if (req.path.compare(0, prefix.size(), prefix) != 0) continue;
          if (prefix.size() >= best_len) {
            best_len = prefix.size();
            prefix_match = &entry.second;
          }
        }
        if (prefix_match) {
          (*prefix_match)(req, &resp);
        } else {
          resp.status = 404;
          resp.body = json_error_body("not found");
        }
      } else {
        it->second(req, &resp);
      }

      send_response(req, resp, req.body.size());
    }).detach();
  }

  return true;
}

}  // namespace agentd
