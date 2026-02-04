#pragma once

#include "cors.h"
#include "daemon_config.h"
#include "agentd/http_types.h"
#include "openai_client.h"
#include "agent_db.h"
#include "tool_extension.h"

#include <json/json.h>

#include <string>

namespace agentd {

using RunCancelCallback = bool (*)(void* ctx);

// Internal helper (also used by orchestrate endpoint): converts a run request JSON body into a response JSON.
Json::Value run_request_to_json_internal(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const std::string& request_body,
  const char* job_id_or_null
);

// Cancellable variant: allows callers (e.g. durable workflow engine) to request cooperative cancellation
// even for "sync" (non-job) runs.
Json::Value run_request_to_json_internal_cancellable(
  const DaemonConfig& daemon_cfg,
  const OpenAIClientConfig& ocfg,
  AgentDb* db_or_null,
  const ToolExtension* tool_ext_or_null,
  const std::string& sessions_root_dir,
  const std::string& request_body,
  const char* job_id_or_null,
  RunCancelCallback should_cancel,
  void* should_cancel_ctx
);

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
