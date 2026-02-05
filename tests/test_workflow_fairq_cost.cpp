#include "workflow_fairq_cost.h"

#include <cassert>
#include <cstdint>
#include <string>

int main() {
  using agentd::workflow_fairq_estimate_task_cost_simple_v1;
  using agentd::workflow_fairq_estimate_task_cost_telemetry_v1;

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
  return 0;
#endif
}
