#pragma once
#include "agent_db.h"
#include "daemon_config.h"
#include "workflow_engine_common.h"
#include <json/json.h>
namespace agentd {
AgentDb::WorkflowBudgetAmounts
workflow_budget_limits(const workflow_engine_internal::WorkflowLimits &limits);
AgentDb::WorkflowBudgetAmounts
workflow_budget_demand(const Json::Value &request, const DaemonConfig &config,
                       const AgentDb::WorkflowBudgetAmounts &limits);
Json::Value
workflow_budget_amounts_json(const AgentDb::WorkflowBudgetAmounts &amounts);
const char *workflow_budget_dimension(int dimension);
} // namespace agentd
