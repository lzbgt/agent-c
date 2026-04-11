#include "workflow_run_budget_clamp.h"

#include <cassert>
#include <string>

int main() {
  using agentd::workflow_engine_internal::WorkflowRunBudgetClampInput;
  using agentd::workflow_engine_internal::workflow_apply_run_budget_clamps;

  {
    Json::Value req(Json::objectValue);
    req["prompt"] = "hi";
    WorkflowRunBudgetClampInput in;
    in.limits.max_total_tokens = 10;
    in.total_tokens_remaining = 7;
    std::string exceeded;
    assert(workflow_apply_run_budget_clamps(&req, in, &exceeded));
    assert(exceeded.empty());
    assert(req["max_completion_tokens"].asInt64() == 7);
  }
  {
    Json::Value req(Json::objectValue);
    req["max_completion_tokens"] = (Json::Int64)99;
    WorkflowRunBudgetClampInput in;
    in.limits.max_total_tokens = 100;
    in.total_tokens_remaining = 12;
    std::string exceeded;
    assert(workflow_apply_run_budget_clamps(&req, in, &exceeded));
    assert(req["max_completion_tokens"].asInt64() == 12);
  }
  {
    Json::Value req(Json::objectValue);
    req["max_completion_tokens"] = (Json::Int64)5;
    WorkflowRunBudgetClampInput in;
    in.limits.max_total_tokens = 100;
    in.total_tokens_remaining = 12;
    assert(workflow_apply_run_budget_clamps(&req, in, nullptr));
    assert(req["max_completion_tokens"].asInt64() == 5);
  }
  {
    Json::Value req(Json::objectValue);
    req["max_tokens"] = (Json::Int64)99;
    WorkflowRunBudgetClampInput in;
    in.limits.max_total_tokens = 100;
    in.total_tokens_remaining = 12;
    assert(workflow_apply_run_budget_clamps(&req, in, nullptr));
    assert(!req.isMember("max_completion_tokens"));
    assert(req["max_tokens"].asInt64() == 12);
  }
  {
    Json::Value req(Json::objectValue);
    req["max_completion_tokens"] = (Json::Int64)99;
    req["max_tokens"] = (Json::Int64)77;
    WorkflowRunBudgetClampInput in;
    in.limits.max_total_tokens = 100;
    in.total_tokens_remaining = 12;
    assert(workflow_apply_run_budget_clamps(&req, in, nullptr));
    assert(req["max_completion_tokens"].asInt64() == 12);
    assert(req["max_tokens"].asInt64() == 12);
  }
  {
    Json::Value req(Json::objectValue);
    WorkflowRunBudgetClampInput in;
    in.limits.max_tool_calls_total = 10;
    in.limits.max_steps_total = 20;
    in.limits.max_elapsed_ms_total = 3000;
    in.tool_calls_remaining = 3;
    in.steps_remaining = 4;
    in.elapsed_ms_remaining = 500;
    assert(workflow_apply_run_budget_clamps(&req, in, nullptr));
    assert(req["max_tool_calls_total"].asInt64() == 3);
    assert(req["max_steps"].asInt64() == 4);
    assert(req["timeout_ms"].asInt64() == 500);
  }
  {
    Json::Value req(Json::objectValue);
    req["max_steps"] = (Json::Int64)2;
    req["timeout_ms"] = (Json::Int64)200;
    WorkflowRunBudgetClampInput in;
    in.limits.max_steps_total = 20;
    in.limits.max_elapsed_ms_total = 3000;
    in.steps_remaining = 4;
    in.elapsed_ms_remaining = 500;
    assert(workflow_apply_run_budget_clamps(&req, in, nullptr));
    assert(req["max_steps"].asInt64() == 2);
    assert(req["timeout_ms"].asInt64() == 200);
  }
  {
    Json::Value req(Json::objectValue);
    WorkflowRunBudgetClampInput in;
    in.limits.max_total_tokens = 10;
    in.total_tokens_remaining = 0;
    std::string exceeded;
    assert(!workflow_apply_run_budget_clamps(&req, in, &exceeded));
    assert(exceeded == "max_total_tokens");
  }
  {
    Json::Value req(Json::objectValue);
    WorkflowRunBudgetClampInput in;
    in.limits.max_tool_calls_total = 1;
    in.tool_calls_remaining = 0;
    std::string exceeded;
    assert(!workflow_apply_run_budget_clamps(&req, in, &exceeded));
    assert(exceeded == "max_tool_calls_total");
  }

  return 0;
}
