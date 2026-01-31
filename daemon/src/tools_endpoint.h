#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "tool_extension.h"

namespace agentd {

void handle_tools_endpoint(
  const DaemonConfig& cfg,
  const CorsConfig& cors_cfg,
  const std::string& sessions_root_dir,
  const ToolExtension* tool_ext_or_null,
  const HttpRequest& req,
  HttpResponse* resp
);

}  // namespace agentd
