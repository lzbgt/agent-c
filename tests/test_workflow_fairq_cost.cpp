#include "workflow_fairq_cost.h"

#include <cassert>
#include <cstdint>
#include <string>

int main() {
  using agentd::workflow_fairq_estimate_task_cost_simple_v1;

#if !defined(AGENT_HAVE_JSONCPP)
  return 77;
#else
  {
    const std::string req = R"({"prompt":"hi"})";
    const int64_t c = workflow_fairq_estimate_task_cost_simple_v1(req, 32);
    assert(c == 1);
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
  return 0;
#endif
}

