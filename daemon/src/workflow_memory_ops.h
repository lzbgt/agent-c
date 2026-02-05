#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "workflow_engine_common.h"

#include <json/json.h>

#include <cstdint>
#include <string>

namespace agentd {
namespace workflow_engine_internal {

// Workflow deterministic task implementations for memory maintenance operations.
// Budget checks are handled by the caller; these functions focus on correctness + evidence surfacing.

Json::Value workflow_memory_put_to_json(
  AgentDb* db,
  const DaemonConfig& cfg,
  const std::string& sessions_root_dir_fallback,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  const Json::Value& rr,
  WorkflowRunCancelCtx* cancel_ctx_or_null,
  int64_t now_unix_ms
);

Json::Value workflow_memory_consolidate_to_json(
  AgentDb* db,
  const DaemonConfig& cfg,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  const Json::Value& rr,
  WorkflowRunCancelCtx* cancel_ctx_or_null,
  int64_t now_unix_ms
);

}  // namespace workflow_engine_internal
}  // namespace agentd

