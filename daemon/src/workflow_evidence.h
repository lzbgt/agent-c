#pragma once

#include "agent_db.h"

#include <json/json.h>

#include <cstdint>
#include <string>

namespace agentd {

std::string workflow_artifact_slug(std::string s);

void persist_workflow_session_artifact_best_effort(
  AgentDb* db,
  int64_t run_id,
  const AgentDb::WorkflowRow& wf,
  const std::string& task_id,
  int64_t ts_unix_ms,
  const Json::Value& artifact
);

int64_t create_workflow_evidence_run_best_effort(
  AgentDb* db,
  const AgentDb::WorkflowRow& wf,
  const AgentDb::WorkflowTaskRow& task,
  int64_t ts_unix_ms,
  const Json::Value& result
);

}  // namespace agentd
