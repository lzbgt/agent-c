#pragma once

#include "agentd/http_types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

// Minimal HTTP/1.1 server for local daemon development.
//
// Intentional scope:
// - Only what we need for a local daemon + web UI on day 1.
// - Avoids pulling a large third-party dependency; can be swapped later.
//
// Supported:
// - Common methods (GET/POST/DELETE/OPTIONS)
// - Content-Length bodies (no chunked encoding)
// - Concurrent per-connection handling (accept loop spawns a thread per client)

namespace agentd {

class HttpServer {
 public:
  using Handler = std::function<void(const HttpRequest&, HttpResponse*)>;
  using StreamHandler = std::function<void(const HttpRequest&, int client_fd)>;
  using OptionsHandler = std::function<void(const HttpRequest&, HttpResponse*)>;

  HttpServer();
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void set_default_headers(std::map<std::string, std::string> headers);
  // Optional: global handler invoked for any OPTIONS request.
  // Useful for centralized CORS preflight logic.
  void set_options_handler(OptionsHandler handler);
  void handle(const std::string& method, const std::string& path, Handler handler);
  // Registers a handler that writes directly to the client socket (e.g., SSE).
  // The handler must write a full HTTP response (status line + headers + body streaming).
  // The server will close the socket after the handler returns.
  void handle_stream(const std::string& method, const std::string& path, StreamHandler handler);

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
  std::map<RouteKey, StreamHandler> stream_routes_;
  OptionsHandler options_handler_;
  std::map<std::string, std::string> default_headers_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
};

}  // namespace agentd
