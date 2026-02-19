#pragma once

#include "daemon_config.h"
#include "http_types.h"
#include "tool_extension.h"

#include <cstdint>
#include <map>
#include <string>

namespace agentd {

// Transport-agnostic agentd "API surface" that can be driven by different client protocols.
//
// - Desktop dev: a local HTTP server maps incoming HTTP requests into HttpRequest/HttpResponse.
// - Cloud/broker relays: a relay can map messages into HttpRequest and call handle().
// - App embedding: host app can call handle() directly without opening a local port.
//
// This keeps the daemon logic independent of a specific wire transport.
class AgentdApi {
 public:
  struct Options {
    DaemonConfig cfg;
    ToolExtension tool_extension{};
    bool enable_tool_extension = false;
  };

  explicit AgentdApi(Options options);
  ~AgentdApi();

  AgentdApi(const AgentdApi&) = delete;
  AgentdApi& operator=(const AgentdApi&) = delete;

  // Opens DB (defaults to <state_dir>/agentd.db if empty), loads runtime config, and becomes ready to serve requests.
  bool init(std::string* out_error);

  // Dispatches a request to the appropriate endpoint handler.
  // Returns true if a route matched (even if it produced an error response); false for 404.
  bool handle(const HttpRequest& req, HttpResponse* resp);

  std::string db_path() const;
  std::string listen_host() const;
  uint16_t listen_port() const;

 private:
  using Handler = void (*)(void* ctx, const HttpRequest& req, HttpResponse* resp);
  struct RouteKey {
    std::string method;
    std::string path;
    bool operator<(const RouteKey& o) const {
      if (method != o.method) return method < o.method;
      return path < o.path;
    }
  };

  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace agentd
