#include "llm_usage.h"
#include "workflow_run_budget_clamp.h"

#include <cassert>
#include <string>

namespace {

Json::Value usage_event(
  int64_t step,
  int64_t epoch,
  int64_t attempt,
  int64_t prompt,
  int64_t completion,
  int64_t total,
  bool estimated
) {
  Json::Value data(Json::objectValue);
  if (step >= 0) data["step"] = (Json::Int64)step;
  if (epoch >= 0) data["epoch"] = (Json::Int64)epoch;
  if (attempt >= 0) data["attempt"] = (Json::Int64)attempt;
  data["prompt_tokens"] = (Json::Int64)prompt;
  data["completion_tokens"] = (Json::Int64)completion;
  data["total_tokens"] = (Json::Int64)total;
  if (estimated) {
    data["estimated"] = true;
    data["source"] = "stream_fallback_missing_provider_usage";
  }
  Json::Value ev(Json::objectValue);
  ev["type"] = "llm_usage";
  ev["data"] = data;
  return ev;
}

}  // namespace

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
  {
    Json::Value events(Json::arrayValue);
    events.append(usage_event(1, 0, -1, 10, 5, 15, true));
    events.append(usage_event(1, 0, -1, 4, 3, 7, false));
    int64_t prompt = 0, completion = 0, total = 0;
    agentd::llm_sum_usage_from_events(events, &prompt, &completion, &total);
    assert(prompt == 4);
    assert(completion == 3);
    assert(total == 7);
  }
  {
    Json::Value events(Json::arrayValue);
    events.append(usage_event(1, 0, -1, 10, 5, 15, true));
    events.append(usage_event(2, 0, -1, 4, 3, 7, false));
    int64_t prompt = 0, completion = 0, total = 0;
    agentd::llm_sum_usage_from_events(events, &prompt, &completion, &total);
    assert(prompt == 14);
    assert(completion == 8);
    assert(total == 22);
  }
  {
    Json::Value events(Json::arrayValue);
    events.append(usage_event(-1, -1, 0, 8, 2, 10, true));
    events.append(usage_event(-1, -1, 0, 3, 1, 4, false));
    int64_t prompt = 0, completion = 0, total = 0;
    agentd::llm_sum_usage_from_events(events, &prompt, &completion, &total);
    assert(prompt == 3);
    assert(completion == 1);
    assert(total == 4);
  }

  return 0;
}
