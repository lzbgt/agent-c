#pragma once

#include "workflow_engine_common.h"

#include <json/json.h>

#include <cstdint>
#include <string>

namespace agentd {
namespace workflow_engine_internal {

struct WorkflowRunBudgetClampInput {
  WorkflowLimits limits;
  int64_t tool_calls_remaining = 0;
  int64_t steps_remaining = 0;
  int64_t elapsed_ms_remaining = 0;
  int64_t total_tokens_remaining = 0;
};

// Applies remaining workflow budgets to a run request object before execution.
// Returns false and sets `out_exceeded` when a configured budget is already exhausted.
bool workflow_apply_run_budget_clamps(
  Json::Value* request,
  const WorkflowRunBudgetClampInput& input,
  std::string* out_exceeded
);

}  // namespace workflow_engine_internal
}  // namespace agentd
