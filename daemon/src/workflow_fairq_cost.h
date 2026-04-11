#pragma once

#include <cstdint>
#include <string>

namespace agentd {

struct WorkflowFairqBudgetPressure {
  int64_t max_tool_calls_total = 0;
  int64_t tool_calls_total_used = 0;
  int64_t max_steps_total = 0;
  int64_t steps_total_used = 0;
  int64_t max_elapsed_ms_total = 0;
  int64_t elapsed_ms_total_used = 0;
  int64_t max_total_tokens = 0;
  int64_t total_tokens_used = 0;
};

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

// Best-effort budget-pressure bump for DRR cost charging (v2.4+).
//
// Input:
// - Workflow limits and retry-safe usage totals for the workflow containing the task being admitted.
//
// Output:
// - Returns 0 when no workflow budget is configured or no budget dimension is under pressure.
// - Otherwise returns a bounded positive cost bump. Near-exhausted budgets produce larger bumps.
//
// Rationale:
// - A workflow that has consumed most of a configured budget should not keep consuming equal DRR
//   deficit against a workflow with plenty of remaining budget. This is a fairness heuristic, not
//   a hard budget guard; hard budget enforcement remains in the workflow engine.
int64_t workflow_fairq_estimate_budget_pressure_cost_bump_v1(
  const WorkflowFairqBudgetPressure& pressure,
  int64_t max_bump = 16
);

}  // namespace agentd
