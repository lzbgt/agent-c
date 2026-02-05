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

// Best-effort telemetry-driven DRR cost estimation (v2.4+).
//
// Input:
// - `workflow_task_last_result_json`: the stored `workflow_tasks.result_json` from the previous attempt
//   (often present for polling-style deterministic tasks like edge_invoke / agentd_call).
//
// Output:
// - Returns 0 when telemetry is unavailable/invalid (caller should fall back to a request-based estimator).
// - Otherwise returns a bounded cost in [1, max_cost] derived from best-effort telemetry fields:
//   - elapsed_ms, total_tokens, tool_calls_total, steps_executed
//   - retryable + retry_in_ms (poll-loop hint)
//
// Rationale:
// - Some tasks have a heavy "first attempt" (submit/enqueue) but a cheap steady-state poll loop.
// - Using last-attempt telemetry allows charging poll attempts cheaply without weakening the initial admission guardrail.
int64_t workflow_fairq_estimate_task_cost_telemetry_v1(
  const std::string& workflow_task_last_result_json,
  int64_t max_cost = 32
);

}  // namespace agentd
