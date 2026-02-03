#pragma once

#include "agent_db.h"
#include "cors.h"
#include "daemon_config.h"
#include "openai_client.h"
#include "tool_extension.h"
#include "agentd/http_types.h"

#include <string>

namespace agentd {

// POST /api/v1/orchestrate
//
// Minimal orchestration endpoint:
// - accepts a list of run requests (same JSON shape as /api/v1/run, plus optional task_id)
// - forces no_session=true unless explicitly allowed
// - runs tasks with bounded concurrency and returns aggregated results
// - optional: if `writeback_session_id` is provided and DB is available, appends a synthesized assistant
//   message with the results into that session
void handle_orchestrate_endpoint(
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
