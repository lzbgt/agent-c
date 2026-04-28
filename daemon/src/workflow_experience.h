#pragma once

#include "agent_db.h"
#include "daemon_config.h"
#include "workflow_engine_common.h"

#include <json/json.h>

#include <string>
#include <unordered_map>

namespace agentd {
namespace workflow_engine_internal {

Json::Value workflow_experience_record_to_json(
  AgentDb* db,
  const DaemonConfig& cfg,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  const Json::Value& rr,
  const std::unordered_map<std::string, Json::Value>& result_json_by_task,
  int64_t now_unix_ms
);

}  // namespace workflow_engine_internal
}  // namespace agentd
