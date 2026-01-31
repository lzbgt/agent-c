#pragma once

#include "daemon_config.h"
#include "tool_extension.h"

#include <cstdint>
#include <memory>
#include <string>

namespace agentd {

// Embeddable agentd service wrapper.
//
// This provides a library-friendly way to run agentd as an in-process sidecar:
// - owns the DB connection, config store, and HTTP server
// - supports optional ToolExtension injection (additional tools beyond basic|host)
//
// Threading:
// - serve_blocking() runs the accept loop on the current thread
// - start_background() runs serve_blocking() on an internal thread
class AgentdService {
 public:
  struct Options {
    DaemonConfig cfg;
    // Optional tool extension. If set, the extension is applied to:
    // - /api/v1/tools (listing)
    // - /api/v1/run and /api/v1/run_async (execution)
    //
    // The caller must ensure the extension callbacks remain valid for the service lifetime.
    ToolExtension tool_extension{};
    bool enable_tool_extension = false;
  };

  explicit AgentdService(Options options);
  ~AgentdService();

  AgentdService(const AgentdService&) = delete;
  AgentdService& operator=(const AgentdService&) = delete;

  // Initializes DB + route registration. Safe to call multiple times.
  bool init(std::string* out_error);

  // Runs the HTTP server accept loop on the current thread (blocks until stop()).
  bool serve_blocking(std::string* out_error);

  // Runs serve_blocking() on a background thread.
  bool start_background(std::string* out_error);

  // Stops the server (safe to call from any thread).
  void stop();

  // Best-effort diagnostics.
  std::string listen_host() const;
  uint16_t listen_port() const;
  std::string db_path() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace agentd

