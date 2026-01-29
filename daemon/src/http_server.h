#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>

// Minimal HTTP/1.1 server for localhost development.
//
// Intentional scope:
// - Only what we need for a local daemon + web UI on day 1.
// - Avoids pulling a large third-party dependency; can be swapped later.
//
// Supported:
// - GET, POST, OPTIONS
// - Content-Length bodies (no chunked encoding)
// - Single-threaded accept loop

struct HttpRequest {
  std::string method;
  std::string raw_path; // includes optional query string
  std::string path;     // path only (no query)
  std::string query;    // portion after '?', without '?'
  std::map<std::string, std::string> headers; // lower-cased keys
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::map<std::string, std::string> headers;
  std::string body;
};

class HttpServer {
 public:
  using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;

  HttpServer();
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void set_default_headers(std::map<std::string, std::string> headers);
  void handle(const std::string& method, const std::string& path, Handler handler);

  // Blocks until stop() is called or accept fails.
  // Returns false on bind/listen failure.
  bool serve(const std::string& host, uint16_t port, std::string* out_error);

  void stop();

 private:
  struct RouteKey {
    std::string method;
    std::string path;
    bool operator<(const RouteKey& o) const {
      if (method != o.method) return method < o.method;
      return path < o.path;
    }
  };

  std::map<RouteKey, Handler> routes_;
  std::map<std::string, std::string> default_headers_;
  int listen_fd_ = -1;
  bool stop_ = false;
};
