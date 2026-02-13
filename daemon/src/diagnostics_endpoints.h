#pragma once

#include "daemon_config.h"
#include "agentd/http_types.h"
#include "openai_client.h"

#include <chrono>
#include <string>

namespace agentd {

class AgentDb;
struct ToolExtension;

void handle_diagnostics_endpoint(
  const DaemonConfig& cfg,
  AgentDb* db_or_null,
  std::chrono::steady_clock::time_point start_time,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_diagnostics_providers_endpoint(
  const DaemonConfig& cfg,
  std::chrono::steady_clock::time_point start_time,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_diagnostics_provider_test_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
