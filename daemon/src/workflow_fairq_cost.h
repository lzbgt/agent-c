#pragma once

#include <cstdint>
#include <string>

namespace agentd {

// Best-effort DRR cost estimation for workflow fair-queue scheduling.
//
// Purpose:
// - When `--workflow-fair-queue-policy drr` is enabled, the scheduler can optionally charge
//   deficits by an estimated task cost (instead of unit-cost=1 per admitted task).
//
// Design constraints:
// - Deterministic: only depends on the task request JSON.
// - Safe fallback: parse failures return cost=1.
// - Bounded: returned cost is clamped to [1, max_cost].
//
// NOTE: This is not a billing system; it is a fairness heuristic.
int64_t workflow_fairq_estimate_task_cost_simple_v1(
  const std::string& workflow_task_request_json,
  int64_t max_cost = 32
);

}  // namespace agentd

