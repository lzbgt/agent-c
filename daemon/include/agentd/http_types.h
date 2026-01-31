#pragma once

#include <map>
#include <string>

namespace agentd {

// Transport-agnostic request/response types used by the daemon endpoints.
//
// These are intentionally "HTTP-shaped" because:
// - the desktop dev path uses a local HTTP server + Web UI
// - other transports (MQTT, cloud relay, gRPC, etc.) can map their messages into this shape
//   without requiring agentd to own a specific network stack
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

}  // namespace agentd

