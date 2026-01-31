#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "openai_client.h"
#include "agent_db.h"
#include "tool_extension.h"

#include <string>

namespace agentd {

void handle_run_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

void handle_run_async_endpoint(
  const DaemonConfig& cfg,
  const OpenAIClientConfig& ocfg,
  const CorsConfig& cors_cfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
