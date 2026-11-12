#pragma once

#include "agent_db.h"
#include <json/json.h>

namespace agentd {
// Explicit process choices, not a replacement for tool permissions or person-level authorization.
bool workflow_decision_validate(const Json::Value& spec, std::string* error);
Json::Value workflow_decision_step(AgentDb* db, const AgentDb::WorkflowTaskRow& task,
  const Json::Value& spec, bool* yielded);
bool workflow_decisions_projection(AgentDb* db, const AgentDb::WorkflowRow& workflow,
  Json::Value* out, std::string* error);
}  // namespace agentd
