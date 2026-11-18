#pragma once
#include "agent_db.h"
#include <json/json.h>
namespace agentd {
// Explicit authenticated process input, not publisher authentication or a tool permission.
bool workflow_signal_validate(const Json::Value& spec, std::string* error);
Json::Value workflow_signal_step(AgentDb* db, const AgentDb::WorkflowTaskRow& task,
  const Json::Value& spec, bool* yielded);
bool workflow_signals_projection(AgentDb* db, const AgentDb::WorkflowRow& workflow,
  Json::Value* out, std::string* error);
}  // namespace agentd
