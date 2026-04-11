#include "workflow_fairq_cost.h"

#include <cassert>
#include <cstdint>
#include <string>

int main() {
  using agentd::workflow_fairq_estimate_task_cost_simple_v1;
  using agentd::workflow_fairq_estimate_task_cost_telemetry_v1;
  using agentd::workflow_fairq_estimate_budget_pressure_cost_bump_v1;
  using agentd::WorkflowFairqBudgetPressure;

#if !defined(AGENT_HAVE_JSONCPP)
  return 77;
#else
  {
    const std::string req = R"({"prompt":"hi"})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    assert(c == 1);
  }
  {
    // LLM-like runs (model+prompt) should cost more than deterministic tasks.
    const std::string req = R"({"prompt":"hi","model":"stub"})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    assert(c >= 2 && c <= 32);
  }
  {
    // Prompt length bump should be bounded and deterministic.
    std::string longp(5000, 'x');
    const std::string req = std::string(R"({"prompt":")") + longp + R"(","model":"stub"})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    assert(c >= 4 && c <= 32);
  }
  {
    const std::string req = R"({"kind":"aggregate","aggregate":{"mode":"quorum_hashes"}})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    assert(c == 1);
  }
  {
    const std::string req = R"({"kind":"edge_invoke","edge":{"node_id":"n1","tool":"t","args":{}}})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    assert(c >= 8 && c <= 32);
  }
  {
    const std::string req = R"({
      "kind":"delegate",
      "delegate":{
        "stop_on_ok":false,
        "attempts":[
          {"id":"a","request":{"prompt":"1"}},
          {"id":"b","request":{"prompt":"2"}},
          {"id":"c","request":{"prompt":"3"}},
          {"id":"d","request":{"prompt":"4"}}
        ]
      }
    })";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    // Base: 1 + attempts (clamped >=2), so should be > 1.
    assert(c >= 5 && c <= 32);
  }
  {
    // Ensure clamping behaves.
    const std::string req = R"({"kind":"edge_invoke","max_tool_calls_total":999999})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 3);
    assert(c == 3);
  }
  {
    // Telemetry cost: empty/missing telemetry should return 0 (caller falls back).
    assert(workflow_fairq_estimate_task_cost_telemetry_v1("", 32) == 0);
    assert(workflow_fairq_estimate_task_cost_telemetry_v1("not json", 32) == 0);
    assert(workflow_fairq_estimate_task_cost_telemetry_v1("[]", 32) == 0);
  }
  {
    // Telemetry cost: derive a bounded cost from prior attempt result telemetry.
    const std::string last = R"({
      "ok": false,
      "retryable": true,
      "retry_in_ms": 100,
      "elapsed_ms": 1000,
      "total_tokens": 800,
      "tool_calls_total": 10,
      "steps_executed": 100
    })";
    // Expected:
    // base=1
    // + tokens/400 = 2
    // + elapsed/500 = 2
    // + tool_calls_total/5 = 2
    // + steps_executed/50 = 2
    // + retryable bump = 1 (+ fast poll bump = 1)
    // => 11
    const int64_t c = workflow_fairq_estimate_task_cost_telemetry_v1(last, 32);
    assert(c == 11);
  }
  {
    // Telemetry clamping behaves.
    const std::string last = R"({"retryable":true,"retry_in_ms":1,"elapsed_ms":999999,"total_tokens":999999})";
    const int64_t c = workflow_fairq_estimate_task_cost_telemetry_v1(last, 3);
    assert(c == 3);
  }
  {
    // Budget pressure: no configured workflow limits means no bump.
    WorkflowFairqBudgetPressure p;
    assert(workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 16) == 0);
  }
  {
    // Low-pressure budgets do not affect DRR charging.
    WorkflowFairqBudgetPressure p;
    p.max_total_tokens = 1000;
    p.total_tokens_used = 100;
    assert(workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 16) == 0);
  }
  {
    // Near-exhausted budgets add deterministic bounded bumps.
    WorkflowFairqBudgetPressure p;
    p.max_total_tokens = 1000;
    p.total_tokens_used = 900;
    const int64_t c = workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 16);
    assert(c == 6);
  }
  {
    // Exhausted or over-spent dimensions get the maximum per-dimension bump.
    WorkflowFairqBudgetPressure p;
    p.max_total_tokens = 1000;
    p.total_tokens_used = 5000;
    const int64_t c = workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 16);
    assert(c == 8);
  }
  {
    // Negative usage is normalized to 0 before pressure math.
    WorkflowFairqBudgetPressure p;
    p.max_total_tokens = 1000;
    p.total_tokens_used = -100;
    const int64_t c = workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 16);
    assert(c == 0);
  }
  {
    // Callers can disable pressure bumps through max_bump.
    WorkflowFairqBudgetPressure p;
    p.max_total_tokens = 1000;
    p.total_tokens_used = 999;
    const int64_t c = workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 0);
    assert(c == 0);
  }
  {
    // Multiple pressured dimensions compound, but the result is clamped.
    WorkflowFairqBudgetPressure p;
    p.max_tool_calls_total = 10;
    p.tool_calls_total_used = 10;
    p.max_steps_total = 100;
    p.steps_total_used = 90;
    p.max_total_tokens = 1000;
    p.total_tokens_used = 900;
    const int64_t c = workflow_fairq_estimate_budget_pressure_cost_bump_v1(p, 12);
    assert(c == 12);
  }
  return 0;
#endif
}
